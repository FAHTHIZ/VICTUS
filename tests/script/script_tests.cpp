// SPDX-License-Identifier: Apache-2.0
// Luau scripting: lifecycle, entity/scene/input API, sandboxing, budgets, errors, hot reload.
#include "victus/ai/ai.hpp"
#include "victus/animation/animation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"

#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <set>
#include <vector>

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

TEST_CASE("script: input actions, gamepads and rebinding") {
    TempDir d("victus_script_actions");
    d.write("scripts/actions.luau", R"lua(
local M = {}
function M:start()
    local names = input.actions()
    assert(#names >= 10, "default actions")
    local b = input.bindings("move_x")
    assert(b[1] == "D" and b[#b] == "-Pad.DpadLeft", table.concat(b, ","))
    assert(not pcall(input.action, "no_such_action"), "unknown action raises")
    local ok, err = input.rebind("jump", { "J", "Pad.North" })
    assert(ok, err)
    ok, err = input.rebind("bad", { "Pad.Nope" })
    assert(not ok and string.find(err, "Pad.Nope"), "bad rebind reports")
    assert(input.rebind("throttle", { "Pad.RightTrigger", "-Pad.LeftTrigger" }))
    self.jumps = 0
end
function M:update(dt)
    local v = input.vector("move_x", "move_y")
    if input.action_pressed("jump") then self.jumps += 1 end
    local pad = input.gamepad_connected() and 1 or 0
    -- Report through the transform: x/y = move vector, z = jumps; scale.x = throttle,
    -- scale.y = gamepad connected, scale.z = raw South button.
    self.entity.position = vector.create(v.x, v.y, self.jumps)
    self.entity.scale = vector.create(input.value("throttle"), pad, input.gamepad_button("A") and 1 or 0)
end
return M
)lua");
    Scene s;
    const Entity root = scripted(s, "Root", "scripts/actions.luau");
    auto host = make_host(d);
    gameplay::ManualInput in;
    gameplay::Simulation sim;
    sim.input = &in;
    script::add_script_systems(sim, *host);
    const auto step = [&] {
        sim.tick(s, 1.0f / 60.0f);
        in.end_frame();
    };
    const auto pos = [&] { return s.world().get<Transform>(root)->position; };
    const auto scl = [&] { return s.world().get<Transform>(root)->scale; };

    step();
    for (const auto& e : host->recent_errors()) MESSAGE(e);
    REQUIRE(host->stats().errors == 0);
    CHECK(scl().y == 0.0f);

    // Diagonal keys are clamped to unit length.
    in.set_key("D", true);
    in.set_key("W", true);
    step();
    CHECK(pos().x == doctest::Approx(0.7071f).epsilon(0.01));
    CHECK(pos().y == doctest::Approx(0.7071f).epsilon(0.01));
    in.set_key("D", false);
    in.set_key("W", false);

    // Left stick half-left and pushed up (SDL +Y is down).
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("LeftX"), -0.6f);
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("LeftY"), -0.2f);
    step();
    CHECK(pos().x == doctest::Approx(-0.5f));
    CHECK(pos().y == doctest::Approx(0.0f)); // within the dead zone
    CHECK(scl().y == 1.0f);

    // Rebound jump: Space no longer jumps, J and the North button do; edges count once.
    in.set_key("Space", true);
    step();
    CHECK(pos().z == 0.0f);
    in.set_key("J", true);
    step();
    step();
    CHECK(pos().z == 1.0f);
    in.set_key("J", false);
    in.set_gamepad_button(0, gameplay::gamepad_button_index("North"), true);
    step();
    CHECK(pos().z == 1.0f); // J then North with no gap is one continuous hold
    in.set_gamepad_button(0, gameplay::gamepad_button_index("North"), false);
    step();
    in.set_gamepad_button(0, gameplay::gamepad_button_index("North"), true);
    step();
    CHECK(pos().z == 2.0f);

    // Negative trigger binding and raw button query.
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("LeftTrigger"), 1.0f);
    in.set_gamepad_button(0, gameplay::gamepad_button_index("South"), true);
    step();
    CHECK(scl().x == doctest::Approx(-1.0f));
    CHECK(scl().z == 1.0f);
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

TEST_CASE("script: physics queries, velocities and collision/trigger callbacks") {
    TempDir d("victus_script_physics");
    // Callbacks record into the Name of a "Probe" entity so the test can observe them.
    d.write("scripts/ball.luau", R"lua(
local Ball = {}
local probe = nil
local function note(text)
    probe.name = probe.name .. text .. ";"
end
function Ball:start()
    probe = scene.find("Probe")
end
function Ball:update(dt)
    if self.entity.position.y < 3 and not self.kicked then
        self.kicked = true
        self.entity:add_impulse(vector.create(0, 0, 0))
        self.entity.velocity = self.entity.velocity + vector.create(0, -1, 0)
    end
end
function Ball:on_collision_enter(other, info)
    note("hit:" .. other.name .. ":" .. (info.normal.y < -0.5 and "down" or "other"))
end
function Ball:on_trigger_enter(other) note("enter:" .. other.name) end
function Ball:on_trigger_exit(other) note("exit:" .. other.name) end
return Ball
)lua");
    auto host = make_host(d);
    auto pw = physics::PhysicsWorld::create();
    REQUIRE(pw.ok());
    auto& world = *pw.value();
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    physics::add_physics_systems(sim, world);
    Scene s;
    const Entity probe_e = s.create("Probe");
    const Entity ground = s.create("Ground");
    s.world().get<Transform>(ground)->position = {0, -0.5f, 0};
    physics::ColliderComponent gc;
    gc.size = {20, 1, 20};
    s.world().add<physics::ColliderComponent>(ground, gc);
    const Entity zone = s.create("Zone");
    s.world().get<Transform>(zone)->position = {0, 2, 0};
    physics::ColliderComponent zc;
    zc.size = {3, 0.3f, 3};
    zc.is_trigger = true;
    s.world().add<physics::ColliderComponent>(zone, zc);
    const Entity ball = scripted(s, "Ball", "scripts/ball.luau");
    s.world().get<Transform>(ball)->position = {0, 4, 0};
    physics::ColliderComponent bc;
    bc.shape = physics::ColliderComponent::Shape::Sphere;
    bc.radius = 0.5f;
    s.world().add<physics::ColliderComponent>(ball, bc);
    s.world().add<physics::RigidBodyComponent>(ball, physics::RigidBodyComponent{});
    for (int i = 0; i < 150; ++i) sim.tick(s, 1.0f / 60);
    CHECK(world.has_body(ball));
    CHECK(s.world().get<Transform>(ball)->position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(s.world().get<scene::Name>(probe_e)->value == "Probeenter:Zone;exit:Zone;hit:Ground:down;");
    auto q = host->eval(R"lua(
local hit = physics.raycast(vector.create(0, 10, 0), vector.create(0, -1, 0), 100)
local zone = physics.overlap_box(vector.create(0, 2, 0), vector.create(1, 0.5, 1))
local b = scene.find("Ball")
return { hit = hit.entity.name, n = #zone, available = physics.available(), g = physics.gravity().y,
         vy = b.velocity.y }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["hit"].as_string() == "Ball");
    CHECK(q.value()["n"].as_number() == 0); // triggers are excluded from queries
    CHECK(q.value()["available"].as_bool());
    CHECK(q.value()["g"].as_number() == doctest::Approx(-9.81));
    CHECK(std::abs(q.value()["vy"].as_number()) < 0.1);
}

TEST_CASE("script: character controller and joint motor") {
    TempDir d("victus_script_character");
    d.write("scripts/walker.luau", R"lua(
local Walker = {}
function Walker:update(dt)
    self.t = (self.t or 0) + dt
    self.entity:move_character(vector.create(2, 0, 0))
    if self.t > 0.5 and self.entity.grounded and not self.jumped then
        self.jumped = self.entity:jump(4)
        self.entity.name = "Walker:jumped"
    end
    local door = scene.find("Door")
    if self.t > 0.2 and not self.motor then
        self.motor = true
        door:set_motor(true, 90)
    end
end
return Walker
)lua");
    auto host = make_host(d);
    auto pw = physics::PhysicsWorld::create();
    REQUIRE(pw.ok());
    auto& world = *pw.value();
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    physics::add_physics_systems(sim, world);
    Scene s;
    const Entity ground = s.create("Ground");
    s.world().get<Transform>(ground)->position = {0, -0.5f, 0};
    physics::ColliderComponent gc;
    gc.size = {40, 1, 40};
    s.world().add<physics::ColliderComponent>(ground, gc);
    const Entity door = s.create("Door");
    s.world().get<Transform>(door)->position = {0.5f, 1, 5};
    physics::ColliderComponent dc;
    dc.size = {1, 2, 0.1f};
    s.world().add<physics::ColliderComponent>(door, dc);
    s.world().add<physics::RigidBodyComponent>(door, physics::RigidBodyComponent{});
    physics::JointComponent j;
    j.anchor = {-0.5f, 0, 0};
    s.world().add<physics::JointComponent>(door, j);
    const Entity walker = scripted(s, "Walker", "scripts/walker.luau");
    s.world().add<physics::CharacterControllerComponent>(walker, physics::CharacterControllerComponent{});
    for (int i = 0; i < 120; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(walker)->value == "Walker:jumped");
    CHECK(s.world().get<Transform>(walker)->position.x == doctest::Approx(4.0f).epsilon(0.1));
    CHECK(s.world().get<physics::JointComponent>(door)->motor);
    auto q = host->eval(R"lua(
local w = scene.find("Walker:jumped")
local cs = w:character_state()
local js = scene.find("Door"):joint_state()
return { valid = cs.valid, grounded = cs.grounded, ny = cs.ground_normal.y, ground = cs.ground and cs.ground.name or "",
         active = js.active, angle = js.angle, jvalid = js.valid }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["valid"].as_bool());
    CHECK(q.value()["grounded"].as_bool());
    CHECK(q.value()["ground"].as_string() == "Ground");
    CHECK(q.value()["ny"].as_number() == doctest::Approx(1.0).epsilon(0.01));
    CHECK(q.value()["jvalid"].as_bool());
    CHECK(q.value()["active"].as_bool());
    CHECK(std::abs(q.value()["angle"].as_number()) > 60.0); // ~90 deg/s for ~1.7 s, wraps at 180
}

TEST_CASE("script: 2D character, one-way drop-through and 2D joint motor") {
    TempDir d("victus_script_character2d");
    d.write("scripts/runner.luau", R"lua(
local Runner = {}
function Runner:update(dt)
    self.t = (self.t or 0) + dt
    if self.t > 0.5 and not self.dropped and self.entity.grounded then
        self.dropped = self.entity:drop_through()
        self.entity.name = "Runner:dropped"
    end
    if self.t > 0.2 and not self.motor then
        self.motor = true
        scene.find("Wheel"):set_motor(true, 180)
    end
end
return Runner
)lua");
    auto host = make_host(d);
    auto pw = physics::PhysicsWorld::create();
    REQUIRE(pw.ok());
    auto& world = *pw.value();
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    physics::add_physics_systems(sim, world);
    Scene s;
    const auto box = [&](const char* name, math::Vec2 p, math::Vec2 size, bool one_way) {
        const Entity e = s.create(name);
        s.world().get<Transform>(e)->position = {p.x, p.y, 0};
        physics::Collider2DComponent c;
        c.size = size;
        c.one_way = one_way;
        s.world().add<physics::Collider2DComponent>(e, c);
        return e;
    };
    box("Floor", {0, -0.5f}, {40, 1}, false);
    box("Ledge", {0, 2}, {4, 0.2f}, true);
    const Entity wheel = box("Wheel", {10, 3}, {1, 1}, false);
    s.world().add<physics::RigidBody2DComponent>(wheel, physics::RigidBody2DComponent{});
    s.world().add<physics::Joint2DComponent>(wheel, physics::Joint2DComponent{});
    const Entity runner = scripted(s, "Runner", "scripts/runner.luau");
    s.world().get<Transform>(runner)->position = {0, 2.7f, 0};
    s.world().add<physics::CharacterController2DComponent>(runner, physics::CharacterController2DComponent{});
    for (int i = 0; i < 150; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(runner)->value == "Runner:dropped");
    CHECK(s.world().get<Transform>(runner)->position.y == doctest::Approx(0.5f).epsilon(0.05)); // on the floor
    CHECK(s.world().get<physics::Joint2DComponent>(wheel)->motor);
    auto q = host->eval(R"lua(
local r = scene.find("Runner:dropped")
local cs = r:character_state()
local js = scene.find("Wheel"):joint_state()
return { grounded = cs.grounded, ground = cs.ground and cs.ground.name or "", active = js.active,
         spin = scene.find("Wheel").angular_velocity.z, floor = scene.find("Floor"):drop_through() }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["grounded"].as_bool());
    CHECK(q.value()["ground"].as_string() == "Floor");
    CHECK(q.value()["active"].as_bool());
    CHECK(q.value()["spin"].as_number() == doctest::Approx(3.14159).epsilon(0.02));
    CHECK_FALSE(q.value()["floor"].as_bool());
}

TEST_CASE("script: animation playback API") {
    TempDir d("victus_script_animation");
    d.write("scripts/dancer.luau", R"lua(
local Dancer = {}
function Dancer:start()
    local clips = self.entity:animation_clips()
    self.entity.name = "Dancer:" .. #clips .. ":" .. clips[1] .. ":" .. clips[2]
end
function Dancer:update(dt)
    self.t = (self.t or 0) + dt
    if self.t > 0.25 and not self.switched then
        self.switched = self.entity:play_animation("Spin", 0)
        self.bad = self.entity:play_animation("Missing")
    end
end
return Dancer
)lua");
    auto host = make_host(d);
    auto set = std::make_shared<animation::AnimationSet>();
    set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {.translation = {0, 1, 0}}});
    animation::Clip slide{.name = "Slide", .duration = 1.0f};
    slide.channels.push_back({.node = 0, .path = animation::Path::Translation, .interpolation = animation::Interpolation::Linear,
                              .times = {0, 1}, .values = {0, 1, 0, 2, 1, 0}});
    animation::Clip spin{.name = "Spin", .duration = 1.0f};
    const math::Quat q1 = math::Quat::axis_angle({0, 1, 0}, 1.0f);
    spin.channels.push_back({.node = 0, .path = animation::Path::Rotation, .interpolation = animation::Interpolation::Linear,
                             .times = {0, 1}, .values = {0, 0, 0, 1, q1.x, q1.y, q1.z, q1.w}});
    set->clips = {slide, spin};
    const Uuid model = Uuid::generate();
    animation::AnimationSystem anim;
    anim.add_set(model, set);

    gameplay::Simulation sim;
    animation::add_animation_systems(sim, anim);
    script::add_script_systems(sim, *host);
    Scene s;
    const Entity dancer = scripted(s, "Dancer", "scripts/dancer.luau");
    const Entity hips = s.create("Hips", dancer);
    auto& a = s.world().add<animation::AnimatorComponent>(dancer);
    a.model = model;
    a.clip = "Slide";
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(dancer)->value == "Dancer:2:Slide:Spin");
    CHECK(s.world().get<animation::AnimatorComponent>(dancer)->clip == "Spin");
    auto q = host->eval(R"lua(
local d = scene.find("Dancer:2:Slide:Spin")
local st = d:animation()
return { valid = st.valid, clip = st.clip, playing = st.playing, looping = st.looping, duration = st.duration,
         time = st.time, none = scene.find("Hips"):play_animation("Spin") }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["valid"].as_bool());
    CHECK(q.value()["clip"].as_string() == "Spin");
    CHECK(q.value()["playing"].as_bool());
    CHECK(q.value()["looping"].as_bool());
    CHECK(q.value()["duration"].as_number() == doctest::Approx(1.0));
    CHECK(q.value()["time"].as_number() > 0.0);
    CHECK_FALSE(q.value()["none"].as_bool()); // no Animator on Hips
    // Spin was switched in with no fade: Hips rotates about Y and has left the slide.
    const auto* t = s.world().get<Transform>(hips);
    CHECK(std::abs(t->rotation.y) > 0.01f);
}

