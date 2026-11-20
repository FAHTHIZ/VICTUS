// SPDX-License-Identifier: Apache-2.0
// Deterministic mutation fuzzing of every loader that reads untrusted bytes (project files a
// user downloads, cooked caches, packs, saves, captures, audio clips, network configuration).
//
// Each target starts from valid documents produced by the engine's own writers (or shipped
// sample assets), applies random byte-level and JSON-structure mutations and feeds the result
// to the parser. A target passes when every input either loads or fails with a non-empty error
// message; a crash, hang, sanitizer report or uncaught exception fails the suite. The CI
// sanitizer lanes run this binary under ASan + UBSan.
//
// Environment:
//   VICTUS_FUZZ_ITERATIONS  mutations per target (default 300)
//   VICTUS_FUZZ_SEED        extra seed mixed into every target's generator (default 0)
//   VICTUS_FUZZ_TRACE=1     write every input to <temp>/victus_fuzz_failures/last-<target>.bin
//                           before running it (to recover the input behind a crash)
// A failing input is written to <temp>/victus_fuzz_failures/<target>-<iteration>.bin.
#include "victus/gameui/localization.hpp"
#include "victus/ai/ai.hpp"
#include "victus/animation/controller.hpp"
#include "victus/animation/sprite.hpp"
#include "victus/assets/cooked.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/material.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/pack.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/gameplay/input_map.hpp"
#include "victus/gameplay/save.hpp"
#include "victus/graph/graph.hpp"
#include "victus/graph/material_graph.hpp"
#include "victus/net/net.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/scene.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace stdfs = std::filesystem;
using namespace victus;

namespace {

using Bytes = std::vector<std::uint8_t>;

// splitmix64: tiny, deterministic on every platform.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
};

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any threads start
    if (v == nullptr || *v == '\0') return fallback;
    return std::strtoull(v, nullptr, 10);
}

std::size_t iterations() { return static_cast<std::size_t>(env_u64("VICTUS_FUZZ_ITERATIONS", 300)); }

Rng rng_for(std::string_view target) {
    return Rng{fnv1a64(target) ^ (env_u64("VICTUS_FUZZ_SEED", 0) * 0x2545F4914F6CDD1Dull)};
}

Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
std::string_view as_text(const Bytes& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

Bytes read_file(const stdfs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void write_file(const stdfs::path& p, const Bytes& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

stdfs::path work_dir() {
    stdfs::path dir = stdfs::temp_directory_path() / "victus_fuzz_work";
    stdfs::create_directories(dir);
    return dir;
}

stdfs::path source_path(std::string_view relative) { return stdfs::path(VICTUS_SOURCE_DIR) / std::string(relative); }

// ---- mutators -----------------------------------------------------------------------------

constexpr std::uint32_t kInterestingU32[] = {0u,          1u,          0x7Fu,       0x80u,      0xFFu,
                                             0x100u,      0xFFFFu,     0x10000u,    0x7FFFFFFFu, 0x80000000u,
                                             0xFFFFFFFEu, 0xFFFFFFFFu, 0x00010000u, 4096u,       64u};

void mutate_bytes(Bytes& b, Rng& rng) {
    const std::size_t ops = 1 + rng.below(4);
    for (std::size_t op = 0; op < ops; ++op) {
        const std::size_t kind = rng.below(9);
        const std::size_t at = rng.below(b.size() + 1);
        switch (kind) {
        case 0: // bit flip
            if (!b.empty()) b[rng.below(b.size())] ^= static_cast<std::uint8_t>(1u << rng.below(8));
            break;
        case 1: // random byte
            if (!b.empty()) b[rng.below(b.size())] = static_cast<std::uint8_t>(rng.next());
            break;
        case 2: { // interesting little-endian u32
            if (b.size() < 4) break;
            const std::uint32_t v = kInterestingU32[rng.below(std::size(kInterestingU32))];
            const std::size_t p = rng.below(b.size() - 3);
            for (int i = 0; i < 4; ++i) b[p + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
            break;
        }
        case 3: // insert bytes
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), 1 + rng.below(8), static_cast<std::uint8_t>(rng.next()));
            break;
        case 4: { // delete a range
            if (b.empty()) break;
            const std::size_t p = rng.below(b.size());
            const std::size_t n = 1 + rng.below(std::min<std::size_t>(64, b.size() - p));
            b.erase(b.begin() + static_cast<std::ptrdiff_t>(p), b.begin() + static_cast<std::ptrdiff_t>(p + n));
            break;
        }
        case 5: // truncate
            b.resize(at);
            break;
        case 6: { // duplicate a range
            if (b.empty()) break;
            const std::size_t p = rng.below(b.size());
            const std::size_t n = 1 + rng.below(std::min<std::size_t>(256, b.size() - p));
            const Bytes chunk(b.begin() + static_cast<std::ptrdiff_t>(p), b.begin() + static_cast<std::ptrdiff_t>(p + n));
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
            break;
        }
        case 7: { // text tokens that stress JSON/number parsers
            static constexpr std::string_view kTokens[] = {"\"", "\\", "{", "}", "[", "]", ",", ":", "-", "1e309",
                                                           "-1e309", "4294967296", "-1", "null", "\\u0000", "\\ud800",
                                                           "0.0000001", "99999999999999999999", "[[[[[[[[", "\xff\xfe"};
            const std::string_view t = kTokens[rng.below(std::size(kTokens))];
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), t.begin(), t.end());
            break;
        }
        default: // swap two bytes
            if (b.size() >= 2) std::swap(b[rng.below(b.size())], b[rng.below(b.size())]);
            break;
        }
    }
}

