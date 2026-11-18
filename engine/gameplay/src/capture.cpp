// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/capture.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <mutex>

namespace victus::gameplay {

const std::vector<std::string_view>& key_names() {
    static const std::vector<std::string_view> names = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
        "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Escape", "Enter", "Tab", "Backspace",
        "Space", "Delete", "Insert", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down", "LeftShift",
        "RightShift", "LeftCtrl", "RightCtrl", "LeftAlt", "RightAlt", "LeftSuper", "RightSuper", "F1", "F2", "F3", "F4",
        "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Minus", "Equals", "LeftBracket", "RightBracket",
        "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma", "Period", "Slash"};
    return names;
}

InputState sample_input(const InputSource& in) {
    InputState s;
    for (const auto k : key_names())
        if (in.key_down(k) || in.key_pressed(k)) s.keys.emplace_back(k);
    for (int b = 0; b < 8; ++b)
        if (in.mouse_down(b) || in.mouse_pressed(b)) s.mouse |= static_cast<std::uint8_t>(1u << b);
    s.mouse_position = in.mouse_position();
    s.mouse_delta = in.mouse_delta();
    for (int p = 0; p < kMaxGamepads; ++p) {
        auto& pad = s.pads[static_cast<std::size_t>(p)];
        pad.connected = in.gamepad_connected(p);
        if (!pad.connected) continue;
        for (int b = 0; b < kGamepadButtonCount; ++b)
            if (in.gamepad_button(p, b)) pad.buttons |= 1u << b;
        for (int a = 0; a < kGamepadAxisCount; ++a) pad.axes[static_cast<std::size_t>(a)] = in.gamepad_axis(p, a);
    }
    return s;
}

bool ReplayInput::has(const InputState& s, std::string_view k) {
    return std::find(s.keys.begin(), s.keys.end(), k) != s.keys.end();
}

// ---- digest -------------------------------------------------------------------------------------

namespace {

long long round_to(float v, double scale) {
    const double r = std::round(static_cast<double>(v) * scale);
    return r == 0 ? 0 : static_cast<long long>(r); // no "-0"
}

std::string name_path(const scene::Scene& s, scene::Entity e) {
    std::string path;
    for (scene::Entity x = e; !x.is_null() && s.world().alive(x); x = s.parent(x)) {
        const auto* n = s.world().get<scene::Name>(x);
        path = (n ? n->value : std::string("?")) + (path.empty() ? "" : "/") + path;
    }
    return path;
}

struct ProbeRegistry {
    std::mutex mutex;
    std::vector<std::pair<std::string, StateProbe>> probes;
};

ProbeRegistry& probes() {
    static ProbeRegistry r;
    return r;
}

} // namespace

void register_state_probe(std::string name, StateProbe probe) {
    auto& r = probes();
    std::lock_guard lock(r.mutex);
    for (auto& [n, p] : r.probes)
        if (n == name) {
            p = std::move(probe);
            return;
        }
    r.probes.emplace_back(std::move(name), std::move(probe));
}

std::string probe_line(const scene::Scene& scene, scene::Entity e, std::string_view kind, std::string_view value) {
    return std::format("@{}|{}|{}", name_path(scene, e), kind, value);
}

std::string probe_number(double v) {
    if (!std::isfinite(v)) return v != v ? "nan" : (v > 0 ? "inf" : "-inf");
    const double r = std::round(v * 1e4);
    if (r == 0) return "0";
    return std::format("{}", r / 1e4);
}

std::string probe_vector(math::Vec3 v) {
    return std::format("({}, {}, {})", probe_number(v.x), probe_number(v.y), probe_number(v.z));
}

std::string probe_vector(math::Vec2 v) { return std::format("({}, {})", probe_number(v.x), probe_number(v.y)); }

