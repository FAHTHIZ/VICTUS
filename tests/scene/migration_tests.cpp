// SPDX-License-Identifier: Apache-2.0
// Versioned scene/prefab data: component versions, renamed components, newer data kept,
// pre-migration copies (ADR-0100).
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/scene.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::scene;
using json::Value;

namespace {

struct Torch {
    float radius{1.0f}; // metres (version 3); version 1 stored "range" in centimetres
    bool lit{true};     // added in version 2 (default true)
};

void register_torch() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& reg = SerializerRegistry::instance();
    reg.add<Torch>(
        "Torch",
        [](const Torch& t) {
            Value v = Value::object();
            v.set("radius", t.radius);
            v.set("lit", t.lit);
            return v;
        },
        [](Torch& t, const Value& v) -> Status {
            if (v.find("range")) return make_error(ErrorCode::ParseError, "Torch: stale field range");
            if (v.find("$version")) return make_error(ErrorCode::ParseError, "Torch: $version reached the loader");
            t.radius = v["radius"].as_float(1.0f);
            t.lit = v["lit"].as_bool(true);
            return {};
        });
    REQUIRE(reg.set_version("Torch", 3,
                            {{1, "add lit",
                              [](Value& c) -> Status {
                                  c.set("lit", true);
                                  return {};
                              }},
                             {2, "range (cm) becomes radius (m)", [](Value& c) -> Status {
                                  if (!c["range"].is_number()) return make_error(ErrorCode::ParseError, "range must be a number");
                                  Value out = Value::object();
                                  for (const auto& [k, v] : c.as_object())
                                      if (k != "range") out.set(k, v);
                                  out.set("radius", c["range"].as_number() / 100.0);
                                  c = out;
                                  return {};
                              }}}));
    REQUIRE(reg.add_former_name("Torch", "Lantern"));
    CHECK_FALSE(reg.set_version("Torch", 3, {})); // incomplete chain rejected
    CHECK_FALSE(reg.set_version("NoSuchComponent", 1, {}));
}

Value scene_with(const char* key, Value component) {
    Value comps = Value::object();
    comps.set(key, std::move(component));
    Value e = Value::object();
    e.set("uuid", "6f1c2a4e-0000-4000-8000-000000000001");
    e.set("name", "T");
    e.set("components", comps);
    Value entities = Value::array();
    entities.push(e);
    Value doc = Value::object();
    doc.set("format", "victus.scene");
    doc.set("version", 1);
    doc.set("name", "Old");
    doc.set("entities", entities);
    return doc;
}

} // namespace

TEST_CASE("scene migration: old component data is upgraded, renamed and saved with its version") {
    register_torch();
    Value v1 = Value::object();
    v1.set("range", 250);
    Scene s;
    REQUIRE(s.load(scene_with("Lantern", v1)));
    const Entity e = s.find_by_name("T");
    const Torch* t = s.world().get<Torch>(e);
    REQUIRE(t);
    CHECK(t->radius == doctest::Approx(2.5f));
    CHECK(t->lit);
    REQUIRE(s.migrations().size() == 3);
    CHECK(s.migrations()[0] == "Lantern: renamed to Torch");
    CHECK(s.migrations()[2] == "Torch 2 -> 3: range (cm) becomes radius (m)");

    const Value saved = s.save_entity(e)["components"];
    CHECK(saved.find("Lantern") == nullptr);
    CHECK(saved["Torch"]["$version"].as_int() == 3);
    CHECK(saved["Torch"]["radius"].as_float() == doctest::Approx(2.5f));
    Scene again;
    REQUIRE(again.load(s.save()));
    CHECK(again.migrations().empty()); // current data: nothing to report
    CHECK(again.world().get<Torch>(again.find_by_name("T"))->radius == doctest::Approx(2.5f));

    // A failing step names the component and step and loads nothing.
    Value broken = Value::object();
    broken.set("range", "far");
    Scene b;
    auto st = b.load(scene_with("Torch", broken));
    REQUIRE_FALSE(st);
    CHECK(st.error().message.find("Torch migration from version 2") != std::string::npos);
    CHECK(b.size() == 0);
}

TEST_CASE("scene migration: data from a newer build is kept unread and round-trips unchanged") {
    register_torch();
    Value future = Value::object();
    future.set("$version", 9);
    future.set("intensity_curve", Value::array());
    Scene s;
    REQUIRE(s.load(scene_with("Torch", future)));
    const Entity e = s.find_by_name("T");
    CHECK(s.world().get<Torch>(e) == nullptr);
    REQUIRE(s.migrations().size() == 1);
    CHECK(s.migrations()[0].find("newer than this build") != std::string::npos);
    CHECK(s.save_entity(e)["components"]["Torch"] == future);

    Value doc = scene_with("Torch", future);
    doc.set("version", kSceneFormatVersion + 1);
    Scene n;
    auto st = n.load(doc);
    REQUIRE_FALSE(st);
    CHECK(st.error().code == ErrorCode::VersionMismatch);
}

TEST_CASE("scene migration: saving keeps the original once as .premigration; prefabs migrate too") {
    register_torch();
    const auto dir = std::filesystem::temp_directory_path() / ("victus_mig_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(dir);
    const auto path = dir / "old.vscene";
    Value v1 = Value::object();
    v1.set("range", 100);
    const std::string original = json::stringify(scene_with("Torch", v1), true);
    REQUIRE(fs::write_text_atomic(path, original));
    Scene s;
    REQUIRE(s.load_file(path));
    REQUIRE_FALSE(s.migrations().empty());
    REQUIRE(s.save_file(path));
    CHECK(fs::read_text(fs::premigration_path(path)).value() == original);
    REQUIRE(s.save_file(path)); // a second save does not replace the kept original
    CHECK(fs::read_text(fs::premigration_path(path)).value() == original);
    Scene reread;
    REQUIRE(reread.load_file(path));
    CHECK(reread.migrations().empty());

    Value pdoc = scene_with("Lantern", v1);
    pdoc.set("format", "victus.prefab");
    auto prefab = parse_prefab(pdoc);
    REQUIRE(prefab);
    CHECK(prefab->migrations.size() == 3);
    CHECK(prefab->entities[0]["components"]["Torch"]["radius"].as_float() == doctest::Approx(1.0f));
    CHECK(prefab->entities[0]["components"].find("Lantern") == nullptr);
    const auto ppath = dir / "old.vprefab";
    REQUIRE(fs::write_text_atomic(ppath, json::stringify(pdoc, true)));
    auto loaded = load_prefab_file(ppath);
    REQUIRE(loaded);
    REQUIRE(save_prefab_file(*loaded, ppath));
    CHECK(std::filesystem::exists(fs::premigration_path(ppath)));
    CHECK(load_prefab_file(ppath)->migrations.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