void collect(json::Value& v, std::vector<json::Value*>& out) {
    out.push_back(&v);
    if (v.is_array()) {
        for (json::Value& c : v.as_array()) collect(c, out);
    } else if (v.is_object()) {
        for (auto& [k, c] : v.as_object()) collect(c, out);
    }
}

json::Value interesting_value(Rng& rng) {
    static constexpr double kNumbers[] = {0.0,           -1.0,          1.0,  0.5,   -0.5,  255.0, 256.0, 65535.0,
                                          65536.0,       2147483647.0,  2147483648.0, -2147483648.0, 4294967295.0,
                                          4294967296.0,  1e9,           1e18, 9.3e18, -9.3e18, 1e300, -1e300, 1e-300};
    static constexpr std::string_view kStrings[] = {"",       "x",          "../../../etc/passwd", "/abs/path", "C:\\x",
                                                    "\\\\unc", "%s%s%n",    "nan",                 "-1",        "0",
                                                    "a/b/../../c", "assets/", "http://example.com/x", "\x01\x02"};
    switch (rng.below(7)) {
    case 0:
    case 1: return json::Value(kNumbers[rng.below(std::size(kNumbers))]);
    case 2: return json::Value(std::string(kStrings[rng.below(std::size(kStrings))]));
    case 3: return json::Value(rng.chance(50));
    case 4: return json::Value(nullptr);
    case 5: return json::Value::array();
    default: return json::Value::object();
    }
}

void mutate_json(json::Value& doc, Rng& rng) {
    const std::size_t ops = 1 + rng.below(3);
    for (std::size_t op = 0; op < ops; ++op) {
        std::vector<json::Value*> nodes;
        collect(doc, nodes);
        json::Value& v = *nodes[rng.below(nodes.size())];
        switch (rng.below(8)) {
        case 0:
        case 1: v = interesting_value(rng); break;
        case 2: // nudge a number
            if (v.is_number()) {
                const double n = v.as_number();
                static constexpr double kFactors[] = {-1.0, 2.0, 1000.0, 1e6, 0.0};
                v = json::Value(rng.chance(50) ? n + (rng.chance(50) ? 1.0 : -1.0) : n * kFactors[rng.below(5)]);
            } else {
                v = interesting_value(rng);
            }
            break;
        case 3: // remove a child
            if (v.is_array() && !v.as_array().empty()) {
                auto& a = v.as_array();
                a.erase(a.begin() + static_cast<std::ptrdiff_t>(rng.below(a.size())));
            } else if (v.is_object() && !v.as_object().empty()) {
                auto& o = v.as_object();
                o.erase(o.begin() + static_cast<std::ptrdiff_t>(rng.below(o.size())));
            }
            break;
        case 4: // duplicate a child (objects get a duplicate key)
            if (v.is_array() && !v.as_array().empty()) {
                auto& a = v.as_array();
                json::Value copy = a[rng.below(a.size())];
                a.insert(a.begin() + static_cast<std::ptrdiff_t>(rng.below(a.size() + 1)), std::move(copy));
            } else if (v.is_object() && !v.as_object().empty()) {
                auto& o = v.as_object();
                auto copy = o[rng.below(o.size())];
                copy.second = interesting_value(rng);
                o.push_back(std::move(copy));
            }
            break;
        case 5: { // replace with a copy of another node (type confusion between fields)
            // Deep copy: json::Value shares storage on copy, so assigning an ancestor to one of
            // its descendants would otherwise create a reference cycle.
            auto copy = json::parse(json::stringify(*nodes[rng.below(nodes.size())], false));
            if (copy.ok()) v = std::move(*copy);
            break;
        }
        case 6: // swap two array elements (order assumptions)
            if (v.is_array() && v.as_array().size() >= 2) {
                auto& a = v.as_array();
                std::swap(a[rng.below(a.size())], a[rng.below(a.size())]);
            }
            break;
        default: // a large array of numbers (allocation and bounds checks)
            if (rng.chance(25)) {
                json::Array a(1 + rng.below(4096), json::Value(static_cast<double>(rng.below(70000)) - 1.0));
                v = json::Value(std::move(a));
            }
            break;
        }
    }
}

