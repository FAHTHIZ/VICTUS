// SPDX-License-Identifier: Apache-2.0
// Luau scripting: lifecycle, entity/scene/input API, sandboxing, budgets, errors, hot reload.
#include "victus/animation/animation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"

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
