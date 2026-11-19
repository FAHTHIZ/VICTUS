// SPDX-License-Identifier: Apache-2.0
// Destruction stage 1 (ADR-0093): deterministic Voronoi fracture, impulse-triggered breaking on the
// real Jolt backend, structural support of static walls, the debris budget and save/load.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>
#include <set>

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

std::vector<math::Vec3> box_points(math::Vec3 half) {
    std::vector<math::Vec3> p;
    for (int i = 0; i < 8; ++i) p.push_back({(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z});
    return p;
}

void ground(Scene& scene) {
    const Entity e = scene.create("Ground");
    scene.world().get<Transform>(e)->position = {0, -0.5f, 0};
    ColliderComponent c;
    c.size = {40, 1, 40};
    scene.world().add<ColliderComponent>(e, c);
}

// A static 3 x 2 x 0.3 m wall standing on the ground at the origin, broken into `pieces`.
Entity wall(Scene& scene, std::uint32_t pieces = 24) {
    const Entity e = scene.create("Wall");
    scene.world().get<Transform>(e)->position = {0, 1.0f, 0};
    ColliderComponent c;
    c.size = {3, 2, 0.3f};
    scene.world().add<ColliderComponent>(e, c);
    RigidBodyComponent rb;
    rb.type = BodyType::Static;
    scene.world().add<RigidBodyComponent>(e, rb);
    DestructibleComponent d;
    d.pieces = pieces;
    d.seed = 7;
    d.strength = 10.0f;
    d.damage_radius = 0.4f;
    scene.world().add<DestructibleComponent>(e, d);
    return e;
}

std::vector<Entity> fragments(Scene& scene) {
    std::vector<Entity> out;
    scene.world().each<DestructibleFragmentComponent>([&](Entity e, DestructibleFragmentComponent&) { out.push_back(e); });
    return out;
}

std::size_t loose(Scene& scene) {
    std::size_t n = 0;
    for (Entity e : fragments(scene))
        if (scene.world().get<RigidBodyComponent>(e)->type == BodyType::Dynamic) ++n;
    return n;
}

} // namespace

TEST_CASE("fracture: Voronoi cells of a box conserve its volume and are deterministic") {
    const auto pts = box_points({1.0f, 0.5f, 0.25f});
    auto a = fracture_convex(pts, {}, {16, 3});
    REQUIRE(a.ok());
    CHECK(a->pieces.size() == 16);
    CHECK(a->volume == doctest::Approx(1.0f).epsilon(1e-4)); // 2 x 1 x 0.5
    float sum = 0;
    for (const FracturePiece& p : a->pieces) {
        CHECK(p.volume > 0.0f);
        sum += p.volume;
        CHECK(p.indices.size() % 3 == 0);
        CHECK(p.positions.size() == p.normals.size());
        CHECK(p.positions.size() == p.colors.size());
        for (const math::Vec3& v : p.positions) {
            CHECK(std::abs(v.x) <= 1.0f + 1e-4f);
            CHECK(std::abs(v.y) <= 0.5f + 1e-4f);
            CHECK(std::abs(v.z) <= 0.25f + 1e-4f);
        }
        CHECK_FALSE(p.neighbours.empty());
    }
    CHECK(sum == doctest::Approx(1.0f).epsilon(1e-3));

    // Adjacency is symmetric.
    for (std::uint32_t i = 0; i < a->pieces.size(); ++i)
        for (std::uint32_t j : a->pieces[i].neighbours) {
            const auto& n = a->pieces[j].neighbours;
            CHECK(std::find(n.begin(), n.end(), i) != n.end());
        }

    auto b = fracture_convex(pts, {}, {16, 3});
    REQUIRE(b.ok());
    REQUIRE(b->pieces.size() == a->pieces.size());
    for (std::size_t i = 0; i < a->pieces.size(); ++i) {
        CHECK(a->pieces[i].positions == b->pieces[i].positions);
        CHECK(a->pieces[i].indices == b->pieces[i].indices);
    }
    auto c = fracture_convex(pts, {}, {16, 4});
    REQUIRE(c.ok());
    CHECK(c->pieces[0].positions != a->pieces[0].positions);
}