// ---- harness ------------------------------------------------------------------------------

// Returns an empty string when the input was handled correctly (loaded, or rejected with a
// message), otherwise a description of the problem.
using TargetFn = std::function<std::string(const Bytes&)>;

template <typename R>
std::string verdict(const R& r) {
    if (r.ok()) return {};
    return r.error().message.empty() ? std::string("error without a message") : std::string{};
}

struct Outcome {
    std::size_t accepted{0};
    std::size_t rejected{0};
};

void record_failure(std::string_view target, std::size_t i, const Bytes& input, const std::string& why) {
    const stdfs::path dir = stdfs::temp_directory_path() / "victus_fuzz_failures";
    stdfs::create_directories(dir);
    const stdfs::path file = dir / (std::string(target) + "-" + std::to_string(i) + ".bin");
    write_file(file, input);
    FAIL_CHECK(target << " iteration " << i << ": " << why << " (input saved to " << file.string() << ")");
}

void trace(std::string_view target, const Bytes& input) {
    static const bool enabled = env_u64("VICTUS_FUZZ_TRACE", 0) != 0;
    if (!enabled) return;
    const stdfs::path dir = stdfs::temp_directory_path() / "victus_fuzz_failures";
    stdfs::create_directories(dir);
    write_file(dir / ("last-" + std::string(target) + ".bin"), input);
}

// Mutates raw bytes of the seeds. `accepted` reports whether the target loaded the input.
void fuzz_bytes(std::string_view target, const std::vector<Bytes>& seeds,
                const std::function<std::string(const Bytes&, bool&)>& fn) {
    REQUIRE_FALSE(seeds.empty());
    Rng rng = rng_for(target);
    Outcome out;
    // The unmodified seeds must load: otherwise the corpus is not exercising the parser.
    for (const Bytes& s : seeds) {
        bool ok = false;
        const std::string why = fn(s, ok);
        CHECK_MESSAGE(why.empty(), target << " seed: " << why);
        CHECK_MESSAGE(ok, target << ": a seed document failed to load");
    }
    const std::size_t n = iterations();
    for (std::size_t i = 0; i < n; ++i) {
        Bytes input = seeds[rng.below(seeds.size())];
        mutate_bytes(input, rng);
        trace(target, input);
        bool ok = false;
        const std::string why = fn(input, ok);
        if (!why.empty()) record_failure(target, i, input, why);
        (ok ? out.accepted : out.rejected) += 1;
    }
    MESSAGE(target << " (bytes): " << out.accepted << " accepted, " << out.rejected << " rejected");
}

// Mutates the JSON structure of the seeds (well-formed documents with wrong values).
void fuzz_json(std::string_view target, const std::vector<json::Value>& seeds,
               const std::function<std::string(const json::Value&, bool&)>& fn) {
    REQUIRE_FALSE(seeds.empty());
    Rng rng = rng_for(std::string(target) + "/json");
    Outcome out;
    for (const json::Value& s : seeds) {
        bool ok = false;
        const std::string why = fn(s, ok);
        CHECK_MESSAGE(why.empty(), target << " seed: " << why);
        CHECK_MESSAGE(ok, target << ": a seed document failed to load");
    }
    const std::size_t n = iterations();
    for (std::size_t i = 0; i < n; ++i) {
        json::Value doc = seeds[rng.below(seeds.size())];
        mutate_json(doc, rng);
        trace(std::string(target) + "-json", to_bytes(json::stringify(doc, false)));
        bool ok = false;
        const std::string why = fn(doc, ok);
        if (!why.empty()) record_failure(std::string(target) + "-json", i, to_bytes(json::stringify(doc, false)), why);
        (ok ? out.accepted : out.rejected) += 1;
    }
    MESSAGE(target << " (json): " << out.accepted << " accepted, " << out.rejected << " rejected");
}