std::vector<std::string> state_lines(const scene::Scene& scene, bool with_probes) {
    auto& world = const_cast<scene::Scene&>(scene).world(); // read-only iteration
    std::vector<std::string> lines;
    lines.reserve(world.alive_count() + 1);
    world.each<scene::Transform>([&](scene::Entity e, scene::Transform& t) {
        math::Quat q = t.rotation;
        if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
        std::string path = name_path(scene, e);
        if (with_probes && !path.empty() && (path.front() == '@' || path.front() == '\\')) path.insert(path.begin(), '\\');
        lines.push_back(std::format("{}|{} {} {}|{} {} {} {}|{} {} {}", path, round_to(t.position.x, 1e4),
                                    round_to(t.position.y, 1e4), round_to(t.position.z, 1e4), round_to(q.x, 1e4),
                                    round_to(q.y, 1e4), round_to(q.z, 1e4), round_to(q.w, 1e4), round_to(t.scale.x, 1e4),
                                    round_to(t.scale.y, 1e4), round_to(t.scale.z, 1e4)));
    });
    if (with_probes) {
        auto& r = probes();
        std::lock_guard lock(r.mutex);
        for (const auto& [n, p] : r.probes) p(scene, lines);
    }
    std::sort(lines.begin(), lines.end());
    lines.insert(lines.begin(), "#entities " + std::to_string(world.alive_count()));
    return lines;
}

std::string digest_of(const std::vector<std::string>& lines) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& l : lines) {
        h = fnv1a64(l, h);
        h = fnv1a64("\n", h);
    }
    return std::format("{:016x}", h);
}

std::string state_digest(const scene::Scene& scene, bool with_probes) { return digest_of(state_lines(scene, with_probes)); }

namespace {

struct ParsedLine {
    std::string path;
    std::string groups[3]; // position, rotation, scale ("1 2 3" in 1e-4 units)
};

ParsedLine parse_line(const std::string& l) {
    ParsedLine p;
    // The name path may contain '|'; the last three '|' separate the numeric groups.
    std::size_t cut[3];
    std::size_t pos = l.size();
    for (int i = 2; i >= 0; --i) {
        pos = l.rfind('|', pos == 0 ? 0 : pos - 1);
        if (pos == std::string::npos) return {l, {}};
        cut[i] = pos;
    }
    p.path = l.substr(0, cut[0]);
    p.groups[0] = l.substr(cut[0] + 1, cut[1] - cut[0] - 1);
    p.groups[1] = l.substr(cut[1] + 1, cut[2] - cut[1] - 1);
    p.groups[2] = l.substr(cut[2] + 1);
    return p;
}

std::string pretty(const std::string& group) {
    std::string out = "(";
    std::size_t start = 0;
    bool first = true;
    while (start < group.size()) {
        std::size_t end = group.find(' ', start);
        if (end == std::string::npos) end = group.size();
        long long v = 0;
        std::from_chars(group.data() + start, group.data() + end, v);
        out += (first ? "" : ", ") + std::format("{:g}", static_cast<double>(v) / 1e4);
        first = false;
        start = end + 1;
    }
    return out + ")";
}

} // namespace

