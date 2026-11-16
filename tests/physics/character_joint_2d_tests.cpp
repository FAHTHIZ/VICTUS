// SPDX-License-Identifier: Apache-2.0
// 2D character mover, one-way platform and 2D joint tests (Slice 14). All scenarios run on the real
// Box2D backend at 60 Hz; tolerances account for the mover's collision skin and solver softness.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kG = 9.81f;

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

Entity box2d(Scene& scene, math::Vec2 p, math::Vec2 size, float angle_deg = 0, bool one_way = false) {
    const Entity e = scene.create("Static");
    auto* t = scene.world().get<Transform>(e);
    t->position = {p.x, p.y, 0};
    t->rotation = math::Quat::axis_angle({0, 0, 1}, angle_deg * std::numbers::pi_v<float> / 180.0f);
    Collider2DComponent c;
    c.size = size;
    c.one_way = one_way;
    scene.world().add<Collider2DComponent>(e, c);
    return e;
}

// Top surface at y = 0.
Entity floor2d(Scene& scene) { return box2d(scene, {0, -0.5f}, {60, 1}); }

Entity dynamic2d(Scene& scene, math::Vec2 p, math::Vec2 size = {0.5f, 0.5f}, float mass = 1.0f) {
    const Entity e = box2d(scene, p, size);
    scene.world().get<Transform>(e)->rotation = {};
    RigidBody2DComponent rb;
    rb.mass = mass;
    rb.linear_damping = 0;
    rb.angular_damping = 0;
    scene.world().add<RigidBody2DComponent>(e, rb);
    return e;
}

Entity character2d(Scene& scene, math::Vec2 centre, CharacterController2DComponent c = {}) {
    const Entity e = scene.create("Player");
    scene.world().get<Transform>(e)->position = {centre.x, centre.y, 0};
    scene.world().add<CharacterController2DComponent>(e, c);
    return e;
}

void joint_on(Scene& scene, Entity e, const Joint2DComponent& j) { scene.world().add<Joint2DComponent>(e, j); }

void run(PhysicsWorld& w, Scene& scene, int steps) {
    for (int i = 0; i < steps; ++i) w.step(scene, kDt);
}

math::Vec3 pos(Scene& scene, Entity e) { return scene.world().get<Transform>(e)->position; }

} // namespace

// ---- character mover ------------------------------------------------------------------------------