// Wraps a JSON target so the byte mutator can drive it through json::parse.
std::function<std::string(const Bytes&, bool&)> via_text(const std::function<std::string(const json::Value&, bool&)>& fn) {
    return [fn](const Bytes& b, bool& ok) -> std::string {
        auto doc = json::parse(as_text(b));
        if (!doc.ok()) return verdict(doc);
        return fn(*doc, ok);
    };
}

std::vector<Bytes> texts(const std::vector<json::Value>& docs) {
    std::vector<Bytes> out;
    for (const auto& d : docs) out.push_back(to_bytes(json::stringify(d, true)));
    return out;
}

json::Value parse_or_fail(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? std::string{} : doc.error().message));
    return *doc;
}

json::Value load_json_file(std::string_view relative) {
    const Bytes b = read_file(source_path(relative));
    REQUIRE_MESSAGE(!b.empty(), "missing seed " << relative);
    return parse_or_fail(as_text(b));
}

// ---- seeds --------------------------------------------------------------------------------

std::vector<json::Value> scene_seeds() {
    project::register_components();
    std::vector<json::Value> out;
    for (const auto& t : project::templates()) {
        scene::Scene s;
        if (project::build_template_scene(t.id, s).ok()) out.push_back(s.save());
    }
    out.push_back(load_json_file("samples/arena/scenes/main.vscene"));
    out.push_back(load_json_file("samples/dungeon_crawler/scenes/main.vscene"));
    return out;
}

std::string text_of_template_file(std::string_view suffix) {
    for (const auto& t : project::templates())
        for (const auto& f : project::template_files(t.id))
            if (f.path.size() >= suffix.size() && f.path.compare(f.path.size() - suffix.size(), suffix.size(), suffix) == 0)
                return f.content;
    return {};
}

constexpr std::string_view kSequence = R"({
  "duration": 2,
  "tracks": [
    { "type": "transform", "target": "Door",
      "position": [ { "t": 0, "v": [0, 0, 0] }, { "t": 2, "v": [0, 4, 0] } ] },
    { "type": "event", "keys": [ { "t": 0, "name": "start" }, { "t": 1, "name": "half", "payload": "p" },
                                 { "t": 2, "name": "end" } ] } ] })";

render::Image test_image(std::uint32_t w, std::uint32_t h) {
    render::Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<std::uint8_t>(i * 37u);
    return img;
}

} // namespace

// ---- targets ------------------------------------------------------------------------------

TEST_CASE("fuzz: json::parse") {
    std::vector<Bytes> seeds = texts(scene_seeds());
    seeds.push_back(to_bytes(kSequence));
    fuzz_bytes("json", seeds, [](const Bytes& b, bool& ok) {
        json::ParseLimits tight;
        tight.max_depth = 16;
        tight.max_string_bytes = 256;
        const auto small = json::parse(as_text(b), tight);
        if (std::string why = verdict(small); !why.empty()) return why;
        const auto r = json::parse(as_text(b));
        ok = r.ok();
        if (ok) { // what parses must survive a stringify round trip
            const auto again = json::parse(json::stringify(*r, false));
            if (!again.ok()) return std::string("stringify output does not parse: ") + again.error().message;
        }
        return verdict(r);
    });
}

TEST_CASE("fuzz: scene::Scene::load") {
    const auto seeds = scene_seeds();
    REQUIRE(seeds.size() >= 2);
    const auto target = [](const json::Value& doc, bool& ok) {
        scene::Scene s;
        const Status st = s.load(doc);
        ok = st.ok();
        if (ok) {
            s.update_transforms();
            (void)s.save(); // a loaded scene must be saveable
        }
        return verdict(st);
    };
    fuzz_json("scene", seeds, target);
    fuzz_bytes("scene", texts(seeds), via_text(target));
}

TEST_CASE("fuzz: prefabs") {
    project::register_components();
    const std::vector<json::Value> seeds = {load_json_file("samples/arena/assets/prefabs/enemy_robot.vprefab"),
                                            load_json_file("samples/arena/assets/prefabs/health_orb.vprefab")};
    const auto target = [](const json::Value& doc, bool& ok) {
        auto p = scene::parse_prefab(doc);
        ok = p.ok();
        if (ok) {
            scene::Scene s;
            auto e = scene::instantiate(s, *p, Uuid::generate());
            if (std::string why = verdict(e); !why.empty()) return why;
            (void)scene::prefab_to_json(*p);
        }
        return verdict(p);
    };
    fuzz_json("prefab", seeds, target);
    fuzz_bytes("prefab", texts(seeds), via_text(target));
}