std::string describe_difference(const std::vector<std::string>& expected, const std::vector<std::string>& actual,
                                std::size_t max_items) {
    std::vector<std::string> items;
    std::size_t more = 0;
    auto add = [&](std::string s) {
        if (items.size() < max_items) items.push_back(std::move(s));
        else ++more;
    };
    auto count_of = [](const std::vector<std::string>& v) {
        return !v.empty() && v.front().rfind("#entities ", 0) == 0 ? v.front().substr(10) : std::string("?");
    };
    if (count_of(expected) != count_of(actual)) add("entity count " + count_of(actual) + ", expected " + count_of(expected));
    // Group the lines by entity path (names may repeat).
    std::map<std::string, std::pair<std::vector<ParsedLine>, std::vector<ParsedLine>>> by_path;
    // Probe lines ("@path|kind|value"), grouped by path and kind.
    std::map<std::pair<std::string, std::string>, std::pair<std::vector<std::string>, std::vector<std::string>>> by_probe;
    auto sort_line = [&](const std::string& l, bool want) {
        if (l.rfind("#entities ", 0) == 0) return;
        if (!l.empty() && l.front() == '@') {
            const std::size_t v = l.rfind('|');
            const std::size_t k = v == std::string::npos || v == 0 ? std::string::npos : l.rfind('|', v - 1);
            if (k != std::string::npos && k > 0) {
                auto& slot = by_probe[{l.substr(1, k - 1), l.substr(k + 1, v - k - 1)}];
                (want ? slot.first : slot.second).push_back(l.substr(v + 1));
                return;
            }
        }
        auto p = parse_line(l);
        if (p.path.size() > 1 && p.path.front() == '\\') p.path.erase(p.path.begin());
        (want ? by_path[p.path].first : by_path[p.path].second).push_back(std::move(p));
    };
    for (const auto& l : expected) sort_line(l, true);
    for (const auto& l : actual) sort_line(l, false);
    std::vector<std::string> reported_paths;
    static const char* kWhat[3] = {"position", "rotation", "scale"};
    for (const auto& [path, pair] : by_path) {
        const auto& [want, got] = pair;
        if (want.size() != got.size()) {
            if (want.empty()) add(path + ": not in the recording");
            else if (got.empty()) add(path + ": missing");
            else add(std::format("{}: {} present, expected {}", path, got.size(), want.size()));
            reported_paths.push_back(path);
            continue;
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            for (int g = 0; g < 3; ++g) {
                if (want[i].groups[g] == got[i].groups[g]) continue;
                add(std::format("{}: {} {} expected {}", path, kWhat[g], pretty(got[i].groups[g]), pretty(want[i].groups[g])));
                break;
            }
        }
    }
    for (const auto& [key, pair] : by_probe) {
        const auto& [path, kind] = key;
        if (std::find(reported_paths.begin(), reported_paths.end(), path) != reported_paths.end()) continue;
        const auto& [want, got] = pair;
        if (want == got) continue;
        if (want.empty()) add(std::format("{}: {} {} not in the recording", path, kind, got.front()));
        else if (got.empty()) add(std::format("{}: {} missing, expected {}", path, kind, want.front()));
        else if (want.size() != got.size()) add(std::format("{}: {} {} values, expected {}", path, kind, got.size(), want.size()));
        else
            for (std::size_t i = 0; i < want.size(); ++i)
                if (want[i] != got[i]) {
                    add(std::format("{}: {} {} expected {}", path, kind, got[i], want[i]));
                    break;
                }
    }
    if (items.empty()) return "state differs";
    std::string out;
    for (const auto& it : items) out += (out.empty() ? "" : "; ") + it;
    if (more) out += std::format("; {} more", more);
    return out;
}

// ---- capture ------------------------------------------------------------------------------------

void Capture::add_frame(const InputState& input, float dt) {
    frames.push_back(input);
    frame_dt.push_back(dt);
}

void Capture::after_tick(const scene::Scene& s) {
    const auto n = static_cast<std::uint64_t>(frames.size());
    if (!digest_interval || n % digest_interval != 0) return;
    auto lines = state_lines(s, version >= 2);
    Digest d{n, digest_of(lines), {}};
    if (detail_limit && lines.size() <= detail_limit + 1) d.state = std::move(lines);
    digests.push_back(std::move(d));
}

void Capture::finish(const scene::Scene& s) {
    const auto n = static_cast<std::uint64_t>(frames.size());
    if (!digests.empty() && digests.back().frame == n) return;
    auto lines = state_lines(s, version >= 2);
    Digest d{n, digest_of(lines), {}};
    if (detail_limit && lines.size() <= detail_limit + 1) d.state = std::move(lines);
    digests.push_back(std::move(d));
}

namespace {

constexpr std::string_view kFormat = "victus.capture";

json::Value vec2(math::Vec2 v) {
    json::Value a = json::Value::array();
    a.as_array().push_back(v.x);
    a.as_array().push_back(v.y);
    return a;
}

Error bad(std::string what) { return make_error(ErrorCode::ParseError, "capture: " + std::move(what)); }

} // namespace