TEST_CASE("script: sprite animation API") {
    TempDir d("victus_script_sprites");
    d.write("scripts/hero.luau", R"lua(
local Hero = {}
function Hero:start()
    local clips = self.entity:sprite_clips()
    self.entity.name = "Hero:" .. #clips .. ":" .. clips[1]
    self.bad = self.entity:play_sprite("fly")
end
function Hero:update(dt)
    self.t = (self.t or 0) + dt
    if self.t > 0.2 and not self.jumped then self.jumped = self.entity:play_sprite("jump") end
end
return Hero
)lua");
    auto host = make_host(d);
    auto sheet = std::make_shared<animation::SpriteSheet>();
    sheet->texture = Uuid::generate();
    sheet->columns = 4;
    sheet->rows = 1;
    sheet->clips.push_back({.name = "walk", .frames = {0, 1}, .fps = 8, .loop = true});
    sheet->clips.push_back({.name = "jump", .frames = {2, 3}, .fps = 4, .loop = false});
    const Uuid sheet_id = Uuid::generate();
    animation::AnimationSystem anim;
    anim.add_sheet(sheet_id, sheet);
    gameplay::Simulation sim;
    animation::add_animation_systems(sim, anim); // binds the system before scripts start
    script::add_script_systems(sim, *host);
    Scene s;
    const Entity hero = scripted(s, "Hero", "scripts/hero.luau");
    s.world().add<scene::SpriteComponent>(hero);
    auto& a = s.world().add<animation::SpriteAnimatorComponent>(hero);
    a.sheet = sheet_id;
    a.clip = "walk";
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(hero)->value == "Hero:2:walk");
    CHECK(s.world().get<animation::SpriteAnimatorComponent>(hero)->clip == "jump");
    auto q = host->eval(R"lua(
local h = scene.find("Hero:2:walk")
local st = h:sprite_animation()
return { valid = st.valid, clip = st.clip, frame = st.frame, looping = st.looping, finished = st.finished,
         duration = st.duration, other = scene.spawn("Plain"):sprite_animation().valid }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["valid"].as_bool());
    CHECK(q.value()["clip"].as_string() == "jump");
    CHECK(q.value()["frame"].as_number() >= 2);
    CHECK_FALSE(q.value()["looping"].as_bool());
    CHECK(q.value()["duration"].as_number() == doctest::Approx(0.5));
    CHECK_FALSE(q.value()["other"].as_bool());
    // The jump frames sit in the right half of the sheet.
    CHECK(s.world().get<scene::SpriteComponent>(hero)->uv.x >= 0.5f);
}