TEST_CASE("2D character lands, reports state and walks") {
    Scene scene;
    auto w = make_world();
    const Entity g = floor2d(scene);
    const Entity p = character2d(scene, {0, 2});
    scene.world().get<Transform>(p)->position.z = 3.0f; // sorting depth survives the mover
    REQUIRE(w->has_body(p) == false); // created on the first step
    run(*w, scene, 90);
    REQUIRE(w->has_body(p));
    const CharacterState s = w->character_state(p);
    REQUIRE(s.valid);
    CHECK(s.grounded);
    CHECK_FALSE(s.on_steep_slope);
    CHECK(s.ground == g);
    CHECK(s.ground_normal.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(pos(scene, p).y == doctest::Approx(0.5f).epsilon(0.03)); // centred 1 m capsule on y = 0
    CHECK(pos(scene, p).z == doctest::Approx(3.0f));
    CHECK(std::abs(s.velocity.y) < 0.05f);

    w->move_character(p, {3, 0, 0});
    run(*w, scene, 60);
    CHECK(pos(scene, p).x == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(w->character_state(p).grounded);
    CHECK(w->linear_velocity(p).x == doctest::Approx(3.0f).epsilon(0.02));
    w->move_character(p, {});
    run(*w, scene, 5);
    const float x = pos(scene, p).x;
    run(*w, scene, 30);
    CHECK(pos(scene, p).x == doctest::Approx(x).epsilon(0.001));
}

TEST_CASE("2D character is blocked by walls and does not stick to them in the air") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    box2d(scene, {3.5f, 2}, {1, 4}); // wall face at x = 3
    const Entity p = character2d(scene, {0, 0.6f});
    run(*w, scene, 30);
    w->move_character(p, {5, 0, 0});
    run(*w, scene, 120);
    CHECK(pos(scene, p).x == doctest::Approx(2.6f).epsilon(0.02)); // radius 0.4 against the face
    CHECK(w->character_state(p).grounded);
    // Jumping while pressing into the wall: the character slides up and falls back, not sticking.
    CHECK(w->jump_character(p, 6.0f));
    float top = 0;
    for (int i = 0; i < 120; ++i) {
        w->step(scene, kDt);
        top = std::max(top, pos(scene, p).y);
    }
    CHECK(top > 2.0f);
    CHECK(w->character_state(p).grounded);
    CHECK(pos(scene, p).y == doctest::Approx(0.5f).epsilon(0.03));
}

TEST_CASE("2D character respects max_slope and snaps onto downward slopes") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    // A 20 degree ramp (walkable) and a 70 degree ramp (too steep for the default 50).
    box2d(scene, {-6, 0}, {8, 1}, 20);
    box2d(scene, {8, 0}, {8, 1}, 70);
    const Entity a = character2d(scene, {-6, 3});
    run(*w, scene, 90);
    const CharacterState gs = w->character_state(a);
    REQUIRE(gs.grounded);
    CHECK(gs.ground_normal.y == doctest::Approx(std::cos(20.0f * std::numbers::pi_v<float> / 180.0f)).epsilon(0.02));
    // Standing still on walkable ground: no creeping downhill.
    const math::Vec3 before = pos(scene, a);
    run(*w, scene, 60);
    CHECK(std::abs(pos(scene, a).x - before.x) < 0.02f);
    // Walking down the ramp keeps the feet on it (snap) instead of hopping off.
    w->move_character(a, {-3, 0, 0});
    int airborne = 0;
    for (int i = 0; i < 40; ++i) {
        w->step(scene, kDt);
        if (!w->character_state(a).grounded) ++airborne;
    }
    CHECK(airborne <= 1);

    // The steep ramp is not ground: a character dropped on it slides off.
    const Entity b = character2d(scene, {7.0f, 4.5f});
    run(*w, scene, 30);
    for (int i = 0; i < 60; ++i) {
        w->step(scene, kDt);
        const CharacterState s = w->character_state(b);
        CHECK_FALSE((s.grounded && s.ground_normal.y < 0.6f));
    }
}

TEST_CASE("2D character jumps only when grounded and follows ballistic height") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    const Entity p = character2d(scene, {0, 0.6f});
    run(*w, scene, 30);
    REQUIRE(w->character_state(p).grounded);
    const float y0 = pos(scene, p).y;
    CHECK(w->jump_character(p, 5.0f));
    float top = y0;
    bool left_ground = false;
    for (int i = 0; i < 90; ++i) {
        w->step(scene, kDt);
        top = std::max(top, pos(scene, p).y);
        if (!w->character_state(p).grounded) {
            left_ground = true;
            CHECK_FALSE(w->jump_character(p, 5.0f)); // no double jumps
        }
    }
    CHECK(left_ground);
    CHECK(top - y0 == doctest::Approx(25.0f / (2 * kG)).epsilon(0.06));
    CHECK(w->character_state(p).grounded);
}

TEST_CASE("2D character sees one-way platforms only from above and drops through on request") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    const Entity plat = box2d(scene, {0, 3}, {4, 0.2f}, 0, true); // top surface at y = 3.1
    const Entity p = character2d(scene, {0, 0.6f});
    run(*w, scene, 30);
    REQUIRE(w->character_state(p).grounded);
    // Jump up through the platform from below and land on it.
    CHECK(w->jump_character(p, std::sqrt(2 * kG * 4.0f)));
    run(*w, scene, 150);
    CharacterState s = w->character_state(p);
    CHECK(s.grounded);
    CHECK(s.ground == plat);
    CHECK(pos(scene, p).y == doctest::Approx(3.6f).epsilon(0.02));
    // Walking off the side drops to the floor; walking back under it is unobstructed.
    w->move_character(p, {4, 0, 0});
    run(*w, scene, 90);
    w->move_character(p, {-4, 0, 0});
    run(*w, scene, 60);
    CHECK(pos(scene, p).y == doctest::Approx(0.5f).epsilon(0.05));
    w->move_character(p, {});
    // Back on top, then drop through.
    scene.world().get<Transform>(p)->position = {0, 3.7f, 0};
    run(*w, scene, 30);
    REQUIRE(w->character_state(p).ground == plat);
    CHECK(w->drop_through(p));
    run(*w, scene, 90);
    CHECK(pos(scene, p).y == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(w->character_state(p).grounded);
    CHECK_FALSE(w->drop_through(plat)); // not a character
}

