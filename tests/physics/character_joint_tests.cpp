// SPDX-License-Identifier: Apache-2.0
// Character controller and joint tests (Slice 11). All scenarios are simulated with the real Jolt
// backend at 60 Hz; tolerances account for Jolt's character padding and solver softness.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

Entity static_box(Scene& scene, math::Vec3 p, math::Vec3 size, math::Quat q = {}) {
    const Entity e = scene.create("Static");
    auto* t = scene.world().get<Transform>(e);
    t->position = p;
    t->rotation = q;
    ColliderComponent c;
    c.size = size;
    scene.world().add<ColliderComponent>(e, c);
    return e;
}

Entity ground(Scene& scene) { return static_box(scene, {0, -0.5f, 0}, {40, 1, 40}); }

Entity dynamic_box(Scene& scene, math::Vec3 p, math::Vec3 size = {1, 1, 1}, float mass = 1.0f) {
    const Entity e = scene.create("Box");
    scene.world().get<Transform>(e)->position = p;
    ColliderComponent c;
    c.size = size;
    scene.world().add<ColliderComponent>(e, c);
    RigidBodyComponent rb;
    rb.mass = mass;
    rb.linear_damping = 0;
    rb.angular_damping = 0;
    scene.world().add<RigidBodyComponent>(e, rb);
    return e;
}

Entity character(Scene& scene, math::Vec3 feet, CharacterControllerComponent c = {}) {
    const Entity e = scene.create("Player");
    scene.world().get<Transform>(e)->position = feet;
    scene.world().add<CharacterControllerComponent>(e, c);
    return e;
}

void run(PhysicsWorld& w, Scene& scene, int steps) {
    for (int i = 0; i < steps; ++i) w.step(scene, kDt);
}

math::Vec3 pos(Scene& scene, Entity e) { return scene.world().get<Transform>(e)->position; }

} // namespace

// ---- character controller -----------------------------------------------------------------------

