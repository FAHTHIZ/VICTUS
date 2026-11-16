// SPDX-License-Identifier: Apache-2.0
// Luau scripting: lifecycle, entity/scene/input API, sandboxing, budgets, errors, hot reload.
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <filesystem>
#include <set>

using namespace victus;
using scene::Entity;
using scene::Scene;
using scene::Transform;
using script::ScriptComponent;
using script::ScriptHost;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path / "scripts");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& text, int age_offset_s = 0) const {
        REQUIRE(fs::write_text_atomic(path / rel, text).ok());
        if (age_offset_s != 0) {
            std::filesystem::last_write_time(path / rel, std::filesystem::file_time_type::clock::now() +
                                                             std::chrono::seconds(age_offset_s));
        }
    }
};

struct FakeInput final : gameplay::InputSource {
    std::set<std::string, std::less<>> down, pressed;
    bool key_down(std::string_view k) const override { return down.contains(k); }
    bool key_pressed(std::string_view k) const override { return pressed.contains(k); }
    bool key_released(std::string_view) const override { return false; }
    bool mouse_down(int b) const override { return b == 0; }
    bool mouse_pressed(int) const override { return false; }
    math::Vec2 mouse_position() const override { return {12, 34}; }
    math::Vec2 mouse_delta() const override { return {1, -1}; }
};

std::unique_ptr<ScriptHost> make_host(const TempDir& d, double budget = 0.25) {
    script::HostOptions o;
    o.assets_root = d.path;
    o.call_budget_seconds = budget;
    auto h = ScriptHost::create(o);
    REQUIRE(h.ok());
    return std::move(h).value();
}

Entity scripted(Scene& s, const char* name, const char* path, json::Value props = json::Value::object()) {
    const Entity e = s.create(name);
    auto& c = s.world().add<ScriptComponent>(e);
    c.script = path;
    c.properties = std::move(props);
    return e;
}

} // namespace