TEST_CASE("fracture: a convex mesh bounds the cells exactly; a concave one falls back to a polytope") {
    // Tetrahedron with outward winding.
    const std::vector<math::Vec3> tet{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const std::vector<std::uint32_t> idx{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    auto f = fracture_convex(tet, idx, {6, 1});
    REQUIRE(f.ok());
    CHECK(f->volume == doctest::Approx(1.0f / 6.0f).epsilon(1e-3));
    float sum = 0;
    for (const auto& p : f->pieces) sum += p.volume;
    CHECK(sum == doctest::Approx(f->volume).epsilon(1e-3));

    // No indices: the 90-direction polytope around a sphere is close to its volume.
    std::vector<math::Vec3> sphere;
    for (int i = 0; i < 400; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 400.0f, r = std::sqrt(1 - y * y), a = 2.39996f * static_cast<float>(i);
        sphere.push_back({std::cos(a) * r, y, std::sin(a) * r});
    }
    auto s = fracture_convex(sphere, {}, {8, 2});
    REQUIRE(s.ok());
    const float exact = 4.0f / 3.0f * 3.14159265f;
    CHECK(s->volume > exact * 0.97f);
    CHECK(s->volume < exact * 1.10f);

    CHECK_FALSE(fracture_convex({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, {}, {4, 1}).ok()); // coplanar
    CHECK_FALSE(fracture_convex({{0, 0, 0}}, {}, {4, 1}).ok());
}

TEST_CASE("destruction: a falling crate breaks on impact and its fragments inherit the motion") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity crate = scene.create("Crate");
    scene.world().get<Transform>(crate)->position = {0, 6.0f, 0};
    scene.world().add<ColliderComponent>(crate, ColliderComponent{});
    RigidBodyComponent rb;
    rb.mass = 20.0f;
    scene.world().add<RigidBodyComponent>(crate, rb);
    scene::MeshRendererComponent look;
    look.primitive = "cube";
    scene.world().add<scene::MeshRendererComponent>(crate, look);
    DestructibleComponent d;
    d.pieces = 10;
    d.strength = 100.0f; // 20 kg at ~10 m/s is ~200 N*s
    scene.world().add<DestructibleComponent>(crate, d);

    bool broke = false;
    float best_impulse = 0;
    for (int i = 0; i < 120 && !broke; ++i) {
        world->step(scene, kDt);
        for (const ContactEvent& ev : world->events())
            if (ev.kind == ContactEvent::Kind::Begin && (ev.a == crate || ev.b == crate)) best_impulse = std::max(best_impulse, ev.impulse);
        broke = scene.world().get<DestructibleComponent>(crate)->broken;
        world->begin_frame();
    }
    REQUIRE(broke);
    CHECK(best_impulse > 100.0f);
    CHECK(best_impulse < 400.0f);
    CHECK_FALSE(scene.world().get<scene::MeshRendererComponent>(crate)->visible);
    const auto parts = fragments(scene);
    CHECK(parts.size() == 10);
    CHECK(loose(scene) == 10);
    float mass = 0;
    for (Entity e : parts) {
        CHECK(scene.parent(e) == crate);
        CHECK(scene.world().get<scene::ProceduralMeshComponent>(e)->mesh != nullptr);
        CHECK(scene.world().get<scene::MeshRendererComponent>(e)->primitive == "cube"); // look copied
        mass += scene.world().get<RigidBodyComponent>(e)->mass;
        CHECK(world->has_body(e));
    }
    CHECK(mass == doctest::Approx(20.0f).epsilon(0.01));
    world->step(scene, kDt);
    CHECK_FALSE(world->has_body(crate));
    // Settle: everything comes to rest on the ground.
    for (int i = 0; i < 240; ++i) world->step(scene, kDt);
    scene.update_transforms();
    for (Entity e : parts) {
        const math::Mat4 m = scene.world_matrix(e);
        CHECK(m.c[3].y > -0.2f);
        CHECK(m.c[3].y < 1.5f);
    }
}

TEST_CASE("destruction: below its strength nothing breaks") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity w = wall(scene);
    world->damage(w, {0, 1, 0.15f}, 5.0f);
    world->step(scene, kDt);
    CHECK_FALSE(scene.world().get<DestructibleComponent>(w)->broken);
    CHECK(fragments(scene).empty());
}

TEST_CASE("destruction: a wall frees the pieces around the hit and keeps its supported part standing") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity w = wall(scene, 24);
    world->step(scene, kDt);
    REQUIRE(world->has_body(w));
    // Hit near the top centre.
    world->damage(w, {0, 1.8f, 0.15f}, 50.0f);
    world->step(scene, kDt);
    REQUIRE(scene.world().get<DestructibleComponent>(w)->broken);
    const auto parts = fragments(scene);
    REQUIRE(parts.size() == 24);
    const std::size_t freed = loose(scene);
    CHECK(freed >= 1);
    CHECK(freed < 24);
    // Every static fragment is connected to the base through other static fragments: a second
    // support pass frees nothing more.
    for (int i = 0; i < 90; ++i) world->step(scene, kDt);
    CHECK(loose(scene) == freed);
    // Static fragments did not move; freed ones fell and their bodies are dynamic.
    scene.update_transforms();
    for (Entity e : parts) {
        const auto* rb = scene.world().get<RigidBodyComponent>(e);
        const auto* f = scene.world().get<DestructibleFragmentComponent>(e);
        CHECK(f->owner == scene.uuid(w));
        if (rb->type == BodyType::Static) CHECK(f->age == 0.0f);
        else CHECK(f->age > 1.0f);
    }

    // Knocking out the whole bottom row drops everything above it.
    for (float x = -1.5f; x <= 1.5f; x += 0.2f) world->damage(w, {x, 0.05f, 0.15f}, 50.0f);
    world->step(scene, kDt);
    std::size_t anchored = 0;
    for (Entity e : fragments(scene))
        if (scene.world().get<RigidBodyComponent>(e)->type == BodyType::Static) ++anchored;
    CHECK(anchored == 0);
}

