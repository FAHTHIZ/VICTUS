// SPDX-License-Identifier: Apache-2.0
#include "victus/core/jobs.hpp"
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

std::unique_ptr<PhysicsWorld> make_world(PhysicsSettings s = {}) {
    auto w = PhysicsWorld::create(s);
    REQUIRE(w.ok());
    return std::move(w.value());
}

Entity ground_3d(Scene& scene) {
    const Entity g = scene.create("Ground");
    scene.world().get<Transform>(g)->position = {0, -0.5f, 0};
    ColliderComponent c;
    c.size = {20, 1, 20};
    scene.world().add<ColliderComponent>(g, c);
    return g;
}

Entity box_3d(Scene& scene, math::Vec3 p, BodyType type = BodyType::Dynamic) {
    const Entity e = scene.create("Box");
    scene.world().get<Transform>(e)->position = p;
    scene.world().add<ColliderComponent>(e, ColliderComponent{});
    RigidBodyComponent rb;
    rb.type = type;
    scene.world().add<RigidBodyComponent>(e, rb);
    return e;
}

Entity ground_2d(Scene& scene) {
    const Entity g = scene.create("Ground2D");
    scene.world().get<Transform>(g)->position = {0, -0.5f, 0};
    Collider2DComponent c;
    c.size = {20, 1};
    scene.world().add<Collider2DComponent>(g, c);
    return g;
}

Entity box_2d(Scene& scene, math::Vec3 p) {
    const Entity e = scene.create("Box2D");
    scene.world().get<Transform>(e)->position = p;
    scene.world().add<Collider2DComponent>(e, Collider2DComponent{});
    scene.world().add<RigidBody2DComponent>(e, RigidBody2DComponent{});
    return e;
}

void run(PhysicsWorld& w, Scene& scene, int steps, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < steps; ++i) w.step(scene, dt);
}

bool has_event(const std::vector<ContactEvent>& evs, ContactEvent::Kind k, Entity a, Entity b, bool trigger) {
    return std::any_of(evs.begin(), evs.end(), [&](const ContactEvent& e) {
        return e.kind == k && e.trigger == trigger && ((e.a == a && e.b == b) || (e.a == b && e.b == a));
    });
}

} // namespace