TEST_CASE("fuzz: materials, tilesets and sprite sheets") {
    const std::vector<json::Value> materials = {load_json_file("samples/arena/assets/materials/ground.vmat"),
                                                load_json_file("samples/arena/assets/materials/health_orb.vmat"),
                                                load_json_file("samples/arena/assets/materials/gate_glow.vmat")};
    fuzz_json("material", materials, [](const json::Value& doc, bool& ok) {
        auto m = assets::MaterialAsset::from_json(doc);
        ok = m.ok();
        if (ok) (void)m->to_json();
        return verdict(m);
    });

    const std::vector<json::Value> tilesets = {load_json_file("samples/dungeon_crawler/assets/tilesets/dungeon.vtileset")};
    const auto tileset = [](const json::Value& doc, bool& ok) {
        auto t = tilemap::parse_tileset(doc);
        ok = t.ok();
        if (ok) (void)tilemap::to_json(*t);
        return verdict(t);
    };
    fuzz_json("tileset", tilesets, tileset);
    fuzz_bytes("tileset", texts(tilesets), via_text(tileset));

    const std::vector<json::Value> sheets = {parse_or_fail(
        R"({"format":"victus.spritesheet","version":1,"texture":"00000000-0000-4000-8000-000000000001",)"
        R"("columns":4,"rows":2,"clips":{"idle":{"frames":[0,1,2,1],"fps":6},"run":{"frames":[4,5,6,7],"fps":12}}})")};
    fuzz_json("spritesheet", sheets, [](const json::Value& doc, bool& ok) {
        auto s = animation::parse_sprite_sheet(doc);
        ok = s.ok();
        return verdict(s);
    });
}

TEST_CASE("fuzz: graphs, behaviour trees and sequences") {
    const std::vector<json::Value> graphs = {graph::graph_to_json(graph::starter_material_graph())};
    const auto graph_target = [](const json::Value& doc, bool& ok) {
        auto g = graph::graph_from_json(doc);
        ok = g.ok();
        if (ok) {
            (void)graph::compile_graph(*g);
            (void)graph::compile_material_graph(*g, "fuzz");
            (void)graph::serialize_graph(*g);
        }
        return verdict(g);
    };
    fuzz_json("graph", graphs, graph_target);
    fuzz_bytes("graph", texts(graphs), [graph_target](const Bytes& b, bool& ok) {
        auto g = graph::parse_graph(as_text(b));
        if (!g.ok()) return verdict(g);
        return graph_target(graph::graph_to_json(*g), ok);
    });

    const std::string tree = text_of_template_file(".vbt");
    REQUIRE_FALSE(tree.empty());
    const std::vector<json::Value> trees = {parse_or_fail(tree)};
    fuzz_json("behaviour_tree", trees, [](const json::Value& doc, bool& ok) {
        auto t = ai::BehaviorTree::from_json(doc, "fuzz");
        ok = t.ok();
        return verdict(t);
    });
    fuzz_bytes("behaviour_tree", {to_bytes(tree)}, [](const Bytes& b, bool& ok) {
        auto t = ai::BehaviorTree::parse(as_text(b), "fuzz");
        ok = t.ok();
        return verdict(t);
    });

    const std::vector<json::Value> sequences = {parse_or_fail(kSequence)};
    fuzz_json("sequence", sequences, [](const json::Value& doc, bool& ok) {
        auto s = sequencer::Sequence::from_json(doc, "fuzz");
        ok = s.ok();
        return verdict(s);
    });
    fuzz_bytes("sequence", {to_bytes(kSequence)}, [](const Bytes& b, bool& ok) {
        auto s = sequencer::Sequence::parse(as_text(b), "fuzz");
        ok = s.ok();
        return verdict(s);
    });
}