TEST_CASE("script: particle emitter API") {
    TempDir d("victus_script_particles");
    d.write("scripts/fx.luau", R"lua(
local Fx = {}
function Fx:update(dt)
    self.frames = (self.frames or 0) + 1
    if self.frames == 1 then
        self.ok = self.entity:emit_particles(7)
        self.stopped = self.entity:set_emitting(false)
        self.plain = scene.find("Plain"):emit_particles(3)
    elseif self.frames == 3 then
        local st = self.entity:particles()
        self.entity.name = string.format("Fx|%s|%s|%s|%d|%s|%s", tostring(self.ok), tostring(self.stopped),
            tostring(self.plain), st.alive, tostring(st.emitting), tostring(st.valid))
    end
end
return Fx
)lua");
    auto host = make_host(d);
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    particles::ParticleSystem ps;
    particles::add_particle_systems(sim, ps);
    Scene s;
    s.create("Plain");
    const Entity fx = scripted(s, "Fx", "scripts/fx.luau");
    auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
    c.rate = 0; // only the scripted burst
    c.lifetime_min = c.lifetime_max = 10;
    for (int i = 0; i < 3; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(fx)->value == "Fx|true|true|false|7|false|true");
    CHECK(particles::status(s, fx).alive == 7);
    auto q = host->eval(R"lua(
local fx = scene.find("Fx|true|true|false|7|false|true")
local reset = fx:reset_particles()
local none = scene.find("Plain"):particles()
return { reset = reset, none = none.valid, alive = fx:particles().alive }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["reset"].as_bool());
    CHECK_FALSE(q.value()["none"].as_bool());
    CHECK(q.value()["alive"].as_number() == doctest::Approx(0.0));
}