TEST_CASE("script: lifecycle, properties and transforms") {
    TempDir d("victus_script_life");
    d.write("scripts/mover.luau", R"lua(
local Mover = {}
Mover.speed = 2
Mover.label = "default"
Mover.direction = vector.create(1, 0, 0)
function Mover:start()
    self.starts = (self.starts or 0) + 1
    self.ticks = 0
    self.fixed = 0
end
function Mover:update(dt)
    self.ticks += 1
    self.entity.position += self.direction * self.speed * dt
    self.entity:set("MeshRenderer", { primitive = "sphere" })
    self.entity.name = self.label .. ":" .. self.ticks
end
function Mover:fixed_update(dt)
    self.fixed += 1
end
return Mover
)lua");
    Scene s;
    script::register_components();
    const Entity a = scripted(s, "A", "scripts/mover.luau");
    json::Value props = json::Value::object();
    props.set("speed", 10);
    props.set("label", "fast");
    const Entity b = scripted(s, "B", "scripts/mover.luau", props);

    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    for (int i = 0; i < 10; ++i) sim.tick(s, 0.1f);

    const auto* ta = s.world().get<Transform>(a);
    const auto* tb = s.world().get<Transform>(b);
    CHECK(ta->position.x == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(tb->position.x == doctest::Approx(10.0f).epsilon(1e-4));
    CHECK(s.world().get<scene::Name>(a)->value == "default:10");
    CHECK(s.world().get<scene::Name>(b)->value == "fast:10");
    REQUIRE(s.world().get<scene::MeshRendererComponent>(a));
    CHECK(s.world().get<scene::MeshRendererComponent>(a)->primitive == "sphere");

    auto starts = host->eval("return 0", &s);
    REQUIRE(starts.ok());
    const auto st = host->stats();
    CHECK(st.modules == 1);
    CHECK(st.instances == 2);
    CHECK(st.errors == 0);
    // 10 ticks x 0.1 s at 50 Hz = 50 fixed steps. Instances start in the first Update phase, which
    // runs after that tick's 5 fixed steps, so each instance sees 45 fixed_update calls.
    // Calls = per instance (start + 10 updates + 45 fixed) + module chunk + eval chunk.
    CHECK(sim.fixed_steps() == 50);
    CHECK(st.calls == 2 * (1 + 10 + 45) + 2);

    // describe() lists editable fields, sorted.
    auto props_info = host->describe("scripts/mover.luau");
    REQUIRE(props_info.ok());
    REQUIRE(props_info->size() == 3);
    CHECK((*props_info)[0].name == "direction");
    CHECK((*props_info)[0].default_value.size() == 3);
    CHECK((*props_info)[1].name == "label");
    CHECK((*props_info)[2].name == "speed");
    CHECK((*props_info)[2].default_value.as_number() == 2);

    // Removing the component drops the instance; re-adding starts it again.
    s.world().remove<ScriptComponent>(a);
    sim.tick(s, 0.1f);
    CHECK(host->stats().instances == 1);
}

TEST_CASE("script: scene, entity and input API") {
    TempDir d("victus_script_api");
    d.write("scripts/api.luau", R"lua(
local M = {}
function M:start()
    local child = scene.spawn("Child", self.entity)
    child.position = vector.create(0, 1, 0)
    child:set("Light", { type = "point", intensity = 5 })
    assert(child.parent == self.entity, "parent")
    assert(self.entity:find_child("Child") == child, "find_child")
    assert(#self.entity:children() == 1, "children")
    assert(child:has("Light") and not child:has("MeshRenderer"), "has")
    assert(child:get("Light").intensity == 5, "get")
    local temp = scene.spawn("Temp")
    scene.destroy(temp)
    assert(not temp.valid, "destroyed entity is invalid")
    assert(scene.find("Temp") == nil, "find after destroy")
    assert(#scene.query("Light") == 1, "query")
    self.entity:rotate(vector.create(0, 1, 0), 90)
    assert(math.abs(self.entity.rotation.y - 90) < 0.01, "rotation")
    assert(tostring(self.entity) == "Entity(Root)", tostring(self.entity))
end
function M:update(dt)
    local x = input.axis("A", "D", "Left", "Right")
    self.entity:translate(vector.create(x, 0, 0))
    if input.pressed("Space") then self.jumps = (self.jumps or 0) + 1 end
    self.mouse = input.mouse_position()
    self.clicking = input.mouse_down(0)
end
return M
)lua");
    Scene s;
    const Entity root = scripted(s, "Root", "scripts/api.luau");
    auto host = make_host(d);
    FakeInput in;
    gameplay::Simulation sim;
    sim.input = &in;
    script::add_script_systems(sim, *host);

    in.down = {"D"};
    in.pressed = {"Space"};
    sim.tick(s, 1.0f / 60.0f);
    CHECK(host->stats().errors == 0);
    for (const auto& e : host->recent_errors()) MESSAGE(e);
    CHECK(s.world().get<Transform>(root)->position.x == doctest::Approx(1.0f));
    in.down = {"Left"};
    in.pressed.clear();
    sim.tick(s, 1.0f / 60.0f);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(s.world().get<Transform>(root)->position.x == doctest::Approx(-1.0f));
    const Entity child = s.find_by_name("Child");
    REQUIRE_FALSE(child.is_null());
    CHECK(s.parent(child) == root);
    REQUIRE(s.world().get<scene::LightComponent>(child));
    CHECK(s.world().get<scene::LightComponent>(child)->type == scene::LightComponent::Type::Point);
    CHECK(s.find_by_name("Temp").is_null());

    // Without input every query is false/zero rather than an error.
    sim.input = nullptr;
    sim.tick(s, 1.0f / 60.0f);
    CHECK(host->stats().errors == 0);
}

TEST_CASE("script: sandbox, budgets and error isolation") {
    TempDir d("victus_script_safety");
    d.write("scripts/bad_runtime.luau", R"lua(
local M = {}
function M:update(dt)
    self.n = (self.n or 0) + 1
    if self.n == 2 then error("boom") end
end
return M
)lua");
    d.write("scripts/bad_syntax.luau", "local M = {\nfunction M:update( end\nreturn M\n");
    d.write("scripts/not_table.luau", "return 42\n");
    d.write("scripts/forever.luau", "local M = {}\nfunction M:update() while true do end end\nreturn M\n");
    d.write("scripts/good.luau", "local M = {}\nfunction M:update(dt) self.n = (self.n or 0) + 1 end\nreturn M\n");
    d.write("scripts/self_destruct.luau", "local M = {}\nfunction M:update() self.entity:destroy() end\nreturn M\n");
    Scene s;
    scripted(s, "Runtime", "scripts/bad_runtime.luau");
    scripted(s, "Syntax", "scripts/bad_syntax.luau");
    scripted(s, "NotTable", "scripts/not_table.luau");
    scripted(s, "Forever", "scripts/forever.luau");
    scripted(s, "Missing", "scripts/missing.luau");
    scripted(s, "Escape", "../outside.luau");
    scripted(s, "Boom", "scripts/self_destruct.luau");
    const Entity good = scripted(s, "Good", "scripts/good.luau");
    auto host = make_host(d, 0.05);
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    for (int i = 0; i < 5; ++i) sim.tick(s, 1.0f / 60.0f);

    const auto& errs = host->recent_errors();
    auto has = [&](std::string_view needle) {
        return std::any_of(errs.begin(), errs.end(), [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    };
    CHECK(has("bad_runtime.luau:5: boom"));
    CHECK(has("bad_syntax.luau"));
    CHECK(has("must return a table"));
    CHECK(has("time budget"));
    CHECK(has("missing.luau"));
    CHECK(has("outside.luau"));
    // Faulted instances do not spam: one error each, despite 5 ticks.
    CHECK(std::count_if(errs.begin(), errs.end(), [](const std::string& e) { return e.find("boom") != std::string::npos; }) == 1);
    CHECK(std::count_if(errs.begin(), errs.end(), [](const std::string& e) { return e.find("time budget") != std::string::npos; }) == 1);
    CHECK(s.find_by_name("Boom").is_null());
    CHECK(s.world().alive(good));
    auto n = host->eval("return 1 + 1");
    REQUIRE(n.ok());
    CHECK(n->as_number() == 2);

    // Sandbox: dangerous libraries are absent, shared libraries are read-only.
    auto libs = host->eval("return { os = os == nil or os.execute == nil, io = io == nil, package = package == nil, "
                           "loadstring = loadstring == nil, dbg = debug == nil or debug.sethook == nil }");
    REQUIRE(libs.ok());
    CHECK(libs->find("io")->as_bool());
    CHECK(libs->find("package")->as_bool());
    CHECK(libs->find("os")->as_bool());
    CHECK(libs->find("dbg")->as_bool());
    CHECK_FALSE(host->eval("math.pi = 3").ok());
    CHECK_FALSE(host->eval("scene.find = nil").ok());
    // Globals written by one chunk are not visible to another.
    REQUIRE(host->eval("shared_value = 7").ok());
    auto other = host->eval("return shared_value == nil");
    REQUIRE(other.ok());
    CHECK(other->as_bool());

    CHECK(script::check_syntax("local x = 1", "ok.luau").ok());
    const auto bad = script::check_syntax("local = 1", "bad.luau");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("bad.luau:1") != std::string::npos);
}

TEST_CASE("script: memory limit") {
    TempDir d("victus_script_mem");
    script::HostOptions o;
    o.assets_root = d.path;
    o.memory_limit_bytes = 8u << 20;
    o.call_budget_seconds = 5.0;
    auto host = ScriptHost::create(o);
    REQUIRE(host.ok());
    auto r = (*host)->eval("local t = {} for i = 1, 1e8 do t[i] = tostring(i) end");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("memory") != std::string::npos);
    CHECK((*host)->stats().heap_bytes <= o.memory_limit_bytes);
    // The VM is still usable afterwards.
    auto ok = (*host)->eval("local t = {} for i = 1, 1000 do t[i] = i end return #t / 200");
    if (!ok.ok()) MESSAGE(ok.error().message);
    REQUIRE(ok.ok());
    CHECK(ok->as_number() == 5);
}

TEST_CASE("script: hot reload keeps instance state") {
    TempDir d("victus_script_reload");
    d.write("scripts/counter.luau", R"lua(
local M = {}
function M:update(dt) self.count = (self.count or 0) + 1 ; self.entity.position = vector.create(self.count, 0, 0) end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Counter", "scripts/counter.luau");
    auto host = make_host(d);
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    for (int i = 0; i < 3; ++i) sim.tick(s, 0.016f);
    CHECK(s.world().get<Transform>(e)->position.x == doctest::Approx(3));
    CHECK(host->reload_changed() == 0);

    // New version: counts by 10 and records that reload() ran; state (count) survives.
    d.write("scripts/counter.luau", R"lua(
local M = {}
function M:reload() self.reloaded = true end
function M:update(dt)
    self.count += 10
    self.entity.position = vector.create(self.count, self.reloaded and 1 or 0, 0)
end
return M
)lua", 5);
    CHECK(host->reload_changed() == 1);
    sim.tick(s, 0.016f);
    CHECK(s.world().get<Transform>(e)->position.x == doctest::Approx(13));
    CHECK(s.world().get<Transform>(e)->position.y == doctest::Approx(1));

    // A broken edit is reported and the previous version keeps running.
    d.write("scripts/counter.luau", "local M = {\n", 10);
    CHECK(host->reload_changed() == 0);
    sim.tick(s, 0.016f);
    CHECK(s.world().get<Transform>(e)->position.x == doctest::Approx(23));
    CHECK(host->stats().reloads == 1);
}

TEST_CASE("script: component serialization round trip") {
    script::register_components();
    Scene s;
    json::Value props = json::Value::object();
    props.set("speed", 3.5);
    props.set("tint", json::Value(json::Array{1, 0.5, 0}));
    const Entity e = scripted(s, "S", "scripts/x.luau", props);
    s.world().get<ScriptComponent>(e)->enabled = false;
    const json::Value doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const Entity e2 = t.find_by_name("S");
    const auto* c = t.world().get<ScriptComponent>(e2);
    REQUIRE(c);
    CHECK(c->script == "scripts/x.luau");
    CHECK_FALSE(c->enabled);
    CHECK(c->properties["speed"].as_number() == 3.5);
    CHECK(c->properties["tint"].size() == 3);
}