TEST_CASE("fuzz: animation controllers") {
    constexpr const char* kController = R"({"version": 1,
      "parameters": [{"name": "speed"}, {"name": "x"}, {"name": "jump", "type": "trigger"}, {"name": "g", "type": "bool"}],
      "default_state": "Move",
      "states": [
        {"name": "Move", "blend1d": {"parameter": "speed", "points": [{"clip": "Idle", "at": 0}, {"clip": "Run", "at": 4, "speed": 1.2}]}},
        {"name": "Strafe", "blend2d": {"x": "x", "y": "speed", "points": [{"clip": "Idle", "at": [0, 0]}, {"clip": "Left", "at": [-1, 0]}, {"clip": "Run", "at": [0, 1]}]},
         "speed_parameter": "speed"},
        {"name": "Jump", "clip": "Jump", "loop": false}],
      "transitions": [
        {"from": "*", "to": "Jump", "duration": 0.1, "conditions": [{"parameter": "jump"}]},
        {"from": "Jump", "to": "Move", "exit_time": 0.9, "conditions": [{"parameter": "g", "op": "true"}]},
        {"from": "Move", "to": "Strafe", "conditions": [{"parameter": "x", "op": "!=", "value": 0}]}]})";
    const std::vector<json::Value> docs = {parse_or_fail(kController)};
    fuzz_json("anim_controller", docs, [](const json::Value& doc, bool& ok) {
        auto c = animation::Controller::from_json(doc, "fuzz");
        ok = c.ok();
        if (ok) { // accepted documents must round-trip
            auto again = animation::Controller::from_json((*c)->to_json(), "fuzz");
            REQUIRE(again.ok());
            CHECK((*again)->states == (*c)->states);
        }
        return verdict(c);
    });
    fuzz_bytes("anim_controller", {to_bytes(kController)}, [](const Bytes& b, bool& ok) {
        auto c = animation::Controller::parse(as_text(b), "fuzz");
        ok = c.ok();
        return verdict(c);
    });
}

TEST_CASE("fuzz: localization string tables") {
    std::vector<Bytes> seeds;
    for (const auto& f : project::template_files("3d"))
        if (f.path.ends_with(".vstrings")) seeds.push_back(to_bytes(f.content));
    REQUIRE(seeds.size() == 1);
    seeds.push_back(to_bytes(R"({"name":"Ελληνικά","strings":{"a":{"b":{"c":"{x} {{y}} Ж"}},"d.e":"f"}})"));
    for (const Bytes& b : seeds) REQUIRE(gameui::StringTable::parse(as_text(b)).ok());
    fuzz_bytes("string_table", seeds, [](const Bytes& b, bool& ok) {
        auto t = gameui::StringTable::parse(as_text(b));
        ok = t.ok();
        if (ok)
            if (const std::string* v = t->find("d.e"))
                (void)gameui::format_text(*v, [](std::string_view n) { return std::optional<std::string>(std::string(n)); });
        return verdict(t);
    });
}

TEST_CASE("fuzz: input maps, saves and captures") {
    gameplay::InputMap map;
    map.add("move_x", {"D", "Right", "pad:left_x"}, {"A", "Left"});
    map.add("jump", {"Space", "pad:a"});
    map.add("fire", {"mouse:left"}, {}, 0.3f);
    const std::vector<json::Value> maps = {map.to_json()};
    REQUIRE(gameplay::InputMap::from_json(maps[0]).ok());
    fuzz_json("input_map", maps, [](const json::Value& doc, bool& ok) {
        auto m = gameplay::InputMap::from_json(doc);
        ok = m.ok();
        if (ok) (void)m->validate([](std::string_view) { return true; });
        return verdict(m);
    });

    json::Value data = json::Value::object();
    data.set("level", 3);
    data.set("name", "hero");
    data.set("inventory", json::Value(json::Array{json::Value("sword"), json::Value(2)}));
    const std::string save = json::stringify(gameplay::make_save_document(data, 2, 1700000000, "Slot 1"), true);
    fuzz_bytes("save", {to_bytes(save)}, [](const Bytes& b, bool& ok) {
        auto s = gameplay::parse_save_document(as_text(b));
        ok = s.ok();
        return verdict(s);
    });

    gameplay::Capture capture;
    capture.project = "fuzz";
    capture.scene = "scenes/main.vscene";
    capture.seed = 42;
    for (int i = 0; i < 8; ++i) {
        gameplay::InputState in;
        if (i % 2 == 0) in.keys.emplace_back("Space");
        in.mouse = static_cast<std::uint8_t>(i);
        in.mouse_position = {static_cast<float>(i), 2.0f};
        in.pads[0].connected = true;
        in.pads[0].buttons = static_cast<std::uint32_t>(i);
        in.pads[0].axes[0] = 0.25f * static_cast<float>(i % 4);
        capture.add_frame(in, 1.0f / 60.0f);
    }
    capture.digests.push_back({8, "0123456789abcdef", {"Player: position (0, 0, 0)"}});
    fuzz_bytes("capture", {to_bytes(capture.to_text())}, [](const Bytes& b, bool& ok) {
        auto c = gameplay::Capture::parse(as_text(b));
        ok = c.ok();
        if (ok) (void)c->to_text();
        return verdict(c);
    });
}