TEST_CASE("one-way platforms catch falling bodies and let rising bodies through") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    box2d(scene, {0, 3}, {6, 0.2f}, 0, true);
    const Entity falling = dynamic2d(scene, {-1, 6});
    const Entity rising = dynamic2d(scene, {1, 0.25f});
    run(*w, scene, 2);
    w->set_linear_velocity(rising, {0, 10, 0}); // apex ~5.1 m: passes the platform from below
    run(*w, scene, 180);
    CHECK(pos(scene, falling).y == doctest::Approx(3.35f).epsilon(0.02)); // rests on the top surface
    CHECK(pos(scene, rising).y == doctest::Approx(3.35f).epsilon(0.02));  // came through and landed on top
}

TEST_CASE("2D character rides kinematic platforms, pushes crates and is seen by queries") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    // Moving platform: kinematic, driven through its Transform.
    const Entity plat = box2d(scene, {-10, 2}, {3, 0.4f});
    RigidBody2DComponent krb;
    krb.type = BodyType::Kinematic;
    scene.world().add<RigidBody2DComponent>(plat, krb);
    const Entity rider = character2d(scene, {-10, 3.0f});
    run(*w, scene, 40);
    REQUIRE(w->character_state(rider).ground == plat);
    const float rx = pos(scene, rider).x;
    for (int i = 0; i < 60; ++i) {
        scene.world().get<Transform>(plat)->position.x += 2.0f * kDt;
        w->step(scene, kDt);
    }
    CHECK(pos(scene, rider).x - rx == doctest::Approx(2.0f).epsilon(0.08));
    CHECK(w->character_state(rider).grounded);

    // Trigger, crate and raycast.
    const Entity p = character2d(scene, {0, 0.6f});
    const Entity trig = box2d(scene, {4, 1}, {1, 2});
    scene.world().get<Collider2DComponent>(trig)->is_trigger = true;
    const Entity crate = dynamic2d(scene, {-2, 0.25f}, {0.5f, 0.5f}, 4.0f);
    run(*w, scene, 20);
    const auto hit = w->raycast_2d({10, 0.5f}, {-1, 0}, 20, kAllLayers, trig);
    REQUIRE(hit);
    CHECK(hit->entity == p);
    CHECK(hit->point.x == doctest::Approx(0.4f).epsilon(0.05));
    const auto self = w->raycast_2d({0, 0.25f}, {-1, 0}, 20, kAllLayers, p);
    REQUIRE(self);
    CHECK(self->entity == crate);

    w->begin_frame();
    w->move_character(p, {4, 0, 0});
    bool entered = false;
    for (int i = 0; i < 90 && !entered; ++i) {
        w->step(scene, kDt);
        for (const ContactEvent& ev : w->events())
            if (ev.kind == ContactEvent::Kind::Begin && ev.trigger && (ev.a == p || ev.b == p) &&
                (ev.a == trig || ev.b == trig))
                entered = true;
    }
    CHECK(entered);
    w->move_character(p, {-2, 0, 0});
    const float crate_x = pos(scene, crate).x;
    run(*w, scene, 240);
    CHECK(pos(scene, crate).x < crate_x - 1.0f);
    // Weak characters cannot push heavy crates.
    scene.world().get<CharacterController2DComponent>(p)->push_force = 0;
    run(*w, scene, 60); // let the crate's momentum die out
    const float stuck = pos(scene, crate).x;
    run(*w, scene, 60);
    CHECK(pos(scene, crate).x == doctest::Approx(stuck).epsilon(0.02));

    // Teleport through the Transform.
    w->move_character(p, {});
    scene.world().get<Transform>(p)->position = {20, 5, 0};
    run(*w, scene, 120);
    CHECK(pos(scene, p).x == doctest::Approx(20.0f).epsilon(0.01));
    CHECK(pos(scene, p).y == doctest::Approx(0.5f).epsilon(0.05));
    const PhysicsStats stats = w->stats();
    CHECK(stats.characters_2d == 2);
}