TEST_CASE("3D dynamic box falls and rests on static ground") {
    Scene scene;
    auto w = make_world();
    const Entity ground = ground_3d(scene);
    const Entity box = box_3d(scene, {0, 3, 0});
    w->begin_frame();
    run(*w, scene, 180);
    const float y = scene.world().get<Transform>(box)->position.y;
    CHECK(y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(w->has_body(box));
    CHECK(w->has_body(ground));
    CHECK(has_event(w->events(), ContactEvent::Kind::Begin, ground, box, false));
    const auto st = w->stats();
    CHECK(st.bodies_3d == 2);
    CHECK(st.steps == 180);
}

TEST_CASE("3D simulation on the job system matches expectations") {
    jobs::JobSystem js(2);
    PhysicsSettings s;
    s.jobs = &js;
    Scene scene;
    auto w = make_world(s);
    ground_3d(scene);
    std::vector<Entity> boxes;
    for (int i = 0; i < 20; ++i) boxes.push_back(box_3d(scene, {static_cast<float>(i % 5) * 1.5f - 3, 1.0f + i * 0.4f, 0}));
    run(*w, scene, 240);
    for (Entity b : boxes) CHECK(scene.world().get<Transform>(b)->position.y > 0.3f);
}

TEST_CASE("3D raycast and overlap queries") {
    Scene scene;
    auto w = make_world();
    const Entity ground = ground_3d(scene);
    const Entity box = box_3d(scene, {0, 3, 0}, BodyType::Static);
    w->sync(scene, 1.0f / 60);
    auto hit = w->raycast({0, 10, 0}, {0, -1, 0}, 100);
    REQUIRE(hit);
    CHECK(hit->entity == box);
    CHECK(hit->point.y == doctest::Approx(3.5f).epsilon(0.01));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(hit->distance == doctest::Approx(6.5f).epsilon(0.01));
    // Ignore the box: the ground is next.
    hit = w->raycast({0, 10, 0}, {0, -1, 0}, 100, kAllLayers, box);
    REQUIRE(hit);
    CHECK(hit->entity == ground);
    CHECK_FALSE(w->raycast({0, 10, 0}, {0, 1, 0}, 100));
    const auto near_box = w->overlap_sphere({0, 3, 0}, 0.25f);
    REQUIRE(near_box.size() == 1);
    CHECK(near_box[0] == box);
    CHECK(w->overlap_box({0, 1.5f, 0}, {5, 2.1f, 5}).size() == 2);
}

TEST_CASE("3D layer masks filter collisions and queries") {
    Scene scene;
    auto w = make_world();
    const Entity ground = ground_3d(scene);
    scene.world().get<ColliderComponent>(ground)->layer = 1;
    const Entity ghost = box_3d(scene, {0, 2, 0});
    scene.world().get<ColliderComponent>(ghost)->mask = kAllLayers & ~(1u << 1);
    run(*w, scene, 120);
    CHECK(scene.world().get<Transform>(ghost)->position.y < -2.0f); // fell through
    CHECK_FALSE(w->raycast({0, 10, 0}, {0, -1, 0}, 100, 1u << 0, ghost));
    CHECK(w->raycast({0, 10, 0}, {0, -1, 0}, 100, 1u << 1, ghost));
}

TEST_CASE("3D triggers report enter and exit") {
    Scene scene;
    auto w = make_world();
    const Entity trigger = scene.create("Trigger");
    scene.world().get<Transform>(trigger)->position = {0, 2, 0};
    ColliderComponent tc;
    tc.size = {2, 0.5f, 2};
    tc.is_trigger = true;
    scene.world().add<ColliderComponent>(trigger, tc);
    const Entity ball = box_3d(scene, {0, 4, 0});
    bool entered = false, exited = false;
    for (int i = 0; i < 180; ++i) {
        w->begin_frame();
        w->step(scene, 1.0f / 60);
        entered |= has_event(w->events(), ContactEvent::Kind::Begin, trigger, ball, true);
        exited |= has_event(w->events(), ContactEvent::Kind::End, trigger, ball, true);
    }
    CHECK(entered);
    CHECK(exited);
    // A trigger does not stop the body.
    CHECK(scene.world().get<Transform>(ball)->position.y < 0.0f);
}

TEST_CASE("3D kinematic bodies follow their transform and push dynamic bodies") {
    Scene scene;
    auto w = make_world();
    ground_3d(scene);
    const Entity pusher = box_3d(scene, {-3, 0.5f, 0}, BodyType::Kinematic);
    const Entity box = box_3d(scene, {0, 0.5f, 0});
    for (int i = 0; i < 120; ++i) {
        scene.world().get<Transform>(pusher)->position.x += 0.05f;
        w->step(scene, 1.0f / 60);
    }
    CHECK(scene.world().get<Transform>(pusher)->position.x == doctest::Approx(3.0f));
    CHECK(scene.world().get<Transform>(box)->position.x > 3.4f);
}

TEST_CASE("3D teleport, velocity control and body removal") {
    Scene scene;
    auto w = make_world();
    w->set_gravity({0, 0, 0});
    CHECK(w->gravity().y == doctest::Approx(0));
    const Entity box = box_3d(scene, {0, 0, 0});
    w->sync(scene, 1.0f / 60);
    w->set_linear_velocity(box, {2, 0, 0});
    run(*w, scene, 60);
    CHECK(scene.world().get<Transform>(box)->position.x == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(w->linear_velocity(box).x == doctest::Approx(2.0f).epsilon(0.05));
    scene.world().get<Transform>(box)->position = {10, 10, 10}; // teleport
    w->step(scene, 1.0f / 60);
    CHECK(scene.world().get<Transform>(box)->position.y == doctest::Approx(10.0f).epsilon(0.01));
    w->add_impulse(box, {0, 5, 0}); // mass 1 -> +5 m/s
    CHECK(w->linear_velocity(box).y == doctest::Approx(5.0f).epsilon(0.01));
    scene.world().remove<ColliderComponent>(box);
    w->sync(scene, 1.0f / 60);
    CHECK_FALSE(w->has_body(box));
}

TEST_CASE("3D dynamic children write back local transforms") {
    Scene scene;
    auto w = make_world();
    ground_3d(scene);
    const Entity parent = scene.create("Parent");
    scene.world().get<Transform>(parent)->position = {5, 0, 0};
    const Entity child = box_3d(scene, {0, 3, 0});
    scene.set_parent(child, parent);
    run(*w, scene, 180);
    const auto* t = scene.world().get<Transform>(child);
    CHECK(t->position.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(t->position.y == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("2D dynamic box lands, preserves z and reports contacts") {
    Scene scene;
    auto w = make_world();
    const Entity ground = ground_2d(scene);
    const Entity box = box_2d(scene, {0, 3, 7});
    w->begin_frame();
    run(*w, scene, 180);
    const auto* t = scene.world().get<Transform>(box);
    CHECK(t->position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(t->position.z == doctest::Approx(7.0f));
    CHECK(has_event(w->events(), ContactEvent::Kind::Begin, ground, box, false));
    for (const auto& e : w->events()) CHECK(e.is_2d);
    CHECK(w->stats().bodies_2d == 2);
}

TEST_CASE("2D on the job system with many bodies") {
    jobs::JobSystem js(2);
    PhysicsSettings s;
    s.jobs = &js;
    Scene scene;
    auto w = make_world(s);
    ground_2d(scene);
    std::vector<Entity> boxes;
    for (int i = 0; i < 60; ++i)
        boxes.push_back(box_2d(scene, {static_cast<float>(i % 10) * 1.1f - 5, 1.0f + (i / 10) * 1.1f, 0}));
    run(*w, scene, 300);
    for (Entity b : boxes) CHECK(scene.world().get<Transform>(b)->position.y > 0.3f);
}

TEST_CASE("2D raycast, overlaps, triggers and fixed rotation") {
    Scene scene;
    auto w = make_world();
    const Entity ground = ground_2d(scene);
    const Entity trigger = scene.create("Coin");
    scene.world().get<Transform>(trigger)->position = {0, 2, 0};
    Collider2DComponent tc;
    tc.shape = Collider2DComponent::Shape::Circle;
    tc.radius = 0.4f;
    tc.is_trigger = true;
    scene.world().add<Collider2DComponent>(trigger, tc);
    const Entity player = box_2d(scene, {0, 5, 0});
    scene.world().get<RigidBody2DComponent>(player)->fixed_rotation = true;
    w->sync(scene, 1.0f / 60);
    auto hit = w->raycast_2d({3, 10}, {0, -1}, 50);
    REQUIRE(hit);
    CHECK(hit->entity == ground);
    CHECK(hit->point.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(0.01));
    // Triggers are ignored by queries.
    hit = w->raycast_2d({0, 3}, {0, -1}, 50);
    REQUIRE(hit);
    CHECK(hit->entity == ground);
    CHECK(w->overlap_circle_2d({0, 5}, 0.2f) == std::vector<Entity>{player});
    CHECK(w->overlap_box_2d({0, 2.5f}, {10, 3}).size() == 2);
    bool entered = false;
    for (int i = 0; i < 180; ++i) {
        w->begin_frame();
        w->add_torque(player, {0, 0, 50});
        w->step(scene, 1.0f / 60);
        entered |= has_event(w->events(), ContactEvent::Kind::Begin, trigger, player, true);
    }
    CHECK(entered);
    const auto* t = scene.world().get<Transform>(player);
    CHECK(t->position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(std::abs(t->rotation.z) < 1e-3f);
}

TEST_CASE("2D kinematic platform carries its own transform") {
    Scene scene;
    auto w = make_world();
    const Entity platform = scene.create("Platform");
    Collider2DComponent c;
    c.size = {3, 0.5f};
    scene.world().add<Collider2DComponent>(platform, c);
    RigidBody2DComponent rb;
    rb.type = BodyType::Kinematic;
    scene.world().add<RigidBody2DComponent>(platform, rb);
    const Entity box = box_2d(scene, {0, 1, 0});
    run(*w, scene, 60);
    for (int i = 0; i < 60; ++i) {
        scene.world().get<Transform>(platform)->position.y += 0.02f;
        w->step(scene, 1.0f / 60);
    }
    CHECK(scene.world().get<Transform>(box)->position.y == doctest::Approx(1.2f + 0.25f + 0.5f).epsilon(0.02));
}

TEST_CASE("physics components are reflected and round-trip through scene files") {
    register_components();
    for (const char* name : {"RigidBody", "Collider", "RigidBody2D", "Collider2D"}) CHECK(scene::find_schema(name));
    Scene scene;
    const Entity e = scene.create("Body");
    RigidBodyComponent rb;
    rb.type = BodyType::Kinematic;
    rb.mass = 3.5f;
    rb.lock_rotation = true;
    rb.linear_velocity = {1, 2, 3};
    ColliderComponent col;
    col.shape = ColliderComponent::Shape::Capsule;
    col.radius = 0.3f;
    col.height = 1.8f;
    col.is_trigger = true;
    col.layer = 3;
    col.mask = 0x00F0;
    scene.world().add<RigidBodyComponent>(e, rb);
    scene.world().add<ColliderComponent>(e, col);
    RigidBody2DComponent rb2;
    rb2.fixed_rotation = true;
    Collider2DComponent c2;
    c2.shape = Collider2DComponent::Shape::Circle;
    scene.world().add<RigidBody2DComponent>(e, rb2);
    scene.world().add<Collider2DComponent>(e, c2);
    const json::Value doc = scene.save();
    Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    Entity le;
    loaded.world().each<ColliderComponent>([&](Entity x, ColliderComponent&) { le = x; });
    REQUIRE_FALSE(le.is_null());
    CHECK(*loaded.world().get<RigidBodyComponent>(le) == rb);
    CHECK(*loaded.world().get<ColliderComponent>(le) == col);
    CHECK(*loaded.world().get<RigidBody2DComponent>(le) == rb2);
    CHECK(*loaded.world().get<Collider2DComponent>(le) == c2);
    // Invalid enum names are rejected.
    const auto* schema = scene::find_schema("Collider");
    ColliderComponent tmp;
    auto bad = json::parse(R"({"shape":"pyramid"})");
    REQUIRE(bad.ok());
    CHECK_FALSE(schema->load(&tmp, bad.value()).ok());
}

TEST_CASE("simulation systems bind the world and gather a tick's events") {
    Scene scene;
    auto w = make_world();
    gameplay::Simulation sim;
    add_physics_systems(sim, *w);
    const Entity ground = ground_3d(scene);
    const Entity box = box_3d(scene, {0, 0.7f, 0});
    bool began = false;
    for (int i = 0; i < 60; ++i) {
        sim.tick(scene, 1.0f / 60);
        began |= has_event(w->events(), ContactEvent::Kind::Begin, ground, box, false);
    }
    CHECK(began);
    CHECK(world_of(scene) == w.get());
}

TEST_CASE("collider wireframes are generated without a simulation") {
    Scene scene;
    ground_3d(scene);
    ground_2d(scene);
    std::vector<DebugLine> lines;
    collider_lines(scene, lines);
    CHECK(lines.size() == 12 + 4);
}