TEST_CASE("fuzz: cooked textures and models") {
    assets::CookedHeader th;
    th.kind = assets::AssetKind::Texture;
    th.key = 7;
    th.dependencies.push_back({"textures/a.png", 99});
    const assets::CookedTexture tex = assets::cook_texture(test_image(8, 4), "fuzz", true, true, 64);
    const Bytes tex_bytes = assets::serialize(th, tex);

    assets::CookedModel model;
    model.materials.push_back({});
    model.meshes.push_back({"cube", render::make_cube(), 0});
    assets::ModelNode node;
    node.name = "root";
    node.meshes = {0};
    model.nodes.push_back(node);
    assets::CookedHeader mh;
    mh.kind = assets::AssetKind::Model;
    const Bytes cube_bytes = assets::serialize(mh, model);

    std::vector<Bytes> model_seeds = {cube_bytes};
    assets::ModelImportSettings settings;
    settings.optimize_meshes = false;
    auto robot = assets::import_gltf(source_path("tests/fixtures/robot.gltf"), settings,
                                     source_path("samples/arena/assets"));
    REQUIRE_MESSAGE(robot.ok(), (robot.ok() ? std::string{} : robot.error().message));
    model_seeds.push_back(assets::serialize(mh, *robot)); // skins, nodes and animation clips

    fuzz_bytes("cooked_texture", {tex_bytes}, [](const Bytes& b, bool& ok) {
        if (std::string why = verdict(assets::read_header(b)); !why.empty()) return why;
        auto t = assets::deserialize_texture(b);
        ok = t.ok();
        return verdict(t);
    });
    fuzz_bytes("cooked_model", model_seeds, [](const Bytes& b, bool& ok) {
        if (std::string why = verdict(assets::read_header(b)); !why.empty()) return why;
        auto m = assets::deserialize_model(b);
        ok = m.ok();
        if (ok) {
            if (std::string why = verdict(assets::validate_model(*m)); !why.empty()) return why;
            if (!assets::validate_model(*m).ok()) return std::string("deserialize_model accepted an invalid model");
            (void)assets::make_animation_set(*m);
        }
        return verdict(m);
    });
}

TEST_CASE("fuzz: glTF import") {
    const json::Value robot = load_json_file("tests/fixtures/robot.gltf");
    const stdfs::path dir = work_dir() / "gltf";
    stdfs::create_directories(dir);
    const stdfs::path file = dir / "model.gltf";
    assets::ModelImportSettings settings;
    settings.optimize_meshes = false; // keeps iterations fast; the optimiser only sees validated meshes
    settings.import_textures = false;
    const auto import_bytes = [&](const Bytes& b, bool& ok) {
        write_file(file, b);
        auto m = assets::import_gltf(file, settings, dir);
        ok = m.ok();
        if (ok && !assets::validate_model(*m).ok()) return std::string("import_gltf returned an invalid model");
        return verdict(m);
    };
    // Structure mutations reach accessor/buffer-view/skin/animation validation; byte
    // mutations mostly exercise the JSON tokenizer and base64 decoding.
    fuzz_json("gltf", {robot}, [&](const json::Value& doc, bool& ok) { return import_bytes(to_bytes(json::stringify(doc, false)), ok); });
    fuzz_bytes("gltf", {to_bytes(json::stringify(robot, false))}, import_bytes);
}

TEST_CASE("fuzz: terrain heightfields") {
    auto flat = terrain::make_flat(33, 1.5f);
    REQUIRE(flat.ok());
    fuzz_bytes("terrain", {terrain::serialize(*flat)}, [](const Bytes& b, bool& ok) {
        auto h = terrain::deserialize(b);
        ok = h.ok();
        if (ok) (void)h->sample(0.5f, 0.5f);
        return verdict(h);
    });
}