// Frames are stored as changes from the previous frame ("+": keys pressed, "-": released, "m":
// mouse buttons, "p"/"d": mouse position/delta, "pads", "dt"); unchanged frames are empty objects.
std::string Capture::to_text() const {
    json::Value doc = json::Value::object();
    doc.set("format", kFormat);
    doc.set("version", version);
    doc.set("project", project);
    doc.set("scene", scene);
    doc.set("seed", std::format("{:016x}", seed));
    doc.set("digest_interval", digest_interval);
    json::Value list = json::Value::array();
    InputState prev;
    float prev_dt = -1;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const InputState& f = frames[i];
        json::Value o = json::Value::object();
        const float dt = i < frame_dt.size() ? frame_dt[i] : 1.0f / 60.0f;
        if (dt != prev_dt) o.set("dt", dt);
        prev_dt = dt;
        json::Value down = json::Value::array(), up = json::Value::array();
        for (const auto& k : f.keys)
            if (std::find(prev.keys.begin(), prev.keys.end(), k) == prev.keys.end()) down.as_array().push_back(k);
        for (const auto& k : prev.keys)
            if (std::find(f.keys.begin(), f.keys.end(), k) == f.keys.end()) up.as_array().push_back(k);
        if (!down.as_array().empty()) o.set("+", down);
        if (!up.as_array().empty()) o.set("-", up);
        if (f.mouse != prev.mouse) o.set("m", static_cast<std::uint32_t>(f.mouse));
        if (!(f.mouse_position == prev.mouse_position)) o.set("p", vec2(f.mouse_position));
        if (!(f.mouse_delta == prev.mouse_delta)) o.set("d", vec2(f.mouse_delta));
        if (f.pads != prev.pads) {
            json::Value pads = json::Value::array();
            for (const auto& p : f.pads) {
                json::Value po = json::Value::object();
                if (p.connected) {
                    po.set("b", p.buttons);
                    json::Value axes = json::Value::array();
                    for (const float a : p.axes) axes.as_array().push_back(a);
                    po.set("a", axes);
                }
                pads.as_array().push_back(po);
            }
            o.set("pads", pads);
        }
        list.as_array().push_back(std::move(o));
        prev = f;
    }
    doc.set("frames", list);
    json::Value dg = json::Value::array();
    for (const auto& d : digests) {
        json::Value o = json::Value::object();
        o.set("frame", d.frame);
        o.set("digest", d.value);
        if (!d.state.empty()) {
            json::Value st = json::Value::array();
            for (const auto& l : d.state) st.as_array().push_back(l);
            o.set("state", st);
        }
        dg.as_array().push_back(o);
    }
    doc.set("digests", dg);
    return json::stringify(doc, false) + "\n";
}

Status Capture::save(const std::filesystem::path& path) const { return fs::write_text_atomic(path, to_text()); }

Result<Capture> Capture::load(const std::filesystem::path& path) {
    auto text = fs::read_text(path, 512ull << 20);
    if (!text) return std::move(text).error();
    return parse(*text);
}