TEST_CASE("destruction: a projectile contact breaks a wall through the contact impulse") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity w = wall(scene, 16);
    const Entity ball = scene.create("Ball");
    scene.world().get<Transform>(ball)->position = {0, 1.2f, 3.0f};
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Sphere;
    c.radius = 0.15f;
    scene.world().add<ColliderComponent>(ball, c);
    RigidBodyComponent rb;
    rb.mass = 5.0f;
    rb.gravity_scale = 0.0f;
    rb.continuous = true;
    scene.world().add<RigidBodyComponent>(ball, rb);
    world->step(scene, kDt);
    world->set_linear_velocity(ball, {0, 0, -20.0f}); // 100 N*s on a static wall
    for (int i = 0; i < 30 && !scene.world().get<DestructibleComponent>(w)->broken; ++i) {
        world->step(scene, kDt);
        world->begin_frame();
    }
    CHECK(scene.world().get<DestructibleComponent>(w)->broken);
    CHECK(loose(scene) >= 1);
}

TEST_CASE("destruction: debris lifetime and max_debris retire freed fragments") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity crate = scene.create("Crate");
    scene.world().get<Transform>(crate)->position = {0, 0.5f, 0};
    scene.world().add<ColliderComponent>(crate, ColliderComponent{});
    scene.world().add<RigidBodyComponent>(crate, RigidBodyComponent{});
    DestructibleComponent d;
    d.pieces = 12;
    d.max_debris = 5;
    d.debris_lifetime = 0.5f;
    d.strength = 1.0f;
    scene.world().add<DestructibleComponent>(crate, d);
    world->step(scene, kDt);
    world->damage(crate, {0, 0.5f, 0}, 10.0f);
    world->step(scene, kDt);
    CHECK(fragments(scene).size() == 5); // capped in the step that broke it
    for (int i = 0; i < 40; ++i) world->step(scene, kDt);
    CHECK(fragments(scene).empty()); // all expired
}

TEST_CASE("destruction: a broken wall saves and loads with its fragments regenerated") {
    auto world = make_world();
    Scene scene;
    ground(scene);
    const Entity w = wall(scene, 12);
    world->step(scene, kDt);
    world->damage(w, {1.4f, 1.9f, 0.15f}, 50.0f);
    world->step(scene, kDt);
    REQUIRE(scene.world().get<DestructibleComponent>(w)->broken);
    std::set<std::uint64_t> ids;
    for (Entity e : fragments(scene)) ids.insert(scene.world().get<scene::ProceduralMeshComponent>(e)->mesh->id);
    const std::size_t freed = loose(scene);
    const json::Value doc = scene.save();

    Scene copy;
    REQUIRE(copy.load(doc).ok());
    const auto parts = fragments(copy);
    REQUIRE(parts.size() == 12);
    for (Entity e : parts) CHECK(copy.world().get<scene::ProceduralMeshComponent>(e) == nullptr);
    auto world2 = make_world();
    CHECK(world2->restore_fragments(copy) == 12);
    CHECK(loose(copy) == freed);
    for (Entity e : parts) {
        const auto* pm = copy.world().get<scene::ProceduralMeshComponent>(e);
        REQUIRE(pm);
        const auto* f = copy.world().get<DestructibleFragmentComponent>(e);
        // Same geometry as before the save (deterministic fracture).
        const Entity orig = fragments(scene)[f->piece];
        const auto* before = scene.world().get<scene::ProceduralMeshComponent>(orig)->mesh.get();
        const auto* bf = scene.world().get<DestructibleFragmentComponent>(orig);
        if (bf->piece == f->piece) CHECK(pm->mesh->positions == before->positions);
    }
    world2->step(copy, kDt);
    CHECK_FALSE(world2->has_body(copy.find(scene.uuid(w))));
    for (Entity e : parts) CHECK(world2->has_body(e));
}
