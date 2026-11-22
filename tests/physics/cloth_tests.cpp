// SPDX-License-Identifier: Apache-2.0
// Cloth tests (ADR-0205): Jolt soft-body sheets driven through PhysicsWorld::step at 60 Hz.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

Entity cloth(Scene& scene, math::Vec3 at, ClothComponent c = {}) {
    const Entity e = scene.create("Cloth");
    scene.world().get<Transform>(e)->position = at;
    scene.world().add<scene::MeshRendererComponent>(e);
    scene.world().add<ClothComponent>(e, c);
    return e;
}

math::Aabb mesh_bounds(Scene& scene, Entity e) {
    const auto* pm = scene.world().get<scene::ProceduralMeshComponent>(e);
    REQUIRE(pm);
    REQUIRE(pm->mesh);
    return pm->mesh->bounds;
}

float lowest_y(Scene& scene, Entity e) {
    const auto* pm = scene.world().get<scene::ProceduralMeshComponent>(e);
    REQUIRE(pm);
    float y = std::numeric_limits<float>::max();
    for (const math::Vec3& p : pm->mesh->positions) y = std::min(y, p.y);
    return y;
}

} // namespace

TEST_CASE("cloth rest mesh is a double-sided grid in the local XY plane") {
    ClothComponent c;
    c.size = {2, 1};
    c.segments_x = 4;
    c.segments_y = 2;
    auto m = cloth_rest_mesh(c);
    REQUIRE(m);
    CHECK(m->positions.size() == 2u * 5u * 3u);
    CHECK(m->indices.size() == 2u * 4u * 2u * 6u);
    CHECK(m->bounds.min.x == doctest::Approx(-1.0f));
    CHECK(m->bounds.max.y == doctest::Approx(0.5f));
    // Front faces +Z, back faces -Z.
    CHECK(m->normals.front().z == doctest::Approx(1.0f));
    CHECK(m->normals.back().z == doctest::Approx(-1.0f));
    c.double_sided = false;
    CHECK(cloth_rest_mesh(c)->positions.size() == 15u);
    c.size = {0, 1};
    CHECK(cloth_rest_mesh(c) == nullptr);
}

TEST_CASE("an unpinned cloth falls and drapes over a box") {
    Scene scene;
    auto world = make_world();
    const Entity floor = scene.create("Floor");
    scene.world().get<Transform>(floor)->position = {0, -0.5f, 0};
    scene.world().add<ColliderComponent>(floor, ColliderComponent{.size = {20, 1, 20}});
    const Entity table = scene.create("Table");
    scene.world().get<Transform>(table)->position = {0, 0.5f, 0};
    scene.world().add<ColliderComponent>(table, ColliderComponent{.size = {1, 1, 1}});

    ClothComponent c;
    c.pin = ClothComponent::Pin::None;
    c.size = {2, 2};
    c.segments_x = c.segments_y = 12;
    // Lay it flat above the table: rotate the XY sheet onto XZ.
    const Entity e = cloth(scene, {0, 1.5f, 0}, c);
    scene.world().get<Transform>(e)->rotation = math::Quat::axis_angle({1, 0, 0}, -math::kPi * 0.5f);
    for (int i = 0; i < 240; ++i) world->step(scene, kDt);

    const ClothState st = world->cloth_state(e);
    CHECK(st.simulated);
    CHECK(st.particles == 13u * 13u);
    CHECK(st.pinned == 0u);
    // Entity-local -Z is world -Y after the rotation: the centre rests on the table top (y = 1, local
    // z = -0.5), the corners hang below it but stay above the floor (local z = -1.5).
    const auto* pm = scene.world().get<scene::ProceduralMeshComponent>(e);
    REQUIRE(pm);
    const math::Vec3 centre = pm->mesh->positions[6 * 13 + 6];
    CHECK(centre.z == doctest::Approx(-0.5f).epsilon(0.08));
    const math::Vec3 corner = pm->mesh->positions[0];
    CHECK(corner.z < -0.7f);  // draped down past the table edge
    CHECK(corner.z > -1.55f); // but not through the floor
    const PhysicsStats stats = world->stats();
    CHECK(stats.cloths == 1u);
    CHECK(stats.cloth_particles == 169u);
}