TEST_CASE("character lands on the ground, reports state and walks") {
    Scene scene;
    auto w = make_world();
    const Entity g = ground(scene);
    const Entity p = character(scene, {0, 2, 0});
    run(*w, scene, 90);
    CharacterState s = w->character_state(p);
    REQUIRE(s.valid);
    CHECK(s.grounded);
    CHECK_FALSE(s.on_steep_slope);
    CHECK(s.ground == g);
    CHECK(s.ground_normal.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(pos(scene, p).y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(w->has_body(p));

    w->move_character(p, {3, 7, 0}); // Y is ignored
    run(*w, scene, 60);
    const math::Vec3 after = pos(scene, p);
    CHECK(after.x == doctest::Approx(3.0f).epsilon(0.08)); // 3 m/s for one second
    CHECK(after.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(w->character_state(p).velocity.x == doctest::Approx(3.0f).epsilon(0.05));

    w->move_character(p, {});
    run(*w, scene, 10);
    CHECK(std::abs(w->character_state(p).velocity.x) < 0.01f);
}

TEST_CASE("character is blocked by walls and slides along them") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    static_box(scene, {2.5f, 1, 0}, {1, 2, 10}); // wall face at x = 2
    const Entity p = character(scene, {0, 0, 0});
    run(*w, scene, 10);
    w->move_character(p, {4, 0, 2}); // diagonal into the wall
    run(*w, scene, 120);
    const math::Vec3 at = pos(scene, p);
    CHECK(at.x < 2.0f - 0.3f + 0.05f);
    CHECK(at.x > 2.0f - 0.3f - 0.1f);
    CHECK(at.z > 3.0f); // kept sliding along the wall
}

TEST_CASE("character climbs steps up to step_height and is stopped by taller ledges") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    static_box(scene, {3, 0.125f, 0}, {2, 0.25f, 4});    // 25 cm step at x in [2, 4]
    static_box(scene, {3, 0.3f, 10}, {2, 0.6f, 4});      // 60 cm ledge at z = 10
    const Entity a = character(scene, {0, 0, 0});
    const Entity b = character(scene, {0, 0, 10});
    run(*w, scene, 10);
    w->move_character(a, {2, 0, 0});
    w->move_character(b, {2, 0, 0});
    run(*w, scene, 90);
    CHECK(pos(scene, a).x > 2.5f);
    CHECK(pos(scene, a).y == doctest::Approx(0.25f).epsilon(0.06));
    CHECK(w->character_state(a).grounded);
    CHECK(pos(scene, b).x < 2.0f - 0.3f + 0.05f);
    CHECK(pos(scene, b).y < 0.05f);
}

TEST_CASE("character respects max_slope") {
    Scene scene;
    auto w = make_world();
    // A 20 degree and a 65 degree ramp far apart.
    static_box(scene, {0, 0, 0}, {10, 1, 10}, math::Quat::axis_angle({0, 0, 1}, math::radians(20)));
    static_box(scene, {10, 0, 0}, {10, 1, 10}, math::Quat::axis_angle({0, 0, 1}, math::radians(65)));
    ground(scene); // catches the character sliding off the steep ramp
    const Entity gentle = character(scene, {0, 0.6f, 0});
    // Start touching the steep face 2 m up its slope: surface point + normal * r - up * r.
    const float a = math::radians(65);
    const math::Vec3 n{-std::sin(a), std::cos(a), 0}, t{std::cos(a), std::sin(a), 0};
    const math::Vec3 surface = math::Vec3{10, 0, 0} + n * 0.5f + t * 2.0f;
    const Entity steep = character(scene, surface + n * 0.32f - math::Vec3{0, 0.3f, 0});
    bool saw_steep = false;
    for (int i = 0; i < 60; ++i) {
        w->step(scene, kDt);
        saw_steep |= w->character_state(steep).on_steep_slope;
    }
    const CharacterState gs = w->character_state(gentle);
    CHECK(gs.grounded);
    CHECK(gs.ground_normal.y == doctest::Approx(std::cos(math::radians(20))).epsilon(0.02));
    // Standing still on walkable ground: no sliding.
    const math::Vec3 before = pos(scene, gentle);
    run(*w, scene, 60);
    CHECK(math::length(pos(scene, gentle) - before) < 0.01f);
    // The steep ramp is not ground: the character slides down it (or falls off it).
    const float start_y = surface.y;
    for (int i = 0; i < 120; ++i) {
        w->step(scene, kDt);
        const CharacterState s = w->character_state(steep);
        saw_steep |= s.on_steep_slope;
        CHECK_FALSE((s.grounded && s.ground_normal.y < 0.6f));
    }
    CHECK(saw_steep);
    CHECK(pos(scene, steep).y < start_y - 0.5f);
}

TEST_CASE("character jumps only when grounded and follows ballistic height") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    const Entity p = character(scene, {0, 0, 0});
    run(*w, scene, 10);
    REQUIRE(w->character_state(p).grounded);
    CHECK(w->jump_character(p, 5.0f));
    float apex = 0;
    bool airborne = false;
    for (int i = 0; i < 90; ++i) {
        w->step(scene, kDt);
        apex = std::max(apex, pos(scene, p).y);
        if (!w->character_state(p).grounded) {
            airborne = true;
            if (i == 5) CHECK_FALSE(w->jump_character(p, 5.0f)); // no double jump
        }
    }
    CHECK(airborne);
    CHECK(apex == doctest::Approx(25.0f / (2 * 9.81f)).epsilon(0.08)); // v^2 / 2g = 1.27 m
    CHECK(w->character_state(p).grounded);
    CHECK(pos(scene, p).y == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("character is visible to raycasts and triggers, pushes boxes and can be teleported") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    const Entity p = character(scene, {0, 0, 0});
    // Trigger volume ahead.
    const Entity trig = scene.create("Trigger");
    scene.world().get<Transform>(trig)->position = {0, 1, 4};
    ColliderComponent tc;
    tc.size = {2, 2, 1};
    tc.is_trigger = true;
    scene.world().add<ColliderComponent>(trig, tc);
    const Entity box = dynamic_box(scene, {0, 0.5f, -2.5f}, {1, 1, 1}, 5.0f);
    run(*w, scene, 10);

    const auto hit = w->raycast({0, 1, 10}, {0, 0, -1}, 20, kAllLayers, trig);
    REQUIRE(hit);
    CHECK(hit->entity == p);
    CHECK(hit->point.z == doctest::Approx(0.3f).epsilon(0.05));
    // A character's own raycasts can ignore itself.
    const auto self = w->raycast({0, 1, 0}, {0, 0, -1}, 20, kAllLayers, p);
    REQUIRE(self);
    CHECK(self->entity == box);

    // Walk into the trigger.
    w->begin_frame();
    w->move_character(p, {0, 0, 4});
    bool entered = false;
    for (int i = 0; i < 60 && !entered; ++i) {
        w->step(scene, kDt);
        for (const ContactEvent& ev : w->events())
            if (ev.kind == ContactEvent::Kind::Begin && ev.trigger && (ev.a == p || ev.b == p) &&
                (ev.a == trig || ev.b == trig))
                entered = true;
    }
    CHECK(entered);

    // Walk back and push the box.
    w->move_character(p, {0, 0, -2});
    const float box_z = pos(scene, box).z;
    run(*w, scene, 300);
    CHECK(pos(scene, box).z < box_z - 0.5f);

    // Teleport through the Transform.
    w->move_character(p, {});
    scene.world().get<Transform>(p)->position = {10, 0, 10};
    run(*w, scene, 5);
    CHECK(pos(scene, p).x == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(pos(scene, p).z == doctest::Approx(10.0f).epsilon(0.01));
}

TEST_CASE("character rides a moving kinematic platform") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    const Entity plat = scene.create("Platform");
    scene.world().get<Transform>(plat)->position = {0, 1, 0};
    ColliderComponent pc;
    pc.size = {4, 0.2f, 4};
    scene.world().add<ColliderComponent>(plat, pc);
    RigidBodyComponent rb;
    rb.type = BodyType::Kinematic;
    scene.world().add<RigidBodyComponent>(plat, rb);
    const Entity p = character(scene, {0, 1.1f, 0});
    run(*w, scene, 20);
    REQUIRE(w->character_state(p).grounded);
    CHECK(w->character_state(p).ground == plat);
    for (int i = 0; i < 60; ++i) {
        scene.world().get<Transform>(plat)->position.x += 2.0f * kDt;
        w->step(scene, kDt);
    }
    CHECK(pos(scene, p).x == doctest::Approx(2.0f).epsilon(0.1));
}

TEST_CASE("character controller replaces a collider and rebuilds on edit") {
    Scene scene;
    auto w = make_world();
    ground(scene);
    const Entity p = character(scene, {0, 0, 0});
    scene.world().add<ColliderComponent>(p, ColliderComponent{}); // ignored
    run(*w, scene, 10);
    const std::uint32_t bodies = w->stats().bodies_3d;
    CHECK(bodies == 2); // ground + the character's inner body
    scene.world().get<CharacterControllerComponent>(p)->height = 1.0f;
    run(*w, scene, 10);
    CHECK(w->stats().bodies_3d == 2);
    const auto hit = w->raycast({0, 1.5f, 5}, {0, 0, -1}, 20);
    CHECK_FALSE(hit); // the shorter capsule is below the ray
    scene.world().remove<CharacterControllerComponent>(p);
    run(*w, scene, 1);
    CHECK_FALSE(w->character_state(p).valid);
}

// ---- joints -------------------------------------------------------------------------------------

namespace {

Entity joint_on(Scene& scene, Entity e, JointComponent j) {
    scene.world().add<JointComponent>(e, j);
    return e;
}

} // namespace

TEST_CASE("fixed joint to the world holds a body in place") {
    Scene scene;
    auto w = make_world();
    const Entity b = dynamic_box(scene, {0, 5, 0});
    JointComponent j;
    j.type = JointComponent::Type::Fixed;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(math::length(pos(scene, b) - math::Vec3{0, 5, 0}) < 0.02f);
    const JointState s = w->joint_state(b);
    CHECK(s.valid);
    CHECK(s.active);
    CHECK_FALSE(s.broken);
    CHECK(s.force == doctest::Approx(9.81f).epsilon(0.05)); // carries the box's weight
}

TEST_CASE("point and hinge pendulums keep their length") {
    for (auto type : {JointComponent::Type::Point, JointComponent::Type::Hinge}) {
        Scene scene;
        auto w = make_world();
        const Entity b = dynamic_box(scene, {2, 5, 0}, {0.5f, 0.5f, 0.5f});
        JointComponent j;
        j.type = type;
        j.anchor = {-2, 0, 0}; // pivot at the world origin's x, y = 5
        j.axis = {0, 0, 1};
        joint_on(scene, b, j);
        float lowest = 5;
        for (int i = 0; i < 120; ++i) {
            w->step(scene, kDt);
            const math::Vec3 p = pos(scene, b);
            CHECK(math::length(p - math::Vec3{0, 5, 0}) == doctest::Approx(2.0f).epsilon(0.02));
            lowest = std::min(lowest, p.y);
            if (type == JointComponent::Type::Hinge) CHECK(std::abs(p.z) < 0.01f); // stays in the hinge plane
        }
        CHECK(lowest < 3.2f); // swung through the bottom
        if (type == JointComponent::Type::Hinge) CHECK(std::abs(w->joint_state(b).angle) > 10.0f);
    }
}

TEST_CASE("hinge limits and motor") {
    Scene scene;
    auto w = make_world();
    // Door hanging on a vertical hinge, pushed sideways.
    const Entity door = dynamic_box(scene, {0.5f, 1, 0}, {1, 2, 0.1f});
    JointComponent j;
    j.type = JointComponent::Type::Hinge;
    j.anchor = {-0.5f, 0, 0};
    j.axis = {0, 1, 0};
    j.limits = true;
    j.min_limit = -30;
    j.max_limit = 30;
    joint_on(scene, door, j);
    w->set_gravity({});
    w->sync(scene, kDt);
    w->set_angular_velocity(door, {0, 6, 0});
    float max_angle = 0;
    for (int i = 0; i < 120; ++i) {
        w->step(scene, kDt);
        max_angle = std::max(max_angle, std::abs(w->joint_state(door).angle));
    }
    CHECK(max_angle < 32.0f);
    CHECK(max_angle > 25.0f);

    // Motor: unlimited hinge turning at 90 deg/s.
    w->set_joint_motor(scene, door, true, 90.0f);
    scene.world().get<JointComponent>(door)->limits = false;
    run(*w, scene, 30); // spin up
    const float a0 = w->joint_state(door).angle;
    run(*w, scene, 20);
    const float a1 = w->joint_state(door).angle;
    float delta = a1 - a0;
    if (delta < -180) delta += 360;
    CHECK(std::abs(delta) == doctest::Approx(30.0f).epsilon(0.1)); // 90 deg/s * 1/3 s
    CHECK(scene.world().get<JointComponent>(door)->motor);
}

TEST_CASE("slider constrains motion to its axis with limits and a motor") {
    Scene scene;
    auto w = make_world();
    const Entity b = dynamic_box(scene, {0, 5, 0});
    JointComponent j;
    j.type = JointComponent::Type::Slider;
    j.axis = {0, 1, 0};
    j.limits = true;
    j.min_limit = -2;
    j.max_limit = 0;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(pos(scene, b).y == doctest::Approx(3.0f).epsilon(0.01)); // fell to the lower limit
    CHECK(std::abs(pos(scene, b).x) < 1e-3f);
    CHECK(w->joint_state(b).position == doctest::Approx(-2.0f).epsilon(0.01));

    // Motor lifts it at 1 m/s against gravity.
    auto* jc = scene.world().get<JointComponent>(b);
    jc->motor = true;
    jc->motor_speed = 1.0f;
    run(*w, scene, 60);
    CHECK(pos(scene, b).y == doctest::Approx(4.0f).epsilon(0.03));
}

TEST_CASE("distance joint acts as a rope between two bodies") {
    Scene scene;
    auto w = make_world();
    const Entity anchor = scene.create("Anchor");
    scene.world().get<Transform>(anchor)->position = {0, 10, 0};
    scene.world().add<ColliderComponent>(anchor, ColliderComponent{});
    RigidBodyComponent krb;
    krb.type = BodyType::Kinematic;
    scene.world().add<RigidBodyComponent>(anchor, krb);
    const Entity b = dynamic_box(scene, {0, 9, 0}, {0.5f, 0.5f, 0.5f});
    JointComponent j;
    j.type = JointComponent::Type::Distance;
    j.connected = scene.uuid(anchor);
    j.limits = true;
    j.min_limit = 0;
    j.max_limit = 3;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(pos(scene, b).y == doctest::Approx(7.0f).epsilon(0.01)); // hangs at the rope length
    CHECK(w->joint_state(b).position == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("joints break above break_force and connected bodies can skip collisions") {
    Scene scene;
    auto w = make_world();
    // 10 kg box on a joint that only holds 50 N.
    const Entity b = dynamic_box(scene, {0, 5, 0}, {1, 1, 1}, 10.0f);
    JointComponent j;
    j.type = JointComponent::Type::Fixed;
    j.break_force = 50.0f;
    joint_on(scene, b, j);
    run(*w, scene, 60);
    const JointState s = w->joint_state(b);
    CHECK(s.broken);
    CHECK_FALSE(s.active);
    CHECK(pos(scene, b).y < 4.0f);
    // Editing the component restores it.
    scene.world().get<JointComponent>(b)->break_force = 0;
    scene.world().get<Transform>(b)->position = {0, 5, 0};
    run(*w, scene, 30);
    CHECK(w->joint_state(b).active);

    // Two overlapping boxes chained by a point joint: no contacts unless collide_connected.
    for (bool collide : {false, true}) {
        Scene s2;
        auto w2 = make_world();
        const Entity a = dynamic_box(s2, {0, 5, 0});
        const Entity c = dynamic_box(s2, {0.5f, 5, 0});
        JointComponent pj;
        pj.type = JointComponent::Type::Point;
        pj.connected = s2.uuid(a);
        pj.collide_connected = collide;
        joint_on(s2, c, pj);
        w2->begin_frame();
        run(*w2, s2, 10);
        const bool touched = std::any_of(w2->events().begin(), w2->events().end(), [&](const ContactEvent& ev) {
            return ev.kind == ContactEvent::Kind::Begin && ((ev.a == a && ev.b == c) || (ev.a == c && ev.b == a));
        });
        CHECK(touched == collide);
    }
}

TEST_CASE("joints survive body rebuilds and disappear with their bodies") {
    Scene scene;
    auto w = make_world();
    const Entity b = dynamic_box(scene, {0, 5, 0});
    JointComponent j;
    j.type = JointComponent::Type::Fixed;
    joint_on(scene, b, j);
    run(*w, scene, 10);
    REQUIRE(w->joint_state(b).active);
    scene.world().get<ColliderComponent>(b)->size = {0.5f, 0.5f, 0.5f}; // rebuilds the body
    run(*w, scene, 60);
    CHECK(w->joint_state(b).active);
    CHECK(pos(scene, b).y == doctest::Approx(5.0f).epsilon(0.01));
    // Missing connected entity: no joint.
    scene.world().get<JointComponent>(b)->connected = Uuid::generate();
    run(*w, scene, 1);
    CHECK_FALSE(w->joint_state(b).valid);
    scene.world().remove<JointComponent>(b);
    run(*w, scene, 1);
    CHECK_FALSE(w->joint_state(b).valid);
    w->clear();
    CHECK_FALSE(w->has_body(b));
}

TEST_CASE("character and joint components reflect, round-trip and draw debug lines") {
    register_components();
    CHECK(scene::find_schema("CharacterController") != nullptr);
    CHECK(scene::find_schema("Joint") != nullptr);
    Scene scene;
    const Entity p = character(scene, {1, 0, 0});
    scene.world().get<CharacterControllerComponent>(p)->step_height = 0.5f;
    const Entity b = dynamic_box(scene, {0, 5, 0});
    JointComponent j;
    j.type = JointComponent::Type::Slider;
    j.connected = scene.uuid(p);
    j.motor = true;
    j.motor_speed = 2.5f;
    joint_on(scene, b, j);

    const json::Value doc = scene.save();
    Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    const Entity lp = loaded.find(scene.uuid(p));
    const Entity lb = loaded.find(scene.uuid(b));
    REQUIRE(loaded.world().get<CharacterControllerComponent>(lp));
    CHECK(loaded.world().get<CharacterControllerComponent>(lp)->step_height == doctest::Approx(0.5f));
    const auto* lj = loaded.world().get<JointComponent>(lb);
    REQUIRE(lj);
    CHECK(lj->type == JointComponent::Type::Slider);
    CHECK(lj->connected == scene.uuid(p));
    CHECK(lj->motor_speed == doctest::Approx(2.5f));

    std::vector<DebugLine> lines;
    collider_lines(scene, lines, p);
    CHECK(lines.size() > 40); // capsule
    const std::size_t n = lines.size();
    collider_lines(scene, lines, b);
    CHECK(lines.size() > n + 3); // joint cross + axis + link + box
}
