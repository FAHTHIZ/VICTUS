// SPDX-License-Identifier: Apache-2.0
// Scene transitions: scene.load/reload/current/params, validation, checkpoints across scenes and
// the literal scene references export/check verify.
#include "victus/gameplay/gameplay.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace victus;
using scene::Entity;
using scene::Scene;

namespace {

struct Project {
    std::filesystem::path root;
    Project() {
        root = std::filesystem::temp_directory_path() / ("victus_scene_transition_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(root / "assets" / "scripts");
        std::filesystem::create_directories(root / "scenes");
    }
    ~Project() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        const auto p = root / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    // A scene file with one scripted entity (and a plain marker entity named after the scene).
    void scene(const std::string& rel, const char* entity, const char* script) const {
        Scene s;
        s.create(std::string("Marker ") + entity);
        const Entity e = s.create(entity);
        s.world().add<script::ScriptComponent>(e).script = script;
        REQUIRE(s.save_file(root / rel).ok());
    }
};

constexpr const char* kMenu = R"lua(
local M = {}
function M:start() self.frames = 0 end
function M:update(dt)
    self.frames += 1
    if self.frames == 2 then
        local ok, err = scene.load("scenes/missing.vscene")
        assert(ok == nil and string.find(err, "no scene"), "missing scene must fail")
        ok, err = scene.load("../outside.vscene")
        assert(ok == nil and err, "escaping paths must fail")
        assert(scene.current() == "scenes/menu.vscene", "current before load")
        assert(scene.params() == nil, "no params yet")
        assert(scene.load("scenes/level.vscene", { difficulty = "hard", lives = 3 }))
        -- Still the menu until this update ends.
        assert(scene.find("Marker Menu") ~= nil)
    end
end
return M
)lua";

constexpr const char* kLevel = R"lua(
local visits = 0 -- module-level state survives transitions
local L = {}
function L:start()
    visits += 1
    local p = scene.params()
    self.entity.name = string.format("Level %s %d visit %d", p and p.difficulty or "none", p and p.lives or 0, visits)
end
return L
)lua";

} // namespace

TEST_CASE("script: scene.load switches scenes at the end of the update with params") {
    script::register_components();
    Project p;
    p.write("assets/scripts/menu.luau", kMenu);
    p.write("assets/scripts/level.luau", kLevel);
    p.scene("scenes/menu.vscene", "Menu", "scripts/menu.luau");
    p.scene("scenes/level.vscene", "Level", "scripts/level.luau");

    std::vector<std::string> loaded;
    script::HostOptions o;
    o.assets_root = p.root / "assets";
    o.on_scene_loaded = [&](Scene&, std::string_view path) { loaded.emplace_back(path); };
    auto host = script::ScriptHost::create(o);
    REQUIRE(host.ok());
    (*host)->set_current_scene("scenes/menu.vscene");

    Scene s;
    REQUIRE(s.load_file(p.root / "scenes/menu.vscene").ok());
    gameplay::Simulation sim(1.0f / 60.0f);
    script::add_script_systems(sim, **host);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(loaded.empty());
    sim.tick(s, 1.0f / 60.0f);
    const std::string last_error = (*host)->recent_errors().empty() ? std::string() : (*host)->recent_errors().back();
    INFO(last_error);
    CHECK((*host)->stats().errors == 0);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0] == "scenes/level.vscene");
    CHECK((*host)->current_scene() == "scenes/level.vscene");
    CHECK((*host)->stats().scene_loads == 1);
    CHECK(s.find_by_name("Marker Menu").is_null());
    CHECK_FALSE(s.find_by_name("Marker Level").is_null());
    CHECK((*host)->stats().instances == 0); // the level's scripts start on the next update

    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(s.find_by_name("Level hard 3 visit 1").is_null());
    auto cur = (*host)->eval("return scene.current()", &s);
    REQUIRE(cur.ok());
    CHECK(cur->as_string() == "scenes/level.vscene");

    // reload keeps the path; new params replace the old ones; module state persists.
    REQUIRE((*host)->eval(R"lua(assert(scene.reload({ difficulty = "easy", lives = 9 })))lua", &s).ok());
    sim.tick(s, 1.0f / 60.0f); // applies the reload at the end of this update
    CHECK(loaded.size() == 2);
    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(s.find_by_name("Level easy 9 visit 2").is_null());
    CHECK((*host)->stats().scene_loads == 2);
    CHECK((*host)->stats().errors == 0);
}

TEST_CASE("script: load_scene from C++ validates first; checkpoints remember their scene") {
    script::register_components();
    Project p;
    p.write("assets/scripts/level.luau", kLevel);
    p.write("assets/scripts/menu.luau", "return {}");
    p.scene("scenes/menu.vscene", "Menu", "scripts/menu.luau");
    p.scene("scenes/level.vscene", "Level", "scripts/level.luau");
    p.write("scenes/broken.vscene", "{ not json");

    script::HostOptions o;
    o.assets_root = p.root / "assets";
    o.save_directory = p.root / "saves";
    auto host = script::ScriptHost::create(o);
    REQUIRE(host.ok());
    Scene s;
    REQUIRE(s.load_file(p.root / "scenes/menu.vscene").ok());
    (*host)->set_current_scene("scenes/menu.vscene");

    auto bad = (*host)->load_scene(s, "scenes/broken.vscene");
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(s.find_by_name("Marker Menu").is_null()); // untouched on failure
    CHECK((*host)->current_scene() == "scenes/menu.vscene");

    json::Value params = json::Value::object();
    params.set("difficulty", "normal");
    params.set("lives", 1);
    REQUIRE((*host)->load_scene(s, "scenes/level.vscene", params).ok());
    CHECK_FALSE(s.find_by_name("Marker Level").is_null());
    gameplay::Simulation sim(1.0f / 60.0f);
    script::add_script_systems(sim, **host);
    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(s.find_by_name("Level normal 1 visit 1").is_null());

    REQUIRE((*host)->checkpoint(s, "level", "in the level").ok());
    REQUIRE((*host)->load_scene(s, "scenes/menu.vscene").ok());
    CHECK((*host)->current_scene() == "scenes/menu.vscene");
    REQUIRE((*host)->restore(s, "level").ok());
    CHECK((*host)->current_scene() == "scenes/level.vscene");
    auto params_after = (*host)->eval("local p = scene.params(); return p and p.difficulty", &s);
    REQUIRE(params_after.ok());
    CHECK(params_after->as_string() == "normal");
    CHECK((*host)->stats().errors == 0);
}

TEST_CASE("script: scene_references lists literal scene.load targets") {
    const auto refs = script::scene_references(R"lua(
        scene.load("scenes/a.vscene")
        scene.load( 'scenes/b.vscene', { x = 1 })
        scene.load("scenes/a.vscene") -- duplicate
        myscene.load("scenes/not_this.vscene")
        scene.load(next_level)
        scene.loader("scenes/nope.vscene")
    )lua");
    REQUIRE(refs.size() == 2);
    CHECK(refs[0] == "scenes/a.vscene");
    CHECK(refs[1] == "scenes/b.vscene");
}