TEST_CASE("script: audio one-shots, entity sources and bus control") {
    TempDir d("victus_script_audio");
    std::vector<float> tone(4800);
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * std::sin(static_cast<float>(i) * 0.0576f);
    REQUIRE(audio::write_wav(d.path / "tone.wav", tone.data(), tone.size(), 1, 48000).ok());
    d.write("scripts/speaker.luau", R"lua(
local Speaker = {}
function Speaker:update(dt)
    if not self.started then
        self.started = true
        self.ok_entity = self.entity:play_audio()
        self.ok_shot = audio.play("tone.wav", { volume = 0.5, bus = "Ui", position = vector.create(1, 0, 0) })
        self.ok_missing = audio.play("missing.wav")
        audio.set_volume("Music", 0.25)
        audio.set_muted("Voice", true)
    end
    self.entity.name = string.format("%s|%s|%s|%s|%.2f|%s", tostring(self.ok_entity), tostring(self.ok_shot),
        tostring(self.ok_missing), tostring(self.entity:audio_playing()), audio.volume("Music"), tostring(audio.muted("Voice")))
end
return Speaker
)lua");
    auto host = make_host(d);
    // Without an engine the API is inert.
    {
        Scene empty;
        auto r = host->eval("return { audio.available(), audio.play('tone.wav'), audio.volume() }", &empty);
        REQUIRE(r.ok());
        CHECK(json::stringify(*r, false) == "[false,false,0]");
    }
    audio::register_components();
    audio::EngineOptions o;
    o.assets_root = d.path;
    o.null_device = true;
    auto a = audio::AudioEngine::create(o);
    REQUIRE(a.ok());
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    audio::add_audio_systems(sim, **a);
    Scene s;
    const Entity e = scripted(s, "Speaker", "scripts/speaker.luau");
    auto& src = s.world().add<audio::AudioSourceComponent>(e);
    src.clip = "tone.wav";
    src.play_on_start = false;
    src.spatial = false;
    sim.tick(s, 1.0f / 60);
    sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(e)->value == "true|true|false|true|0.25|true");
    CHECK((*a)->stats().one_shots == 1);
    CHECK((*a)->bus_volume(audio::Bus::Music) == doctest::Approx(0.25f));
    auto bad = host->eval("audio.set_volume('Nope', 1)", &s);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("unknown audio bus") != std::string::npos);
}

TEST_CASE("script: in-game UI text, visibility, state and on_click") {
    TempDir d("victus_script_ui");
    // The menu script sits on the canvas: clicks on its buttons bubble up to it.
    d.write("scripts/menu.luau", R"lua(
local Menu = {}
function Menu:start()
    self.score = 0
    scene.find("Score"):set_text("Score 0")
    scene.find("Banner"):set_visible(false)
end
function Menu:on_click(button, action)
    if action == "add" then
        self.score += 1
        scene.find("Score"):set_text("Score " .. self.score)
        if self.score >= 2 then scene.find("Banner"):set_visible(true) end
    end
    self.last = button.name .. ":" .. action
end
function Menu:update(dt)
    local w, h = ui.screen_size()
    self.entity.name = (self.last or "none") .. "|" .. w .. "x" .. h
end
return Menu
)lua");
    auto host = make_host(d);
    gameui::register_components();
    gameplay::Simulation sim;
    gameui::add_ui_systems(sim);
    script::add_script_systems(sim, *host);
    gameplay::ManualInput input;
    sim.input = &input;
    Scene s;
    gameui::set_screen_size(s, {640, 360});
    const Entity canvas = scripted(s, "Menu", "scripts/menu.luau");
    auto& cc = s.world().add<gameui::UiCanvasComponent>(canvas);
    cc.reference_size = {640, 360};
    const auto rect = [&](const char* name, math::Vec2 pos) {
        const Entity e = s.create(name, canvas);
        gameui::UiRectComponent r;
        r.anchor_min = r.anchor_max = {0, 0};
        r.pivot = {0, 0};
        r.position = pos;
        r.size = {100, 40};
        s.world().add<gameui::UiRectComponent>(e, r);
        return e;
    };
    const Entity score = rect("Score", {10, 10});
    s.world().add<gameui::UiTextComponent>(score);
    const Entity add = rect("Add", {10, 100});
    s.world().add<gameui::UiButtonComponent>(add).action = "add";
    const Entity banner = rect("Banner", {200, 10});
    s.world().add<gameui::UiPanelComponent>(banner);

    sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<gameui::UiTextComponent>(score)->text == "Score 0");
    CHECK_FALSE(s.world().get<gameui::UiRectComponent>(banner)->visible);
    // Two real mouse clicks on the button (press + release inside it).
    input.set_mouse_position({50, 120});
    for (int i = 0; i < 2; ++i) {
        input.set_mouse(0, true);
        sim.tick(s, 1.0f / 60);
        input.set_mouse(0, false);
        sim.tick(s, 1.0f / 60);
    }
    CHECK(s.world().get<gameui::UiTextComponent>(score)->text == "Score 2");
    CHECK(s.world().get<gameui::UiRectComponent>(banner)->visible);
    CHECK(s.world().get<scene::Name>(canvas)->value == "Add:add|640x360");

    auto q = host->eval(R"lua(
local b = scene.find("Add")
local st = b:ui_state()
local clicked = ui.click(b)
local banner = scene.find("Banner")
return { hovered = st.hovered, x = st.x, w = st.width, clicks = st.clicks, clicked = clicked,
         text = scene.find("Score"):text(), visible = banner:visible(), over = ui.pointer_over_ui(),
         none = scene.find("Banner"):ui_state().pressed }
)lua", &s);
    REQUIRE(q.ok());
    CHECK(q.value()["hovered"].as_bool());
    CHECK(q.value()["x"].as_number() == doctest::Approx(10));
    CHECK(q.value()["w"].as_number() == doctest::Approx(100));
    CHECK(q.value()["clicks"].as_number() == doctest::Approx(2));
    CHECK(q.value()["clicked"].as_bool());
    CHECK(q.value()["text"].as_string() == "Score 2");
    CHECK(q.value()["visible"].as_bool());
    CHECK(q.value()["over"].as_bool());
    CHECK_FALSE(q.value()["none"].as_bool());
    // The scripted click is delivered to on_click on the next update.
    sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<gameui::UiTextComponent>(score)->text == "Score 3");
    auto bad = host->eval("scene.find('Banner'):set_text('x')", &s);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("no UiText") != std::string::npos);
}