TEST_CASE("fuzz: image decoding") {
    std::vector<Bytes> seeds;
    auto png = render::encode_png(test_image(16, 8));
    REQUIRE(png.ok());
    seeds.push_back(*png);
    seeds.push_back(read_file(source_path("samples/dungeon_crawler/assets/textures/dungeon_tiles.png")));
    if (seeds.back().empty()) seeds.pop_back();
    render::ImageF hdr;
    hdr.width = 8;
    hdr.height = 4;
    hdr.pixels.assign(8u * 4u * 4u, 0.75f);
    auto hdr_bytes = render::encode_hdr(hdr);
    REQUIRE(hdr_bytes.ok());
    fuzz_bytes("image", seeds, [](const Bytes& b, bool& ok) {
        auto img = render::decode_image(b);
        ok = img.ok();
        if (ok && img->pixels.size() != static_cast<std::size_t>(img->width) * img->height * 4)
            return std::string("decoded pixel buffer does not match its size");
        return verdict(img);
    });
    fuzz_bytes("image_hdr", {*hdr_bytes}, [](const Bytes& b, bool& ok) {
        auto img = render::decode_image_hdr(b);
        ok = img.ok();
        if (ok && img->pixels.size() != static_cast<std::size_t>(img->width) * img->height * 4)
            return std::string("decoded pixel buffer does not match its size");
        return verdict(img);
    });
}

TEST_CASE("fuzz: audio clip decoding") {
    // Seeds: engine-written WAVs (mono and stereo), the shipped sample WAVs, and the first bytes of
    // miniaudio's Ogg Vorbis and FLAC test files (truncated so each iteration stays fast; a
    // truncated stream must still decode what it has).
    std::vector<Bytes> seeds;
    std::vector<float> tone(480 * 2);
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f);
    seeds.push_back(audio::encode_wav(tone.data(), 960, 1, 48000));
    seeds.push_back(audio::encode_wav(tone.data(), 480, 2, 22050));
    for (const char* sample : {"samples/arena/assets/audio/impact.wav", "samples/dungeon_crawler/assets/audio/coin.wav"}) {
        Bytes b = read_file(source_path(sample));
        if (!b.empty()) seeds.push_back(std::move(b));
    }
    const stdfs::path data = VICTUS_MINIAUDIO_DATA_DIR;
    for (const auto& [name, keep] : {std::pair{"48000-stereo.ogg", 24u * 1024u}, std::pair{"16-44100-stereo.flac", 48u * 1024u}}) {
        Bytes b = read_file(data / name);
        if (b.empty()) continue;
        if (b.size() > keep) b.resize(keep);
        seeds.push_back(std::move(b));
    }
    constexpr std::uint64_t kMaxFrames = 48000ull * 20;
    fuzz_bytes("audio_clip", seeds, [](const Bytes& b, bool& ok) {
        auto info = audio::inspect_clip(b, kMaxFrames);
        ok = info.ok();
        if (ok && (info->channels == 0 || info->sample_rate == 0 || info->frames == 0))
            return std::string("accepted a clip with no channels, rate or samples");
        if (!ok && info.error().code != ErrorCode::ParseError && info.error().code != ErrorCode::LimitExceeded)
            return std::string("unexpected error code");
        return verdict(info);
    });
}

TEST_CASE("fuzz: pack archives") {
    const stdfs::path dir = work_dir() / "pack";
    stdfs::create_directories(dir);
    pack::PackWriter writer;
    REQUIRE(writer.add_text("project.vproject", R"({"name":"fuzz"})").ok());
    REQUIRE(writer.add_text("scenes/main.vscene", "{}").ok());
    REQUIRE(writer.add("textures/a.bin", Bytes(300, 0xAB)).ok());
    const stdfs::path seed_path = dir / "seed.vpak";
    REQUIRE(writer.write(seed_path).ok());
    const stdfs::path file = dir / "mutated.vpak";
    fuzz_bytes("pack", {read_file(seed_path)}, [&](const Bytes& b, bool& ok) {
        write_file(file, b);
        auto pack = pack::Pack::open(file);
        ok = pack.ok();
        if (!ok) return verdict(pack);
        for (const std::string& path : (*pack)->list()) {
            auto data = (*pack)->read(path, 1u << 20);
            if (std::string why = verdict(data); !why.empty()) return why;
        }
        return std::string{};
    });
}

TEST_CASE("fuzz: network configuration strings") {
    const std::vector<Bytes> addresses = {to_bytes("127.0.0.1:7777"), to_bytes("example.org"), to_bytes("localhost:1")};
    fuzz_bytes("net_address", addresses, [](const Bytes& b, bool& ok) {
        auto a = net::parse_address(as_text(b));
        ok = a.ok();
        return verdict(a);
    });
    const std::vector<Bytes> conditions = {to_bytes("latency=80,jitter=10,loss=0.05")};
    fuzz_bytes("net_conditions", conditions, [](const Bytes& b, bool& ok) {
        auto c = net::parse_conditions(as_text(b));
        ok = c.ok();
        return verdict(c);
    });
}
