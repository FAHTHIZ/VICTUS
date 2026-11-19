// SPDX-License-Identifier: Apache-2.0
// Convex-hull and triangle-mesh colliders: geometry sources (procedural meshes, providers),
// shared shape building, scaling, the dynamic-mesh fallback, invalidation and debug lines.
#include "victus/physics/physics.hpp"
#include "victus/core/jobs.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <thread>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;

namespace {

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create();
    REQUIRE(w.ok());
    return std::move(w.value());
}

void run(PhysicsWorld& w, Scene& scene, int steps) {
    for (int i = 0; i < steps; ++i) w.step(scene, 1.0f / 60.0f);
}

// Axis-aligned quad in XZ at height y (two counter-clockwise triangles facing up).
void add_quad(std::vector<math::Vec3>& p, std::vector<std::uint32_t>& idx, float x0, float z0, float x1, float z1, float y) {
    const auto b = static_cast<std::uint32_t>(p.size());
    p.insert(p.end(), {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}});
    idx.insert(idx.end(), {b, b + 2, b + 1, b, b + 3, b + 2});
}

// Unit cube (-0.5..0.5) as 24 vertices (duplicated per face, like a real imported mesh).
std::shared_ptr<const CollisionMesh> cube_mesh(float half = 0.5f) {
    std::vector<math::Vec3> p;
    std::vector<std::uint32_t> idx;
    const math::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a)
        for (float sgn : {-1.0f, 1.0f}) {
            const math::Vec3 n = axes[a] * sgn, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            const auto b = static_cast<std::uint32_t>(p.size());
            for (int k = 0; k < 4; ++k) {
                const float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = k >= 2 ? 1.0f : -1.0f;
                p.push_back((n + u * su + v * sv) * half);
            }
            idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        }
    auto m = make_collision_mesh(std::move(p), std::move(idx));
    REQUIRE(m.ok());
    return *m;
}

// Two 10x10 floor slabs at y=0 with a 2 m gap along x (concave as a whole).
std::shared_ptr<const scene::ProceduralMesh> gapped_floor() {
    scene::ProceduralMesh pm;
    std::vector<std::uint32_t> idx;
    add_quad(pm.positions, idx, -11, -5, -1, 5, 0);
    add_quad(pm.positions, idx, 1, -5, 11, 5, 0);
    pm.indices = std::move(idx);
    auto m = scene::publish_mesh(std::move(pm));
    REQUIRE(m.ok());
    return *m;
}

Entity mesh_entity(Scene& scene, std::shared_ptr<const scene::ProceduralMesh> mesh, ColliderComponent::Shape shape,
                   math::Vec3 pos = {}) {
    const Entity e = scene.create("Mesh");
    scene.world().get<Transform>(e)->position = pos;
    scene.world().add<scene::ProceduralMeshComponent>(e, scene::ProceduralMeshComponent{std::move(mesh)});
    ColliderComponent c;
    c.shape = shape;
    scene.world().add<ColliderComponent>(e, c);
    return e;
}

Entity dynamic_box(Scene& scene, math::Vec3 p) {
    const Entity e = scene.create("Box");
    scene.world().get<Transform>(e)->position = p;
    scene.world().add<ColliderComponent>(e, ColliderComponent{});
    scene.world().add<RigidBodyComponent>(e, RigidBodyComponent{});
    return e;
}

} // namespace

