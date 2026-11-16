// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/capture.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

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
        if (in.key_down(k)) s.keys.emplace_back(k);
    for (int b = 0; b < 8; ++b)
        if (in.mouse_down(b)) s.mouse |= static_cast<std::uint8_t>(1u << b);
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

} // namespace

std::string state_digest(const scene::Scene& scene) {
    auto& world = const_cast<scene::Scene&>(scene).world(); // read-only iteration
    std::vector<std::string> lines;
    lines.reserve(world.alive_count());
    world.each<scene::Transform>([&](scene::Entity e, scene::Transform& t) {
        math::Quat q = t.rotation;
        if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
        lines.push_back(std::format("{}|{} {} {}|{} {} {} {}|{} {} {}", name_path(scene, e), round_to(t.position.x, 1e4),
                                    round_to(t.position.y, 1e4), round_to(t.position.z, 1e4), round_to(q.x, 1e4),
                                    round_to(q.y, 1e4), round_to(q.z, 1e4), round_to(q.w, 1e4), round_to(t.scale.x, 1e4),
                                    round_to(t.scale.y, 1e4), round_to(t.scale.z, 1e4)));
    });
    std::sort(lines.begin(), lines.end());
    std::uint64_t h = fnv1a64(std::to_string(world.alive_count()));
    for (const auto& l : lines) h = fnv1a64(l + "\n", h);
    return std::format("{:016x}", h);
}

// ---- capture ------------------------------------------------------------------------------------

void Capture::add_frame(const InputState& input, float dt) {
    frames.push_back(input);
    frame_dt.push_back(dt);
}

void Capture::after_tick(const scene::Scene& s) {
    const auto n = static_cast<std::uint64_t>(frames.size());
    if (digest_interval && n % digest_interval == 0) digests.push_back({n, state_digest(s)});
}

void Capture::finish(const scene::Scene& s) {
    const auto n = static_cast<std::uint64_t>(frames.size());
    if (digests.empty() || digests.back().frame != n) digests.push_back({n, state_digest(s)});
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
    doc.set("version", kCaptureFormatVersion);
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
        for (const auto& d : dg->as_array())
            c.digests.push_back({static_cast<std::uint64_t>(d["frame"].as_int(0)), d["digest"].as_string_or("")});
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
    const std::string got = state_digest(scene);
    ++checked_;
    const auto& want = d[next_digest_++].value;
    if (got == want) return true;
    if (mismatch_.empty()) mismatch_ = std::format("frame {}: expected {}, got {}", frame_, want, got);
    return false;
}

} // namespace victus::gameplay