Result<Capture> Capture::parse(std::string_view text) {
    json::ParseLimits limits;
    limits.max_bytes = 512ull << 20;
    auto doc = json::parse(text, limits);
    if (!doc) return bad(doc.error().message);
    if (doc->find("format") == nullptr || (*doc)["format"].as_string_or("") != kFormat) return bad("not a VICTUS capture");
    const auto version = (*doc)["version"].as_int(0);
    if (version < 1 || version > static_cast<std::int64_t>(kCaptureFormatVersion))
        return bad(std::format("format version {} is not supported (this build reads up to {})", version, kCaptureFormatVersion));
    Capture c;
    c.version = static_cast<std::uint32_t>(version);
    c.project = (*doc)["project"].as_string_or("");
    c.scene = (*doc)["scene"].as_string_or("");
    const std::string seed = (*doc)["seed"].as_string_or("0");
    if (std::from_chars(seed.data(), seed.data() + seed.size(), c.seed, 16).ec != std::errc{}) return bad("bad seed");
    c.digest_interval = static_cast<std::uint32_t>((*doc)["digest_interval"].as_int(60));
    const json::Value& frames = (*doc)["frames"];
    if (!frames.is_array()) return bad("missing frames");
    InputState cur;
    float dt = 1.0f / 60.0f;
    const auto& names = key_names();
    auto read_vec2 = [](const json::Value& v, math::Vec2 fallback) {
        if (!v.is_array() || v.as_array().size() != 2) return fallback;
        return math::Vec2{v.as_array()[0].as_float(0), v.as_array()[1].as_float(0)};
    };
    for (const auto& f : frames.as_array()) {
        if (!f.is_object()) return bad("frame is not an object");
        if (const auto* v = f.find("dt")) {
            dt = v->as_float(-1);
            if (!(dt > 0 && dt < 10)) return bad("bad frame time");
        }
        if (const auto* v = f.find("+"); v && v->is_array())
            for (const auto& k : v->as_array()) {
                const std::string name = k.as_string_or("");
                if (std::find(names.begin(), names.end(), name) == names.end()) return bad("unknown key '" + name + "'");
                if (std::find(cur.keys.begin(), cur.keys.end(), name) == cur.keys.end()) cur.keys.push_back(name);
            }
        if (const auto* v = f.find("-"); v && v->is_array())
            for (const auto& k : v->as_array()) std::erase(cur.keys, k.as_string_or(""));
        // Keep key_names() order so sampled and parsed states compare equal.
        std::sort(cur.keys.begin(), cur.keys.end(), [&](const std::string& a, const std::string& b) {
            return std::find(names.begin(), names.end(), a) < std::find(names.begin(), names.end(), b);
        });
        if (const auto* v = f.find("m")) cur.mouse = static_cast<std::uint8_t>(v->as_int(0) & 0xFF);
        if (const auto* v = f.find("p")) cur.mouse_position = read_vec2(*v, cur.mouse_position);
        if (const auto* v = f.find("d")) cur.mouse_delta = read_vec2(*v, cur.mouse_delta);
        if (const auto* v = f.find("pads"); v && v->is_array()) {
            for (std::size_t p = 0; p < cur.pads.size(); ++p) {
                auto& pad = cur.pads[p];
                pad = {};
                if (p >= v->as_array().size()) continue;
                const json::Value& po = v->as_array()[p];
                if (!po.find("b")) continue;
                pad.connected = true;
                pad.buttons = static_cast<std::uint32_t>(po["b"].as_int(0)) & ((1u << kGamepadButtonCount) - 1);
                if (const auto* a = po.find("a"); a && a->is_array())
                    for (std::size_t i = 0; i < pad.axes.size() && i < a->as_array().size(); ++i)
                        pad.axes[i] = std::clamp(a->as_array()[i].as_float(0), -1.0f, 1.0f);
            }
        }
        c.frames.push_back(cur);
        c.frame_dt.push_back(dt);
    }
    if (const auto* dg = doc->find("digests"); dg && dg->is_array())
        for (const auto& d : dg->as_array()) {
            Digest out{static_cast<std::uint64_t>(d["frame"].as_int(0)), d["digest"].as_string_or(""), {}};
            if (const auto* st = d.find("state"); st && st->is_array())
                for (const auto& l : st->as_array()) out.state.push_back(l.as_string_or(""));
            c.digests.push_back(std::move(out));
        }
    return c;
}

// ---- replay -------------------------------------------------------------------------------------

float CaptureReplay::begin_tick() {
    if (done()) return 1.0f / 60.0f;
    input_.set(capture_->frames[frame_]);
    return frame_ < capture_->frame_dt.size() ? capture_->frame_dt[frame_] : 1.0f / 60.0f;
}

bool CaptureReplay::end_tick(const scene::Scene& scene) {
    input_.end_frame();
    ++frame_;
    const auto& d = capture_->digests;
    while (next_digest_ < d.size() && d[next_digest_].frame < frame_) ++next_digest_;
    if (next_digest_ >= d.size() || d[next_digest_].frame != frame_) return true;
    const auto lines = state_lines(scene, capture_->version >= 2);
    const std::string got = digest_of(lines);
    ++checked_;
    const auto& want = d[next_digest_++];
    if (got == want.value) return true;
    if (mismatch_.empty())
        mismatch_ = want.state.empty() ? std::format("frame {}: expected digest {}, got {}", frame_, want.value, got)
                                       : std::format("frame {}: {}", frame_, describe_difference(want.state, lines));
    return false;
}

} // namespace victus::gameplay