TEST_CASE("make_collision_mesh validates indices and computes bounds") {
    auto ok = make_collision_mesh({{0, 0, 0}, {1, 2, 0}, {0, 0, 3}}, {0, 1, 2});
    REQUIRE(ok.ok());
    CHECK((*ok)->bounds.min.x == doctest::Approx(0));
    CHECK((*ok)->bounds.max.y == doctest::Approx(2));
    CHECK((*ok)->bounds.max.z == doctest::Approx(3));
    CHECK_FALSE(make_collision_mesh({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, {0, 1}).ok());
    CHECK_FALSE(make_collision_mesh({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, {0, 1, 3}).ok());
    CHECK_FALSE(make_collision_mesh({{0, 0, 0}, {NAN, 0, 0}, {0, 0, 1}}, {0, 1, 2}).ok());
}

TEST_CASE("a static triangle-mesh floor supports bodies and keeps its concave gap") {
    Scene scene;
    auto w = make_world();
    const Entity floor = mesh_entity(scene, gapped_floor(), ColliderComponent::Shape::Mesh);
    const Entity box = dynamic_box(scene, {5, 3, 0});
    const Entity faller = dynamic_box(scene, {0, 3, 0}); // above the gap
    run(*w, scene, 180);
    CHECK(w->has_body(floor));
    CHECK(scene.world().get<Transform>(box)->position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(scene.world().get<Transform>(faller)->position.y < -2.0f); // fell through the gap
    // Ray straight down in the gap misses; on a slab it hits the mesh with an upward normal.
    CHECK_FALSE(w->raycast({0, 5, 3}, {0, -1, 0}, 20, kAllLayers, faller).has_value());
    const auto hit = w->raycast({-5, 5, 3}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->point.y == doctest::Approx(0).epsilon(0.01));
    CHECK(hit->normal.y == doctest::Approx(1).epsilon(0.01));
}

TEST_CASE("a convex collider wraps the same geometry in its hull") {
    Scene scene;
    auto w = make_world();
    const Entity floor = mesh_entity(scene, gapped_floor(), ColliderComponent::Shape::Convex);
    run(*w, scene, 1);
    // The hull of two coplanar slabs is a flat box spanning the gap (convex radius makes it thin).
    const auto hit = w->raycast({0, 5, 3}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->point.y == doctest::Approx(0).epsilon(0.02));
}

TEST_CASE("dynamic convex bodies fall and rest on their hull; a dynamic mesh collider falls back to it") {
    for (const auto shape : {ColliderComponent::Shape::Convex, ColliderComponent::Shape::Mesh}) {
        CAPTURE(static_cast<int>(shape));
        Scene scene;
        auto w = make_world();
        const Entity ground = scene.create("Ground");
        scene.world().get<Transform>(ground)->position = {0, -0.5f, 0};
        ColliderComponent gc;
        gc.size = {20, 1, 20};
        scene.world().add<ColliderComponent>(ground, gc);

        scene::ProceduralMesh pm;
        const auto cube = cube_mesh();
        pm.positions = cube->positions;
        pm.indices = cube->indices;
        const Entity body = mesh_entity(scene, *scene::publish_mesh(std::move(pm)), shape, {0, 2, 0});
        scene.world().add<RigidBodyComponent>(body, RigidBodyComponent{});
        run(*w, scene, 180);
        CHECK(w->has_body(body));
        CHECK(scene.world().get<Transform>(body)->position.y == doctest::Approx(0.5f).epsilon(0.03));
    }
}

TEST_CASE("providers resolve model meshes once per source and shapes are shared") {
    Scene scene;
    auto w = make_world();
    int calls = 0;
    const Uuid model = Uuid::generate();
    w->set_mesh_provider([&](const CollisionMeshSource& src) -> std::shared_ptr<const CollisionMesh> {
        ++calls;
        if (src.model != model || src.mesh_index != 0) return nullptr;
        return cube_mesh();
    });
    std::vector<Entity> pillars;
    for (int i = 0; i < 3; ++i) {
        const Entity e = scene.create("Pillar");
        scene.world().get<Transform>(e)->position = {static_cast<float>(i) * 3.0f, 0, 0};
        ColliderComponent c;
        c.shape = ColliderComponent::Shape::Convex;
        c.mesh_asset = model;
        scene.world().add<ColliderComponent>(e, c);
        pillars.push_back(e);
    }
    // A MeshRenderer model is used when the collider names no mesh; a missing mesh index gives no body.
    const Entity rendered = scene.create("Rendered");
    scene.world().get<Transform>(rendered)->position = {0, 0, 5};
    scene::MeshRendererComponent mr;
    mr.mesh_asset = model;
    scene.world().add<scene::MeshRendererComponent>(rendered, mr);
    ColliderComponent rc;
    rc.shape = ColliderComponent::Shape::Mesh;
    scene.world().add<ColliderComponent>(rendered, rc);
    const Entity missing = scene.create("Missing");
    ColliderComponent mc = rc;
    mc.mesh_asset = model;
    mc.mesh_index = 7;
    scene.world().add<ColliderComponent>(missing, mc);

    run(*w, scene, 10);
    for (const Entity e : pillars) CHECK(w->has_body(e));
    CHECK(w->has_body(rendered));
    CHECK_FALSE(w->has_body(missing));
    CHECK(calls == 2); // (model, 0) and (model, 7), each once
    CHECK(w->stats().mesh_shapes_built == 2); // one hull shared by the pillars, one triangle mesh
    const auto hit = w->raycast({3, 5, 0}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == pillars[1]);
    CHECK(hit->point.y == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("mesh colliders follow non-uniform scale and offset") {
    Scene scene;
    auto w = make_world();
    w->set_mesh_provider([](const CollisionMeshSource&) { return cube_mesh(); });
    const Entity e = scene.create("Tall");
    scene.world().get<Transform>(e)->scale = {2, 3, 1};
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Mesh;
    c.mesh_asset = Uuid::generate();
    c.offset = {0, 0.5f, 0}; // local units, scaled like the other shapes
    scene.world().add<ColliderComponent>(e, c);
    run(*w, scene, 1);
    const auto top = w->raycast({0.9f, 10, 0}, {0, -1, 0}, 20);
    REQUIRE(top.has_value());
    CHECK(top->point.y == doctest::Approx(3.0f).epsilon(0.01)); // (0.5 + 0.5) * 3
    CHECK_FALSE(w->raycast({1.1f, 10, 0}, {0, -1, 0}, 20).has_value()); // half width 1
    const auto side = w->raycast({0, 1.5f, 5}, {0, 0, -1}, 20);
    REQUIRE(side.has_value());
    CHECK(side->point.z == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("mirrored mesh colliders collide mirrored") {
    for (const auto shape : {ColliderComponent::Shape::Mesh, ColliderComponent::Shape::Convex}) {
        CAPTURE(static_cast<int>(shape));
        scene::ProceduralMesh pm;
        std::vector<std::uint32_t> idx;
        add_quad(pm.positions, idx, 1, -1, 3, 1, 0); // only on +x
        pm.indices = idx;
        Scene scene;
        auto w = make_world();
        const Entity e = mesh_entity(scene, *scene::publish_mesh(std::move(pm)), shape);
        scene.world().get<Transform>(e)->scale = {-1, 1, 1};
        run(*w, scene, 1);
        CHECK_FALSE(w->raycast({2, 5, 0}, {0, -1, 0}, 20).has_value());
        const auto hit = w->raycast({-2, 5, 0}, {0, -1, 0}, 20);
        REQUIRE(hit.has_value());
        CHECK(hit->point.y == doctest::Approx(0).epsilon(0.02));
        if (shape == ColliderComponent::Shape::Mesh) CHECK(hit->normal.y > 0.99f); // winding flipped back up
    }
}

TEST_CASE("colliders without geometry make no body until geometry appears; invalidation rebuilds") {
    Scene scene;
    auto w = make_world();
    const Entity e = scene.create("Late");
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Convex;
    scene.world().add<ColliderComponent>(e, c);
    run(*w, scene, 2);
    CHECK_FALSE(w->has_body(e));

    float half = 0.5f;
    w->set_mesh_provider([&](const CollisionMeshSource& src) {
        return src.primitive == "cube" ? cube_mesh(half) : nullptr;
    });
    scene::MeshRendererComponent mr;
    mr.primitive = "cube";
    scene.world().add<scene::MeshRendererComponent>(e, mr);
    run(*w, scene, 1);
    REQUIRE(w->has_body(e));
    auto hit = w->raycast({0, 10, 0}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(0.5f).epsilon(0.01));

    half = 1.0f; // the "model" was edited: cached geometry stays until invalidated
    run(*w, scene, 1);
    CHECK(w->raycast({0, 10, 0}, {0, -1, 0}, 20)->point.y == doctest::Approx(0.5f).epsilon(0.01));
    w->invalidate_mesh(Uuid{});  // primitives are keyed by the nil model
    run(*w, scene, 1);
    hit = w->raycast({0, 10, 0}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(0.01));

    // Removing the geometry removes the body.
    scene.world().remove<scene::MeshRendererComponent>(e);
    run(*w, scene, 1);
    CHECK_FALSE(w->has_body(e));
}

TEST_CASE("replacing a procedural mesh rebuilds the body") {
    Scene scene;
    auto w = make_world();
    const Entity e = mesh_entity(scene, gapped_floor(), ColliderComponent::Shape::Mesh);
    run(*w, scene, 1);
    CHECK_FALSE(w->raycast({0, 5, 0}, {0, -1, 0}, 20).has_value());
    scene::ProceduralMesh pm;
    std::vector<std::uint32_t> idx;
    add_quad(pm.positions, idx, -5, -5, 5, 5, 1.0f);
    pm.indices = idx;
    scene.world().get<scene::ProceduralMeshComponent>(e)->mesh = *scene::publish_mesh(std::move(pm));
    run(*w, scene, 1);
    const auto hit = w->raycast({0, 5, 0}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("convex and mesh colliders reflect, round-trip and draw their triangles") {
    register_components();
    Scene scene;
    const Entity e = scene.create("Rock");
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Convex;
    c.mesh_asset = Uuid::generate();
    c.mesh_index = 2;
    scene.world().add<ColliderComponent>(e, c);
    Scene loaded;
    REQUIRE(loaded.load(scene.save()).ok());
    Entity le;
    loaded.world().each<ColliderComponent>([&](Entity x, ColliderComponent&) { le = x; });
    REQUIRE_FALSE(le.is_null());
    const auto* lc = loaded.world().get<ColliderComponent>(le);
    REQUIRE(lc);
    CHECK(*lc == c);

    std::vector<DebugLine> lines;
    collider_lines(scene, lines);
    CHECK(lines.empty()); // no cache: nothing to draw
    CollisionMeshCache cache([](const CollisionMeshSource&) { return cube_mesh(); });
    collider_lines(scene, lines, {}, &cache);
    CHECK(lines.size() == 12 * 3);
    CHECK(cache.size() == 1);
    cache.invalidate(c.mesh_asset);
    CHECK(cache.size() == 0);
}

namespace {

// An n x n grid of quads at y = 0 spanning -10..10 (2 n^2 triangles).
std::shared_ptr<const scene::ProceduralMesh> grid_floor(int n) {
    scene::ProceduralMesh pm;
    std::vector<std::uint32_t> idx;
    const float step = 20.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float x = -10.0f + step * static_cast<float>(i), z = -10.0f + step * static_cast<float>(j);
            add_quad(pm.positions, idx, x, z, x + step, z + step, 0);
        }
    pm.indices = std::move(idx);
    auto m = scene::publish_mesh(std::move(pm));
    REQUIRE(m.ok());
    return *m;
}

// Steps until `e` has a body (background builds finish on worker threads), at most ~10 s.
int steps_until_body(PhysicsWorld& w, Scene& scene, Entity e) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int steps = 0;
    while (!w.has_body(e) && std::chrono::steady_clock::now() < deadline) {
        w.step(scene, 1.0f / 60.0f);
        ++steps;
        if (!w.has_body(e)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return steps;
}

} // namespace

TEST_CASE("large mesh colliders build their shapes in the background") {
    jobs::JobSystem js(2);
    PhysicsSettings ps;
    ps.jobs = &js;
    auto created = PhysicsWorld::create(ps);
    REQUIRE(created.ok());
    PhysicsWorld& w = *created.value();
    Scene scene;
    const auto big = grid_floor(60); // 7200 triangles > the 4096 default
    const Entity floor = mesh_entity(scene, big, ColliderComponent::Shape::Mesh);
    const Entity hull = mesh_entity(scene, big, ColliderComponent::Shape::Convex, {0, -50, 0});
    const Entity box = dynamic_box(scene, {5, 3, 0});
    // The first step only starts the builds: no bodies yet, the frame is not stalled.
    w.step(scene, 1.0f / 60.0f);
    CHECK_FALSE(w.has_body(floor));
    CHECK_FALSE(w.has_body(hull));
    CHECK(w.has_body(box));
    CHECK(w.stats().mesh_shapes_pending == 2);
    CHECK(w.stats().mesh_shapes_built == 0);
    const int steps = steps_until_body(w, scene, floor);
    CHECK(w.has_body(floor));
    MESSAGE("background mesh shape ready after " << steps << " steps");
    (void)steps_until_body(w, scene, hull);
    CHECK(w.has_body(hull));
    CHECK(w.stats().mesh_shapes_pending == 0);
    CHECK(w.stats().mesh_shapes_built == 2);
    const auto hit = w.raycast({-5, 5, 3}, {0, -1, 0}, 20);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->point.y == doctest::Approx(0).epsilon(0.01));
    // A second entity sharing the mesh reuses the finished shape at once.
    const Entity twin = mesh_entity(scene, big, ColliderComponent::Shape::Mesh, {0, -20, 0});
    w.step(scene, 1.0f / 60.0f);
    CHECK(w.has_body(twin));
    CHECK(w.stats().mesh_shapes_built == 2);
    // A box dropped once the floor exists lands on it (the first box fell meanwhile: bodies
    // over a still-building mesh have nothing to stand on). It drops away from the first box,
    // which may still be above the floor when a fast build finishes and would catch it.
    const Entity lander = dynamic_box(scene, {-5, 3, 3});
    run(w, scene, 180);
    CHECK(scene.world().get<Transform>(lander)->position.y == doctest::Approx(0.5f).epsilon(0.03));
}

TEST_CASE("background shape builds respect the threshold and outlive their world safely") {
    jobs::JobSystem js(2);
    const auto big = grid_floor(60);
    {
        // Threshold 0 (or no job system): built during the first step, as before.
        PhysicsSettings ps;
        ps.jobs = &js;
        ps.async_mesh_triangles = 0;
        auto w = PhysicsWorld::create(ps);
        REQUIRE(w.ok());
        Scene scene;
        const Entity floor = mesh_entity(scene, big, ColliderComponent::Shape::Mesh);
        w.value()->step(scene, 1.0f / 60.0f);
        CHECK(w.value()->has_body(floor));
        CHECK(w.value()->stats().mesh_shapes_pending == 0);
    }
    {
        // Small meshes stay synchronous under the default threshold.
        PhysicsSettings ps;
        ps.jobs = &js;
        auto w = PhysicsWorld::create(ps);
        REQUIRE(w.ok());
        Scene scene;
        const Entity floor = mesh_entity(scene, gapped_floor(), ColliderComponent::Shape::Mesh);
        w.value()->step(scene, 1.0f / 60.0f);
        CHECK(w.value()->has_body(floor));
    }
    {
        // Destroying the world (or removing the collider) with builds in flight waits for them.
        PhysicsSettings ps;
        ps.jobs = &js;
        ps.async_mesh_triangles = 1;
        auto w = PhysicsWorld::create(ps);
        REQUIRE(w.ok());
        Scene scene;
        const Entity a = mesh_entity(scene, grid_floor(80), ColliderComponent::Shape::Convex);
        (void)mesh_entity(scene, grid_floor(80), ColliderComponent::Shape::Mesh);
        w.value()->step(scene, 1.0f / 60.0f);
        CHECK(w.value()->stats().mesh_shapes_pending == 2);
        scene.world().remove<ColliderComponent>(a);
        w.value()->step(scene, 1.0f / 60.0f);
        CHECK_FALSE(w.value()->has_body(a));
    }
}