TEST_CASE("a top-pinned banner hangs from its entity and follows it") {
    Scene scene;
    auto world = make_world();
    ClothComponent c;
    c.size = {1, 1};
    c.segments_x = c.segments_y = 8;
    const Entity e = cloth(scene, {0, 3, 0}, c);
    for (int i = 0; i < 120; ++i) world->step(scene, kDt);
    CHECK(world->cloth_state(e).pinned == 9u);
    const math::Aabb b = mesh_bounds(scene, e);
    // The top edge stays at local y = 0.5; the bottom hangs no lower than the sheet height allows.
    CHECK(b.max.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(lowest_y(scene, e) > -0.6f);
    CHECK(lowest_y(scene, e) < -0.4f);

    // Carry the banner sideways: the pins follow, the sheet stays attached (local bounds unchanged).
    for (int i = 0; i < 60; ++i) {
        scene.world().get<Transform>(e)->position.x += 0.02f;
        world->step(scene, kDt);
    }
    const math::Aabb moved = mesh_bounds(scene, e);
    CHECK(moved.max.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(moved.min.x > -0.9f);
    CHECK(moved.max.x < 0.9f);
}

TEST_CASE("wind blows a flag out from its pole and impulses push particles") {
    Scene scene;
    auto world = make_world();
    ClothComponent c;
    c.size = {1.5f, 1};
    c.segments_x = 12;
    c.segments_y = 8;
    c.pin = ClothComponent::Pin::LeftEdge;
    c.mass = 0.3f;
    const Entity still = cloth(scene, {0, 5, 0}, c);
    c.wind = {8, 0, 0};
    const Entity windy = cloth(scene, {0, 5, 10}, c);
    for (int i = 0; i < 180; ++i) world->step(scene, kDt);
    // Without wind the flag keeps its plane; wind along it makes it flutter out of the plane.
    const math::Aabb calm = mesh_bounds(scene, still);
    const math::Aabb blown = mesh_bounds(scene, windy);
    CHECK(std::max(std::abs(calm.min.z), std::abs(calm.max.z)) < 0.01f);
    CHECK(blown.max.z - blown.min.z > 0.1f);
    CHECK(blown.max.x > 0.3f); // still streams out from the pole
    CHECK(!world->cloth_state(windy).sleeping);

    // A wind across the flag pushes it downwind.
    scene.world().get<ClothComponent>(windy)->wind = {0, 0, 8};
    for (int i = 0; i < 120; ++i) world->step(scene, kDt);
    CHECK(mesh_bounds(scene, windy).max.z > 0.4f);

    // A sideways hit moves the calm flag's particles along +Z.
    const float before = mesh_bounds(scene, still).max.z;
    world->add_cloth_impulse(still, {0, 5, 0}, {0, 0, 5}, 3.0f);
    for (int i = 0; i < 6; ++i) world->step(scene, kDt);
    CHECK(mesh_bounds(scene, still).max.z > before + 0.05f);
}

TEST_CASE("cloth changes: live fields keep the body, shape fields rebuild, removal destroys") {
    Scene scene;
    auto world = make_world();
    const Entity e = cloth(scene, {0, 2, 0});
    world->step(scene, kDt);
    const std::uint32_t bodies = world->stats().bodies_3d;
    CHECK(bodies == 1u);
    scene.world().get<ClothComponent>(e)->damping = 0.5f;
    scene.world().get<ClothComponent>(e)->wind = {1, 0, 0};
    world->step(scene, kDt);
    CHECK(world->stats().bodies_3d == bodies);
    scene.world().get<ClothComponent>(e)->segments_x = 4;
    world->step(scene, kDt);
    CHECK(world->cloth_state(e).particles == 5u * 17u);
    scene.world().get<ClothComponent>(e)->size = {0, 0};
    world->step(scene, kDt);
    CHECK(!world->cloth_state(e).simulated);
    CHECK(world->cloth_state(e).reason == "size must be positive");
    scene.world().remove<ClothComponent>(e);
    world->step(scene, kDt);
    CHECK(world->stats().bodies_3d == 0u);
    CHECK(world->stats().cloths == 0u);
}

TEST_CASE("identical cloths are deterministic") {
    auto run = [] {
        Scene scene;
        auto world = make_world();
        ClothComponent c;
        c.wind = {3, 0, 1};
        const Entity e = cloth(scene, {0, 2, 0}, c);
        for (int i = 0; i < 90; ++i) world->step(scene, kDt);
        return scene.world().get<scene::ProceduralMeshComponent>(e)->mesh->positions;
    };
    CHECK(run() == run());
}

TEST_CASE("Cloth serializes through the component registry") {
    register_components();
    Scene scene;
    ClothComponent c;
    c.size = {3, 1.5f};
    c.pin = ClothComponent::Pin::Corners;
    c.wind = {2, 0, 0};
    const Entity e = cloth(scene, {}, c);
    const json::Value saved = scene.save();
    Scene loaded;
    REQUIRE(loaded.load(saved).ok());
    bool found = false;
    loaded.world().each<ClothComponent>([&](Entity, ClothComponent& got) {
        found = true;
        CHECK(got == c);
    });
    CHECK(found);
    (void)e;
}