TEST_CASE("script: navigation queries and agents (nav.*, set_destination, nav_state)") {
    TempDir d("victus_script_nav");
    d.write("scripts/walker.luau", R"lua(
local Walker = {}
Walker.goal = vector.create(4, 0, 0)
function Walker:start()
    self.ok = self.entity:set_destination(self.goal)
end
function Walker:update(dt)
    local st = self.entity:nav_state()
    if st.reached and not self.done then
        self.done = true
        self.entity.name = "Arrived:" .. tostring(self.ok)
    end
end
return Walker
)lua");
    auto host = make_host(d);
    navigation::register_components();
    gameplay::Simulation sim;
    script::add_script_systems(sim, *host);
    navigation::NavigationSystem nav;
    navigation::add_navigation_systems(sim, nav);
    Scene s;
    const Entity floor = s.create("Floor");
    s.world().get<scene::Transform>(floor)->position = {0, -0.5f, 0};
    s.world().add<physics::ColliderComponent>(floor).size = {12, 1, 12};
    const Entity wall = s.create("Wall");
    s.world().get<scene::Transform>(wall)->position = {0, 1, 1};
    s.world().add<physics::ColliderComponent>(wall).size = {1, 2, 8}; // z in [-3, 5]
    const Entity walker = scripted(s, "Walker", "scripts/walker.luau");
    s.world().get<scene::Transform>(walker)->position = {-4, 0, 0};
    s.world().add<navigation::NavAgentComponent>(walker);

    // Before any update (bound by hand here; the installed systems bind it each frame) the navmesh
    // is built on demand by the first query.
    navigation::bind(s, &nav);
    auto q = host->eval(R"lua(
local path, partial = nav.find_path(vector.create(-4, 0, 0), vector.create(4, 0, 0))
local near = nav.nearest(vector.create(-4, 3, 0))
local hit, at, normal = nav.raycast(vector.create(-4, 0, 0), vector.create(4, 0, 0))
local free = nav.raycast(vector.create(-4, 0, 0), vector.create(-4, 0, 4))
local r = nav.random_point(3)
local r2 = nav.random_point(3)
return { available = nav.available(), corners = #path, partial = partial, near_y = near.y,
         hit = hit, hit_x = at.x, normal_x = normal.x, free = free,
         same = r == r2, rx = r.x, off = nav.nearest(vector.create(50, 0, 50)) == nil,
         none = nav.find_path(vector.create(50, 0, 50), vector.create(0, 0, 0)) == nil }
)lua", &s);
    if (!q.ok()) MESSAGE(q.error().message);
    REQUIRE(q.ok());
    CHECK(q.value()["available"].as_bool());
    CHECK(q.value()["corners"].as_number() >= 3); // around the wall's -z end
    CHECK_FALSE(q.value()["partial"].as_bool());
    CHECK(std::fabs(q.value()["near_y"].as_number()) < 0.25);
    CHECK(q.value()["hit"].as_bool());
    CHECK(q.value()["hit_x"].as_number() < -0.5);
    CHECK(q.value()["normal_x"].as_number() < -0.5);
    CHECK_FALSE(q.value()["free"].as_bool());
    CHECK(q.value()["same"].as_bool());
    CHECK(std::fabs(q.value()["rx"].as_number()) <= 6.0);
    CHECK(q.value()["off"].as_bool());
    CHECK(q.value()["none"].as_bool());

    for (int i = 0; i < 60 * 8 && s.world().get<scene::Name>(walker)->value == "Walker"; ++i) sim.tick(s, 1.0f / 60);
    CHECK(s.world().get<scene::Name>(walker)->value == "Arrived:true");
    const math::Vec3 p = s.world().get<scene::Transform>(walker)->position;
    CHECK(std::hypot(p.x - 4, p.z) < 0.3f);

    auto st = host->eval(R"lua(
local w = scene.find("Arrived:true")
w:set_destination(vector.create(-4, 0, 4))
local a = w:nav_state()
w:stop()
local b = w:nav_state()
return { active = a.active, moving = a.has_destination, stopped = not b.has_destination,
         dx = a.destination.x, rebuilt = nav.rebuild() }
)lua", &s);
    REQUIRE(st.ok());
    CHECK(st.value()["active"].as_bool());
    CHECK(st.value()["moving"].as_bool());
    CHECK(st.value()["stopped"].as_bool());
    CHECK(st.value()["dx"].as_number() == doctest::Approx(-4));
    CHECK(st.value()["rebuilt"].as_bool());
    CHECK(host->stats().errors == 0);
}

TEST_CASE("script: scene.instantiate creates prefab instances whose scripts run") {
    TempDir d("victus_script_prefab");
    script::register_components();
    // Build the prefab: a "Coin" root with a spinning script and a "Glow" child light.
    {
        Scene src;
        const Entity coin = scripted(src, "Coin", "scripts/spin.luau");
        src.create("Glow", coin);
        const scene::Prefab p = scene::make_prefab(src, coin);
        REQUIRE(scene::save_prefab_file(p, d.path / "prefabs/coin.vprefab").ok());
        d.write("prefabs/coin.vprefab.meta", R"({"uuid": "6f1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a7b", "version": 1})");
    }
    d.write("scripts/spin.luau", R"lua(
local Spin = {}
function Spin:update(dt) self.entity.name = "Spun" end
return Spin
)lua");
    d.write("scripts/spawner.luau", R"lua(
local M = {}
function M:start()
    self.a = scene.instantiate("prefabs/coin.vprefab", { position = vector.create(3, 0, 0) })
    self.b = scene.instantiate("prefabs/coin.vprefab", { parent = self.entity, name = "Child coin" })
    assert(self.a.position.x == 3, "position")
    assert(self.b.parent == self.entity, "parent")
    local ok, err = pcall(scene.instantiate, "prefabs/missing.vprefab")
    assert(not ok and string.find(err, "missing.vprefab"), "missing prefab error: " .. tostring(err))
end
return M
)lua");
    Scene s;
    const Entity spawner = scripted(s, "Spawner", "scripts/spawner.luau");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sim.tick(s, 0.02f);
    sim.tick(s, 0.02f);
    CHECK(host->stats().errors == 0);
    const auto instances = scene::find_instances(s);
    REQUIRE(instances.size() == 2);
    Uuid expected;
    REQUIRE(Uuid::parse("6f1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a7b", expected));
    for (const Entity e : instances) {
        CHECK(s.world().get<scene::PrefabInstance>(e)->prefab == expected);
        CHECK(s.world().get<scene::Name>(e)->value == "Spun"); // the instance's script ran
        CHECK(s.children(e).size() == 1);
    }
    CHECK(s.parent(instances[0]) == spawner); // hierarchy order: the spawner (and its child) come first
    CHECK(s.parent(instances[1]).is_null());
}