TEST_CASE("2D character replaces a collider and rebuilds on edit") {
    Scene scene;
    auto w = make_world();
    floor2d(scene);
    const Entity p = character2d(scene, {0, 0.6f});
    scene.world().add<Collider2DComponent>(p, Collider2DComponent{}); // ignored
    run(*w, scene, 30);
    CHECK(w->stats().bodies_2d == 2); // floor + the character's inner body
    scene.world().get<CharacterController2DComponent>(p)->size = {0.8f, 2.0f};
    w->move_character(p, {1, 0, 0});
    run(*w, scene, 60);
    CHECK(pos(scene, p).y == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(pos(scene, p).x > 0.6f); // the desired velocity survived the rebuild
    scene.world().remove<CharacterController2DComponent>(p);
    run(*w, scene, 1);
    CHECK_FALSE(w->character_state(p).valid);
    CHECK(w->has_body(p)); // back to its plain collider
}

// ---- joints ---------------------------------------------------------------------------------------

TEST_CASE("2D weld joint to the world holds a body in place") {
    Scene scene;
    auto w = make_world();
    const Entity b = dynamic2d(scene, {0, 5});
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Weld;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(pos(scene, b).y == doctest::Approx(5.0f).epsilon(0.01));
    const JointState s = w->joint_state(b);
    CHECK(s.valid);
    CHECK(s.active);
    CHECK(s.force == doctest::Approx(kG * 1.0f).epsilon(0.05)); // carries the 1 kg body's weight
    CHECK(w->stats().joints_2d == 1);
}

TEST_CASE("2D revolute pendulum keeps its length, limits and motor") {
    Scene scene;
    auto w = make_world();
    // Pendulum: pivot 2 m to the left of the bob, released horizontally.
    const Entity bob = dynamic2d(scene, {2, 10}, {0.3f, 0.3f});
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Revolute;
    j.anchor = {-2, 0};
    joint_on(scene, bob, j);
    float lowest = 10;
    for (int i = 0; i < 240; ++i) {
        w->step(scene, kDt);
        const math::Vec3 p = pos(scene, bob);
        CHECK(std::hypot(p.x, p.y - 10) == doctest::Approx(2.0f).epsilon(0.01));
        lowest = std::min(lowest, p.y);
    }
    CHECK(lowest == doctest::Approx(8.0f).epsilon(0.01));

    // Limits: a gate hinged at its left end that may swing down only 30 degrees.
    const Entity gate = dynamic2d(scene, {11, 5}, {2, 0.2f});
    Joint2DComponent l;
    l.type = Joint2DComponent::Type::Revolute;
    l.anchor = {-1, 0};
    l.limits = true;
    l.min_limit = -30;
    l.max_limit = 30;
    joint_on(scene, gate, l);
    run(*w, scene, 180);
    CHECK(w->joint_state(gate).angle == doctest::Approx(-30.0f).epsilon(0.03));

    // Motor: a wheel on the world spins at the commanded speed; set_joint_motor writes the component.
    const Entity wheel = dynamic2d(scene, {20, 5}, {1, 1});
    Joint2DComponent m;
    m.type = Joint2DComponent::Type::Revolute;
    joint_on(scene, wheel, m);
    run(*w, scene, 1);
    w->set_joint_motor(scene, wheel, true, 90.0f);
    CHECK(scene.world().get<Joint2DComponent>(wheel)->motor);
    run(*w, scene, 60);
    CHECK(w->angular_velocity(wheel).z == doctest::Approx(std::numbers::pi_v<float> / 2).epsilon(0.02));
    w->set_joint_motor(scene, wheel, true, 0.0f);
    run(*w, scene, 30);
    CHECK(std::abs(w->angular_velocity(wheel).z) < 0.01f);
}

TEST_CASE("2D prismatic joint slides along its axis with limits and a motor") {
    Scene scene;
    auto w = make_world();
    // Vertical slider on the world: falls under gravity until the -2 m limit.
    const Entity b = dynamic2d(scene, {0, 5});
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Prismatic;
    j.axis = {0, 1};
    j.limits = true;
    j.min_limit = -2;
    j.max_limit = 1;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(pos(scene, b).y == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(pos(scene, b).x == doctest::Approx(0.0f));
    CHECK(w->joint_state(b).position == doctest::Approx(-2.0f).epsilon(0.01));
    // Motor drives it up to the upper limit.
    w->set_joint_motor(scene, b, true, 1.0f);
    run(*w, scene, 240);
    CHECK(pos(scene, b).y == doctest::Approx(6.0f).epsilon(0.01));
    // Sideways pushes do not leave the axis.
    w->add_impulse(b, {5, 0, 0});
    run(*w, scene, 30);
    CHECK(std::abs(pos(scene, b).x) < 0.01f);
}

TEST_CASE("2D distance joint acts as a rope, wheel joint as a suspension") {
    Scene scene;
    auto w = make_world();
    const Entity anchor = box2d(scene, {0, 10}, {0.2f, 0.2f});
    const Entity b = dynamic2d(scene, {0, 9}, {0.3f, 0.3f});
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Distance;
    j.connected = scene.uuid(anchor);
    j.limits = true;
    j.min_limit = 0;
    j.max_limit = 3;
    joint_on(scene, b, j);
    run(*w, scene, 120);
    CHECK(pos(scene, b).y == doctest::Approx(7.0f).epsilon(0.01)); // hangs at the rope length
    CHECK(w->joint_state(b).position == doctest::Approx(3.0f).epsilon(0.01));

    // A rigid distance joint keeps its initial length.
    const Entity c = dynamic2d(scene, {5, 8}, {0.3f, 0.3f});
    Joint2DComponent r;
    r.type = Joint2DComponent::Type::Distance;
    r.connected_anchor = {5, 10}; // world point
    joint_on(scene, c, r);
    w->add_impulse(c, {1, 0, 0});
    run(*w, scene, 90);
    CHECK(w->joint_state(c).position == doctest::Approx(2.0f).epsilon(0.01));

    // Wheel: a soft vertical suspension sags under load but stays on its axis.
    const Entity wheel = dynamic2d(scene, {10, 5}, {0.5f, 0.5f}, 2.0f);
    Joint2DComponent s;
    s.type = Joint2DComponent::Type::Wheel;
    s.axis = {0, 1};
    s.spring_hertz = 2.0f;
    s.spring_damping = 1.0f;
    joint_on(scene, wheel, s);
    run(*w, scene, 240);
    // Static deflection of a 2 Hz spring: g / (2*pi*f)^2.
    const float omega = 2 * std::numbers::pi_v<float> * 2.0f;
    CHECK(w->joint_state(wheel).position == doctest::Approx(-kG / (omega * omega)).epsilon(0.05));
    CHECK(pos(scene, wheel).x == doctest::Approx(10.0f).epsilon(0.001));
}

TEST_CASE("2D joints break, skip connected collisions and survive body rebuilds") {
    Scene scene;
    auto w = make_world();
    // 10 kg box on a weld that only holds 50 N.
    const Entity b = dynamic2d(scene, {0, 5}, {1, 1}, 10.0f);
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Weld;
    j.break_force = 50.0f;
    joint_on(scene, b, j);
    run(*w, scene, 60);
    JointState s = w->joint_state(b);
    CHECK(s.broken);
    CHECK_FALSE(s.active);
    CHECK(pos(scene, b).y < 4.0f);
    // Editing the component restores it.
    scene.world().get<Joint2DComponent>(b)->break_force = 0;
    scene.world().get<Transform>(b)->position = {0, 5, 0};
    run(*w, scene, 30);
    CHECK(w->joint_state(b).active);
    CHECK(pos(scene, b).y == doctest::Approx(5.0f).epsilon(0.01));

    // Rebuilding the body (collider edit) keeps the joint.
    scene.world().get<Collider2DComponent>(b)->size = {0.8f, 0.8f};
    run(*w, scene, 30);
    CHECK(w->joint_state(b).active);
    CHECK(pos(scene, b).y == doctest::Approx(5.0f).epsilon(0.01));
    // Removing the connected body removes a body-to-body joint.
    const Entity a = dynamic2d(scene, {3, 5});
    const Entity c = dynamic2d(scene, {4, 5});
    Joint2DComponent pj;
    pj.type = Joint2DComponent::Type::Revolute;
    pj.connected = scene.uuid(a);
    joint_on(scene, c, pj);
    run(*w, scene, 2);
    CHECK(w->joint_state(c).active);
    scene.destroy(a);
    run(*w, scene, 2);
    CHECK_FALSE(w->joint_state(c).valid);

    // Two overlapping boxes chained by a revolute joint: no contacts unless collide_connected.
    for (bool collide : {false, true}) {
        Scene s2;
        auto w2 = make_world();
        const Entity x = dynamic2d(s2, {0, 5}, {1, 1});
        const Entity y = dynamic2d(s2, {0.5f, 5}, {1, 1});
        Joint2DComponent rj;
        rj.type = Joint2DComponent::Type::Revolute;
        rj.connected = s2.uuid(x);
        rj.collide_connected = collide;
        joint_on(s2, y, rj);
        w2->begin_frame();
        run(*w2, s2, 10);
        const bool touched = std::any_of(w2->events().begin(), w2->events().end(), [&](const ContactEvent& ev) {
            return ev.kind == ContactEvent::Kind::Begin && ((ev.a == x && ev.b == y) || (ev.a == y && ev.b == x));
        });
        CHECK(touched == collide);
    }
}

TEST_CASE("2D character and joint components round-trip and draw wireframes") {
    register_components();
    for (const char* name : {"CharacterController2D", "Joint2D"}) CHECK(scene::find_schema(name));
    Scene scene;
    const Entity e = scene.create("Player");
    CharacterController2DComponent cc;
    cc.size = {0.6f, 1.4f};
    cc.max_slope = 40;
    cc.air_control = 0.8f;
    cc.layer = 2;
    scene.world().add<CharacterController2DComponent>(e, cc);
    const Entity other = scene.create("Other");
    Joint2DComponent j;
    j.type = Joint2DComponent::Type::Wheel;
    j.connected = scene.uuid(e);
    j.anchor = {0.5f, -0.25f};
    j.axis = {0, 1};
    j.limits = true;
    j.motor = true;
    j.motor_speed = 45;
    j.break_force = 100;
    scene.world().add<Joint2DComponent>(other, j);
    Collider2DComponent col;
    col.one_way = true;
    scene.world().add<Collider2DComponent>(other, col);
    const json::Value doc = scene.save();
    Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    const Entity le = loaded.find(scene.uuid(e)), lo = loaded.find(scene.uuid(other));
    REQUIRE_FALSE(le.is_null());
    REQUIRE_FALSE(lo.is_null());
    CHECK(*loaded.world().get<CharacterController2DComponent>(le) == cc);
    CHECK(*loaded.world().get<Joint2DComponent>(lo) == j);
    CHECK(loaded.world().get<Collider2DComponent>(lo)->one_way);
    auto bad = json::parse(R"({"type":"rope"})");
    REQUIRE(bad.ok());
    Joint2DComponent tmp;
    CHECK_FALSE(scene::find_schema("Joint2D")->load(&tmp, bad.value()).ok());

    std::vector<DebugLine> lines;
    collider_lines(loaded, lines);
    CHECK(lines.size() > 20);
    std::vector<DebugLine> only;
    collider_lines(loaded, only, le);
    CHECK_FALSE(only.empty());
    CHECK(only.size() < lines.size());
}