TEST_CASE("script: rendering API swaps materials, meshes, colors and sprites") {
    TempDir d("victus_script_render");
    script::register_components();
    d.write("materials/gold.vmat", R"({"victus_material": 1})");
    d.write("materials/gold.vmat.meta", R"({"uuid": "7a1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a01", "version": 1})");
    d.write("models/ship.glb", "x");
    d.write("models/ship.glb.meta", R"({"uuid": "7a1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a02", "version": 1})");
    d.write("textures/coin.png", "x");
    d.write("textures/coin.png.meta", R"({"uuid": "7a1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a03", "version": 1})");
    d.write("materials/raw.vmat", R"({"victus_material": 1})"); // no .meta: not imported
    d.write("scripts/render.luau", R"lua(
local M = {}
function M:start()
    local e, sp = self.entity, scene.find("Sprite")
    assert(e:material() == nil, "no material yet")
    e:set_material("materials/gold.vmat")
    assert(e:material() == "materials/gold.vmat", "material path")
    local ok, err = pcall(e.set_material, e, "textures/coin.png")
    assert(not ok and string.find(err, "vmat"), "non-material rejected: " .. tostring(err))
    ok, err = pcall(e.set_material, e, "materials/raw.vmat")
    assert(not ok and string.find(err, "not an imported asset"), "unimported rejected: " .. tostring(err))
    e:set_mesh("models/ship.glb", 2)
    local m, i = e:mesh()
    assert(m == "models/ship.glb" and i == 2, "model mesh")
    e:set_color(vector.create(1, 0.5, 0.25), 0.5)
    local c, a = e:color()
    assert(c.y == 0.5 and a == 0.5, "color")
    e:set_color(vector.create(0, 1, 0))
    c, a = e:color()
    assert(c.x == 0 and a == 0.5, "alpha kept")
    e:set_visible(false)
    assert(e:visible() == false, "visible")
    e:set_cast_shadows(false)
    sp:set_sprite("textures/coin.png")
    assert(sp:sprite() == "textures/coin.png", "sprite")
    sp:set_color(vector.create(1, 0, 0), 1)
    ok = pcall(scene.find("Empty").set_color, scene.find("Empty"), vector.create(1, 1, 1))
    assert(not ok, "no renderer error")
    assert(scene.find("Empty"):material() == nil, "material of plain entity")
end
function M:update()
    local e = self.entity
    e:set_mesh("sphere")
    e:set_material(nil)
    local m = e:mesh()
    assert(m == "sphere" and e:material() == nil, "primitive reset")
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Ship", "scripts/render.luau");
    s.world().add<scene::MeshRendererComponent>(e).primitive = "cube";
    const Entity sp = s.create("Sprite");
    s.world().add<scene::SpriteComponent>(sp);
    s.create("Empty");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sim.tick(s, 0.02f); // start + update
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    const auto* m = s.world().get<scene::MeshRendererComponent>(e);
    CHECK(m->primitive == "sphere");
    CHECK(m->mesh_asset.is_nil());
    CHECK(m->material_asset.is_nil());
    CHECK_FALSE(m->visible);
    CHECK_FALSE(m->cast_shadows);
    CHECK(m->base_color.g == doctest::Approx(1.0f));
    CHECK(m->base_color.a == doctest::Approx(0.5f));
    const auto* spr = s.world().get<scene::SpriteComponent>(sp);
    CHECK(spr->texture_asset.to_string() == "7a1e2d3c-4b5a-4978-8a1b-2c3d4e5f6a03");
    CHECK(spr->tint.g == doctest::Approx(0.0f));
}

TEST_CASE("script: uv offset, scroll and emissive overrides") {
    TempDir d("victus_script_overrides");
    script::register_components();
    gameplay::register_components();
    d.write("scripts/glow.luau", R"lua(
local M = {}
function M:start()
    local e = self.entity
    assert(not e:has("MaterialOverrides"), "starts without overrides")
    e:set_uv_offset(vector.create(0.25, 0.5, 0))
    e:set_uv_scroll(vector.create(1, 0, 0))
    e:set_emissive(vector.create(1, 0.5, 0), 4)
    local o = e:get("MaterialOverrides")
    assert(o.override_emissive and o.emissive_strength == 4, "emissive")
    assert(o.uv_offset[2] == 0.5, "offset")
end
function M:update()
    if time.frame() == 2 then self.entity:set_emissive(nil) end
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Lava", "scripts/glow.luau");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sim.tick(s, 0.02f);
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    const auto* o = s.world().get<gameplay::MaterialOverridesComponent>(e);
    REQUIRE(o);
    CHECK(o->uv_offset.x == doctest::Approx(0.25f));
    CHECK(o->uv_scroll.x == doctest::Approx(1.0f));
    CHECK(o->emissive.g == doctest::Approx(0.5f));
    for (int i = 0; i < 4; ++i) sim.tick(s, 0.02f);
    CHECK(host->stats().errors == 0);
    CHECK_FALSE(o->override_emissive);
}

TEST_CASE("script: tilemap painting, queries and coordinates") {
    TempDir d("victus_script_tilemap");
    script::register_components();
    gameplay::register_components();
    tilemap::register_components();
    d.write("scripts/level.luau", R"lua(
local M = {}
function M:start()
    local e = self.entity
    e:set_tile(0, 0, 3)
    e:set_tile(1, 0, 4, true, false)
    local t, fx, fy = e:tile(1, 0)
    assert(t == 4 and fx == true and fy == false, "tile read back")
    assert(e:tile(5, 5) == nil, "empty cell")
    assert(e:fill_tiles(0, -2, 9, -2, 1) == 10, "fill count")
    local x0, y0, x1, y1 = e:tile_bounds()
    assert(x0 == 0 and y0 == -2 and x1 == 9 and y1 == 0, "bounds")
    e:set_tile(0, 0, nil)
    assert(e:tile(0, 0) == nil, "erase")
    local tx, ty = e:world_to_tile(vector.create(10 + 2.5, 20 - 1.5, 0))
    assert(tx == 2 and ty == -2, "world_to_tile " .. tx .. "," .. ty)
    local p = e:tile_to_world(3, 0)
    assert(math.abs(p.x - (10 + 3.5)) < 1e-4 and math.abs(p.y - 20.5) < 1e-4, "tile_to_world")
    assert(not pcall(e.set_tile, e, 1e9, 0, 1), "coordinate range")
    assert(not pcall(e.paint_terrain, e, 0, 5, "lava"), "unknown terrain")
    local other = scene.spawn("Plain")
    assert(not pcall(other.set_tile, other, 0, 0, 1), "needs a Tilemap")
end
function M:update()
    if time.frame() == 1 then
        local e = self.entity
        e:clear_tiles()
        e:paint_terrain(0, 5, "ground")
        e:paint_terrain(1, 5, "ground")
        assert(e:tile(0, 5) == 2 and e:tile(1, 5) == 8, "terrain masks")
        e:paint_terrain(1, 5, nil)
        assert(e:tile(0, 5) == 0, "terrain erase")
    end
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Level", "scripts/level.luau");
    s.world().get<scene::Transform>(e)->position = {10, 20, 0};
    const Uuid tsid = Uuid::generate();
    auto ts = std::make_shared<tilemap::Tileset>();
    ts->columns = 4;
    ts->rows = 4;
    tilemap::Terrain ground;
    ground.name = "ground";
    for (int i = 0; i < 16; ++i) ground.tiles[static_cast<std::size_t>(i)] = i;
    ts->terrains.push_back(ground);
    tilemap::TilemapComponent map;
    map.tileset = tsid;
    s.world().add<tilemap::TilemapComponent>(e, map);
    tilemap::TilemapSystem tiles;
    tiles.add_tileset(tsid, ts);
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    tilemap::add_tilemap_systems(sim, tiles);
    script::add_script_systems(sim, *host);
    for (int i = 0; i < 4; ++i) sim.tick(s, 0.02f);
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    const auto* m = s.world().get<tilemap::TilemapComponent>(e);
    CHECK(m->cells.size() == 1);
    CHECK(m->tile(0, 5) == 0);
}

TEST_CASE("script: terrain height, sample and raycast queries") {
    TempDir d("victus_script_terrain");
    script::register_components();
    gameplay::register_components();
    terrain::register_components();
    d.write("scripts/probe.luau", R"lua(
local M = {}
function M:update()
    local h = terrain.height(4, -3)
    assert(h ~= nil and math.abs(h - (5 + 0.25 * 20)) < 1e-3, "height " .. tostring(h))
    assert(terrain.height(500, 0) == nil, "off the terrain")
    local s = terrain.sample(4, -3)
    assert(s ~= nil and s.entity.name == "Ground", "sample entity")
    assert(math.abs(s.height - h) < 1e-4, "sample height")
    assert(s.normal.y > 0.999, "flat normal")
    assert(s.weights[3] > 0.99 and s.weights[1] < 0.01, "painted layer 2")
    local hit = terrain.raycast(vector.create(4, 50, -3), vector.create(0, -1, 0))
    assert(hit ~= nil and math.abs(hit.point.y - h) < 1e-2, "raycast point")
    assert(math.abs(hit.distance - (50 - h)) < 1e-2, "raycast distance")
    assert(terrain.raycast(vector.create(4, 50, -3), vector.create(0, 1, 0)) == nil, "raycast miss")
    assert(terrain.height(4, -3, s.entity) == h, "explicit entity")
    assert(not pcall(terrain.height, 4, -3, self.entity), "needs a Terrain")
    assert(not pcall(terrain.raycast, vector.create(0, 0, 0), vector.create(0, -1, 0), -1), "bad distance")
    self.entity.name = "ok"
end
return M
)lua");
    Scene s;
    const Entity probe = scripted(s, "Probe", "scripts/probe.luau");
    const Entity ground = s.create("Ground");
    s.world().get<scene::Transform>(ground)->position = {0, 5, 0};
    auto hf = terrain::make_flat(33, 0.25f);
    REQUIRE(hf.ok());
    for (auto& w : hf->splat) w = terrain::pack_weights({0, 0, 1, 0});
    const Uuid id = Uuid::generate();
    terrain::TerrainSystem terrains;
    terrains.add_data(id, std::make_shared<terrain::Heightfield>(std::move(*hf)));
    terrain::TerrainComponent comp;
    comp.data = id;
    comp.size = {64, 20, 64};
    comp.collision = false;
    s.world().add<terrain::TerrainComponent>(ground, comp);
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    terrain::add_terrain_systems(sim, terrains);
    script::add_script_systems(sim, *host);
    s.update_transforms();
    for (int i = 0; i < 2; ++i) sim.tick(s, 0.02f);
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    CHECK(s.world().get<scene::Name>(probe)->value == "ok");
}

TEST_CASE("script: AI blackboards, perception, noises and Luau behaviour tree tasks") {
    TempDir d("victus_script_ai");
    script::register_components();
    gameplay::register_components();
    ai::register_components();
    d.write("scripts/guard.luau", R"lua(
local Guard = {}
function Guard:start()
    assert(ai.available(), "ai available")
    self.entity:bb_set("speed", 3)
    self.entity:bb_set("home", vector.create(1, 2, 3))
    self.entity:bb_set("tmp", true)
    self.entity:bb_set("tmp", nil)
    assert(self.entity:bb_get("tmp") == nil, "nil erases")
    assert(not pcall(function() self.entity:bb_set("bad", {}) end), "tables rejected")
    self.waves = 0
    self.firsts = 0
end
function Guard:wave(dt, first)
    assert(dt > 0, "task dt")
    if first then self.firsts += 1 end
    self.waves += 1
    if self.waves < 3 then return "running" end
    return "success"
end
function Guard:update(dt)
    local bb = self.entity:blackboard()
    local p = self.entity:perception()
    if bb.waved and bb.speed == 3 and p and p.visible and p.target and p.target.name == "Player"
        and p.seen[1] == p.target and not self.reported then
        self.reported = true
        self.entity.name = "Done:" .. self.waves .. ":" .. self.firsts
        ai.report_noise(vector.create(0, 0, -2), 1, nil)
    end
end
return Guard
)lua");
    d.write("scripts/idle.luau", R"lua(
local M = {}
function M:aborting() return "running" end
function M:bogus() return "maybe" end
function M:on_task_abort(name) self.entity.name = "Aborted:" .. name end
return M
)lua");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    ai::AiSystem brain;
    ai::add_ai_systems(sim, brain);
    Scene s;
    const Entity guard = scripted(s, "Guard", "scripts/guard.luau");
    ai::PerceptionComponent pc;
    pc.line_of_sight = false;
    s.world().add<ai::PerceptionComponent>(guard, pc);
    const Entity player = s.create("Player");
    s.world().get<scene::Transform>(player)->position = {0, 0, 5};
    s.world().add<ai::PerceptibleComponent>(player);
    auto tree = ai::BehaviorTree::parse(R"({"root": {"type": "sequence", "children": [
        {"type": "action", "action": "wave"}, {"type": "set", "key": "waved", "value": true}]}})");
    REQUIRE(tree.ok());
    brain.set_tree(s, guard, *tree);

    // A running script task is aborted when a reactive selector's earlier branch succeeds; an
    // invalid return value fails the leaf and is reported.
    const Entity other = scripted(s, "Other", "scripts/idle.luau");
    auto reactive = ai::BehaviorTree::parse(R"({"root": {"type": "selector", "reactive": true, "children": [
        {"type": "check", "key": "stop", "value": true}, {"type": "action", "action": "aborting"}]}})");
    REQUIRE(reactive.ok());
    brain.set_tree(s, other, *reactive);
    const Entity bogus = scripted(s, "Bogus", "scripts/idle.luau");
    auto bad = ai::BehaviorTree::parse(R"({"root": {"type": "action", "action": "bogus"}})");
    REQUIRE(bad.ok());
    brain.set_tree(s, bogus, *bad);

    s.update_transforms();
    for (int i = 0; i < 3; ++i) sim.tick(s, 0.02f);
    brain.blackboard(s, other)->set("stop", true);
    for (int i = 0; i < 3; ++i) sim.tick(s, 0.02f);
    CHECK(s.world().get<scene::Name>(guard)->value == "Done:3:1");
    CHECK(s.world().get<scene::Name>(other)->value == "Aborted:aborting");
    CHECK(brain.blackboard(s, guard)->vec3("home")->z == 3.0f);
    CHECK(brain.blackboard(s, guard)->boolean("waved"));
    CHECK_FALSE(brain.blackboard(s, guard)->has("tmp"));
    const auto bogus_debug = brain.debug(s, bogus);
    REQUIRE(bogus_debug.has_value());
    CHECK(bogus_debug->nodes.at(0).status == ai::NodeStatus::Failure);
    bool reported = false;
    for (const auto& err : host->recent_errors()) reported |= err.find("task returned \"maybe\"") != std::string::npos;
    CHECK(reported);
    sim.tick(s, 0.02f);
    const ai::PerceptionState* ps = brain.perception(s, guard);
    REQUIRE(ps);
    CHECK(ps->heard);
    CHECK(ps->noise_position.z == doctest::Approx(-2.0f));

    // Without an AI system the queries are harmless and writes report an error.
    Scene bare;
    const Entity lone = bare.create("Lone");
    auto q = host->eval(R"lua(
local e = scene.find("Lone")
assert(not ai.available())
assert(e:bb_get("x") == nil and e:perception() == nil and next(e:blackboard()) == nil)
ai.report_noise(vector.create(0, 0, 0))
return pcall(function() e:bb_set("x", 1) end)
)lua", &bare);
    REQUIRE(q.ok());
    CHECK(q->is_bool());
    CHECK_FALSE(q->as_bool());
    (void)lone;
}

TEST_CASE("script: sequencer playback control, state and on_sequence_event") {
    TempDir d("victus_script_sequencer");
    script::register_components();
    gameplay::register_components();
    sequencer::register_components();
    d.write("seq/intro.vseq", R"({"duration": 0.3, "tracks": [
        {"type": "event", "target": "Door", "keys": [{"t": 0.1, "name": "open", "payload": "fast"}]},
        {"type": "event", "keys": [{"t": 0.2, "name": "done", "payload": "x"}]}]})");
    d.write("scripts/director.luau", R"lua(
local M = {}
function M:start()
    assert(sequencer.available(), "sequencer available")
    assert(self.entity:play_sequence("seq/intro.vseq"), "plays")
    assert(not self.entity:play_sequence("seq/missing.vseq"), "missing fails")
    assert(self.entity:play_sequence("seq/intro.vseq"), "plays again")
    self.entity:pause_sequence()
    assert(self.entity:sequence_state().paused, "paused")
    self.entity:pause_sequence(false)
    self.log = ""
end
function M:on_sequence_event(name, payload, player)
    assert(player == self.entity, "player")
    self.log ..= name .. "=" .. payload .. ";"
end
function M:update(dt)
    local st = self.entity:sequence_state()
    if st.finished and not self.reported then
        self.reported = true
        self.entity.name = "Finished:" .. self.log .. st.sequence
        assert(self.entity:seek_sequence(0.05))
        assert(self.entity:sequence_state().time > 0.04)
        self.entity:stop_sequence()
        assert(not self.entity:sequence_state().playing)
    end
end
return M
)lua");
    d.write("scripts/door.luau", R"lua(
local M = {}
function M:on_sequence_event(name, payload, player)
    self.entity.name = "Door:" .. name .. ":" .. payload .. ":" .. player.name
end
return M
)lua");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sequencer::SequencerSystem seqs(sequencer::directory_loader(d.path));
    sequencer::add_sequencer_systems(sim, seqs);
    Scene s;
    const Entity director = scripted(s, "Director", "scripts/director.luau");
    s.world().add<sequencer::SequencePlayerComponent>(director, sequencer::SequencePlayerComponent{.play_on_start = false});
    const Entity door = s.create("Door", director);
    s.world().add<script::ScriptComponent>(door, script::ScriptComponent{.script = "scripts/door.luau"});
    for (int i = 0; i < 25; ++i) sim.tick(s, 0.02f);
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    CHECK(s.world().get<scene::Name>(door)->value == "Door:open:fast:Director");
    CHECK(s.world().get<scene::Name>(director)->value == "Finished:open=fast;done=x;seq/intro.vseq");
    CHECK(seqs.stats().events == 2);

    // Without a sequencer the calls are harmless.
    Scene bare;
    bare.create("Lone");
    auto q = host->eval(R"lua(
local e = scene.find("Lone")
assert(not sequencer.available())
e:stop_sequence()
e:pause_sequence(true)
return not e:play_sequence() and not e:seek_sequence(1) and e:sequence_state() == nil
)lua", &bare);
    REQUIRE(q.ok());
    CHECK(q->as_bool());
}
