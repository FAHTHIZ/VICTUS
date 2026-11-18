// SPDX-License-Identifier: Apache-2.0
// Navigation tests (Slice 16): navmesh generation from static colliders (Recast), path/nearest/
// raycast queries (Detour) and crowd agents, including CharacterController agents driven through
// the physics character mover.
#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>

using namespace victus;
using namespace victus::navigation;
using physics::ColliderComponent;
using scene::Scene;
using scene::Transform;

namespace {

Entity box(Scene& s, math::Vec3 center, math::Vec3 size, const char* name = "Box") {
    const Entity e = s.create(name);
    s.world().get<Transform>(e)->position = center;
    ColliderComponent c;
    c.size = size;
    s.world().add<ColliderComponent>(e, c);
    return e;
}

// 20 x 20 floor with its top at y = 0 and a wall across x = 0 from z = -6 to z = 10 (open at z < -6).
void arena(Scene& s, bool wall = true) {
    box(s, {0, -0.5f, 0}, {20, 1, 20}, "Floor");
    if (wall) box(s, {0, 1, 2}, {1, 2, 16}, "Wall");
}

float horizontal(math::Vec3 a, math::Vec3 b) { return std::hypot(a.x - b.x, a.z - b.z); }

float path_length(const std::vector<math::Vec3>& p) {
    float l = 0;
    for (std::size_t i = 1; i < p.size(); ++i) l += math::length(p[i] - p[i - 1]);
    return l;
}

} // namespace

TEST_CASE("navigation: navmesh builds from static colliders and paths go around obstacles") {
    Scene s;
    arena(s);
    NavigationSystem nav;
    REQUIRE(nav.build(s).ok());
    CHECK(nav.valid());
    CHECK(nav.stats().source_triangles == 24);
    CHECK(nav.stats().polygons > 0);

    const auto ground = nav.nearest({-5, 3, 5}, {2, 4, 2});
    REQUIRE(ground.has_value());
    CHECK(std::fabs(ground->y) < 0.25f);

    std::vector<math::Vec3> path;
    bool partial = true;
    REQUIRE(nav.find_path({-5, 0, 5}, {5, 0, 5}, path, &partial));
    CHECK_FALSE(partial);
    REQUIRE(path.size() >= 3); // has to turn around the wall end
    CHECK(horizontal(path.front(), {-5, 0, 5}) < 0.3f);
    CHECK(horizontal(path.back(), {5, 0, 5}) < 0.3f);
    // Detour goes through the opening at z < -6 (wall spans z in [-6, 10]).
    bool via_gap = false;
    for (const auto& p : path) via_gap |= p.z < -6.0f;
    CHECK(via_gap);
    CHECK(path_length(path) > 2 * std::hypot(5.0f, 11.0f) - 1.0f);

    // Without the wall the path is a straight line.
    Scene open;
    arena(open, false);
    NavigationSystem nav2;
    REQUIRE(nav2.build(open).ok());
    REQUIRE(nav2.find_path({-5, 0, 5}, {5, 0, 5}, path));
    CHECK(path.size() == 2);
    CHECK(path_length(path) == doctest::Approx(10.0f).epsilon(0.03));
}

TEST_CASE("navigation: clearance, unreachable targets, raycast and random points") {
    Scene s;
    arena(s);
    // Closed room on the far side: walls all around a 4x4 area at x in [6, 10], z in [-9, -5].
    box(s, {8, 1, -9.25f}, {4.5f, 2, 0.5f});
    box(s, {8, 1, -4.75f}, {4.5f, 2, 0.5f});
    box(s, {5.75f, 1, -7}, {0.5f, 2, 4.5f});
    box(s, {10.25f, 1, -7}, {0.5f, 2, 4.5f});
    NavigationSystem nav;
    REQUIRE(nav.build(s).ok());

    // The wall's own top (2 m high, 1 m wide) is walkable but disconnected from the floor.
    // Agent clearance: points right next to the wall snap away from it by >= the agent radius.
    const auto near_wall = nav.nearest({-0.55f, 0, 0}, {1, 2, 1});
    REQUIRE(near_wall.has_value());
    CHECK(near_wall->x <= -0.5f - 0.3f);

    std::vector<math::Vec3> path;
    bool partial = false;
    REQUIRE(nav.find_path({-5, 0, 5}, {8, 0, -7}, path, &partial));
    CHECK(partial); // inside the closed room: the path ends at the closest reachable point
    CHECK(horizontal(path.back(), {8, 0, -7}) > 1.5f);

    const RaycastResult blocked = nav.raycast({-5, 0, 5}, {5, 0, 5});
    CHECK(blocked.hit);
    CHECK(blocked.point.x < -0.5f);
    CHECK(blocked.normal.x < -0.5f); // the wall faces -x on this side
    const RaycastResult clear = nav.raycast({-5, 0, 5}, {-5, 0, -5});
    CHECK_FALSE(clear.hit);

    const auto r1 = nav.random_point(7), r2 = nav.random_point(7), r3 = nav.random_point(8);
    REQUIRE(r1.has_value());
    REQUIRE(r3.has_value());
    CHECK(math::length(*r1 - *r2) < 1e-6f); // deterministic per seed
    CHECK(math::length(*r1 - *r3) > 1e-4f);
    CHECK(std::fabs(r1->x) < 10.0f);

    // Nothing walkable.
    Scene empty;
    NavigationSystem none;
    REQUIRE(none.build(empty).ok());
    CHECK_FALSE(none.valid());
    CHECK_FALSE(none.nearest({0, 0, 0}).has_value());
    CHECK_FALSE(none.find_path({0, 0, 0}, {1, 0, 0}, path));
}

TEST_CASE("navigation: triggers and dynamic bodies are ignored; kinematic bodies are upright obstacles") {
    Scene s;
    box(s, {0, -0.5f, 0}, {10, 1, 10});
    const Entity trigger = box(s, {0, 1, 0}, {1, 2, 10});
    s.world().get<ColliderComponent>(trigger)->is_trigger = true;
    const Entity crate = box(s, {2, 0.5f, 0}, {1, 1, 10});
    s.world().add<physics::RigidBodyComponent>(crate, physics::RigidBodyComponent{});
    NavigationSystem nav;
    REQUIRE(nav.build(s).ok());
    CHECK(nav.stats().source_triangles == 12);
    std::vector<math::Vec3> path;
    REQUIRE(nav.find_path({-4, 0, 0}, {4, 0, 0}, path));
    CHECK(path.size() == 2);

    // A spinning kinematic block in the way: carved out as a bounding cylinder (radius ~0.71 for a
    // 1 m cube), so paths bend around it and spinning it about Y does not trigger rebuilds.
    const Entity spinner = box(s, {0, 0.5f, 0}, {1, 1, 1});
    s.world().add<physics::RigidBodyComponent>(spinner).type = physics::BodyType::Kinematic;
    REQUIRE(nav.ensure_built(s).ok());
    CHECK(nav.stats().builds == 2);
    REQUIRE(nav.find_path({-4, 0, 0}, {4, 0, 0}, path));
    CHECK(path.size() >= 3);
    for (const auto& p : path) CHECK(std::hypot(p.x, p.z) > 0.7f);
    for (int i = 1; i <= 8; ++i) {
        s.world().get<Transform>(spinner)->rotation = math::Quat::axis_angle({0, 1, 0}, 0.4f * static_cast<float>(i));
        REQUIRE(nav.ensure_built(s).ok());
    }
    CHECK(nav.stats().builds == 2);
    s.world().get<Transform>(spinner)->position = {0, 0.5f, 3};
    REQUIRE(nav.ensure_built(s).ok());
    CHECK(nav.stats().builds == 3);
}

TEST_CASE("navigation: agents walk to their destination around obstacles") {
    Scene s;
    arena(s);
    const Entity agent = s.create("Agent");
    s.world().get<Transform>(agent)->position = {-5, 0, 5};
    NavAgentComponent a;
    a.speed = 4;
    s.world().add<NavAgentComponent>(agent, a);

    NavigationSystem nav;
    gameplay::Simulation sim;
    add_navigation_systems(sim, nav);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(nav.valid());
    CHECK(navigation_of(s) == &nav);
    CHECK(nav.agent_state(agent).active);
    REQUIRE(nav.set_destination(s, agent, {5, 0, 5}));
    CHECK(nav.agent_state(agent).has_destination);

    float min_z = 100;
    int frames = 0;
    for (; frames < 60 * 20 && !nav.agent_state(agent).reached; ++frames) {
        sim.tick(s, 1.0f / 60.0f);
        const math::Vec3 p = s.world().get<Transform>(agent)->position;
        min_z = std::min(min_z, p.z);
        CHECK_FALSE((std::fabs(p.x) < 0.5f && p.z > -6.0f)); // never inside the wall
    }
    CHECK(nav.agent_state(agent).reached);
    const math::Vec3 end = s.world().get<Transform>(agent)->position;
    CHECK(horizontal(end, {5, 0, 5}) <= 0.25f);
    CHECK(min_z < -6.0f);
    // ~ 2 * hypot(5, 11.4) = 25 m at 4 m/s: a bit over 6 s.
    CHECK(frames > 60 * 5);
    CHECK(frames < 60 * 12);
    // Faces its last movement direction (+x/+z quadrant), stays put once arrived.
    sim.tick(s, 1.0f / 60.0f);
    CHECK(horizontal(s.world().get<Transform>(agent)->position, end) < 1e-3f);
    CHECK(nav.stats().agents == 1);
    CHECK(nav.stats().moving == 0);

    // Teleporting the entity re-places the agent; stop() cancels a destination.
    s.world().get<Transform>(agent)->position = {-5, 0, -8};
    REQUIRE(nav.set_destination(s, agent, {-5, 0, 8}));
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60.0f);
    const math::Vec3 moving = s.world().get<Transform>(agent)->position;
    CHECK(moving.z > -8.0f + 0.5f);
    CHECK(std::fabs(moving.x + 5) < 0.5f);
    nav.stop(s, agent);
    for (int i = 0; i < 60; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(nav.agent_state(agent).has_destination);
    CHECK(s.world().get<Transform>(agent)->position.z < moving.z + 2.5f); // decelerates and halts

    // Removing the component unregisters the agent.
    s.world().remove<NavAgentComponent>(agent);
    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(nav.agent_state(agent).active);
}

TEST_CASE("navigation: agents avoid each other") {
    Scene s;
    box(s, {0, -0.5f, 0}, {20, 1, 20});
    NavigationSystem nav;
    gameplay::Simulation sim;
    add_navigation_systems(sim, nav);
    const Entity a = s.create("A"), b = s.create("B");
    s.world().get<Transform>(a)->position = {-5, 0, 0};
    s.world().get<Transform>(b)->position = {5, 0, 0};
    s.world().add<NavAgentComponent>(a, NavAgentComponent{});
    s.world().add<NavAgentComponent>(b, NavAgentComponent{});
    sim.tick(s, 1.0f / 60.0f);
    REQUIRE(nav.set_destination(s, a, {5, 0, 0}));
    REQUIRE(nav.set_destination(s, b, {-5, 0, 0}));
    float closest = 100;
    for (int i = 0; i < 60 * 8; ++i) {
        sim.tick(s, 1.0f / 60.0f);
        closest = std::min(closest, math::length(s.world().get<Transform>(a)->position - s.world().get<Transform>(b)->position));
    }
    CHECK(closest > 0.5f); // radii 0.4 + 0.4: they sidestep instead of passing through
    CHECK(nav.agent_state(a).reached);
    CHECK(nav.agent_state(b).reached);
}

TEST_CASE("navigation: CharacterController agents move through the physics character mover") {
    auto world = physics::PhysicsWorld::create({});
    REQUIRE(world.ok());
    Scene s;
    arena(s);
    const Entity npc = s.create("Npc");
    s.world().get<Transform>(npc)->position = {-5, 0, 5};
    s.world().add<physics::CharacterControllerComponent>(npc, physics::CharacterControllerComponent{});
    NavAgentComponent a;
    a.speed = 4;
    a.radius = 0.3f;
    s.world().add<NavAgentComponent>(npc, a);

    NavigationSystem nav;
    gameplay::Simulation sim;
    physics::add_physics_systems(sim, *world.value());
    add_navigation_systems(sim, nav);
    sim.tick(s, 1.0f / 60.0f);
    REQUIRE(nav.set_destination(s, npc, {5, 0, 5}));
    int frames = 0;
    for (; frames < 60 * 20 && !nav.agent_state(npc).reached; ++frames) sim.tick(s, 1.0f / 60.0f);
    CHECK(nav.agent_state(npc).reached);
    const math::Vec3 p = s.world().get<Transform>(npc)->position;
    CHECK(horizontal(p, {5, 0, 5}) <= 0.35f);
    CHECK(std::fabs(p.y) < 0.1f);
    CHECK(world.value()->character_state(npc).grounded);
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(math::length(world.value()->character_state(npc).velocity) < 0.2f);
}

TEST_CASE("navigation: rebuilds when static geometry changes; surface settings apply") {
    Scene s;
    box(s, {0, -0.5f, 0}, {20, 1, 20});
    const Entity agent = s.create("Agent");
    s.world().get<Transform>(agent)->position = {-5, 0, 0};
    s.world().add<NavAgentComponent>(agent, NavAgentComponent{});
    NavigationSystem nav;
    gameplay::Simulation sim;
    add_navigation_systems(sim, nav);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(nav.stats().builds == 1);
    std::vector<math::Vec3> path;
    REQUIRE(nav.find_path({-5, 0, 0}, {5, 0, 0}, path));
    CHECK(path.size() == 2);

    // A new wall appears: the next change check rebuilds and paths bend around it.
    box(s, {0, 1, 0}, {1, 2, 12});
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(nav.stats().builds == 2);
    REQUIRE(nav.find_path({-5, 0, 0}, {5, 0, 0}, path));
    CHECK(path.size() >= 3);
    CHECK(nav.agent_state(agent).active); // agents survive rebuilds
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(nav.stats().builds == 2); // no change, no rebuild

    // A bigger agent radius closes a 1.4 m gap.
    Scene g;
    box(g, {0, -0.5f, 0}, {20, 1, 20});
    box(g, {0, 1, -5.35f}, {1, 2, 9.3f});
    box(g, {0, 1, 5.35f}, {1, 2, 9.3f}); // gap at z in [-0.7, 0.7]
    NavigationSystem thin;
    REQUIRE(thin.build(g).ok());
    bool partial = true;
    REQUIRE(thin.find_path({-5, 0, 0}, {5, 0, 0}, path, &partial));
    CHECK_FALSE(partial);
    const Entity surface = g.create("Navigation");
    NavMeshSurfaceComponent wide;
    wide.agent_radius = 0.8f;
    g.world().add<NavMeshSurfaceComponent>(surface, wide);
    NavigationSystem fat;
    REQUIRE(fat.ensure_built(g).ok());
    REQUIRE(fat.find_path({-5, 0, 0}, {5, 0, 0}, path, &partial));
    CHECK(partial);

    std::vector<DebugLine> lines;
    fat.debug_lines(lines);
    CHECK(lines.size() > 8);
}

TEST_CASE("navigation: components are reflected and serialize") {
    register_components();
    const scene::ComponentSerializer* agent = scene::SerializerRegistry::instance().by_name("NavAgent");
    const scene::ComponentSerializer* surf = scene::SerializerRegistry::instance().by_name("NavMeshSurface");
    REQUIRE(agent);
    REQUIRE(surf);
    Scene s;
    const Entity e = s.create("Agent");
    NavAgentComponent a;
    a.speed = 6.5f;
    a.avoidance = false;
    s.world().add<NavAgentComponent>(e, a);
    const json::Value doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* back = t.world().get<NavAgentComponent>(t.find_by_name("Agent"));
    REQUIRE(back);
    CHECK(*back == a);
}

TEST_CASE("navigation: static height fields (terrain) are walkable and steep ridges block paths") {
    // 33 x 33 samples, 0.75 m apart (24 m), ground at y = 1 with a 6 m ridge across the middle
    // column band for z rows 0..23 (open near +z).
    Scene s;
    const Entity t = s.create("Terrain");
    s.world().get<Transform>(t)->position = {50, 0, 0};
    s.world().get<Transform>(t)->scale = {4, 4, 4}; // ignored, like the physics body
    physics::StaticHeightfieldComponent hf;
    hf.resolution = 33;
    auto heights = std::make_shared<std::vector<float>>(33 * 33, 1.0f);
    for (int z = 0; z <= 23; ++z)
        for (int x = 15; x <= 17; ++x) (*heights)[static_cast<std::size_t>(z) * 33 + static_cast<std::size_t>(x)] = 7.0f;
    hf.heights = heights;
    hf.offset = {-12, 0, -12};
    hf.spacing = {0.75f, 0.75f};
    hf.revision = 1;
    s.world().add<physics::StaticHeightfieldComponent>(t, hf);
    s.update_transforms();

    NavigationSystem nav;
    REQUIRE(nav.build(s).ok());
    REQUIRE(nav.valid());
    CHECK(nav.stats().source_triangles == 32u * 32u * 2u);
    const auto ground = nav.nearest({45, 3, -5}, {2, 4, 2});
    REQUIRE(ground);
    CHECK(ground->y == doctest::Approx(1.0f).epsilon(0.05));
    std::vector<math::Vec3> path;
    bool partial = true;
    REQUIRE(nav.find_path({42, 1, -5}, {58, 1, -5}, path, &partial));
    CHECK_FALSE(partial);
    bool around = false;
    for (const auto& p : path) around |= p.z > 5.0f; // through the gap near +z
    CHECK(around);
    CHECK(path_length(path) > 25.0f);

    // Removing the ridge (new heights, new revision) is picked up by ensure_built.
    auto flat = std::make_shared<std::vector<float>>(33 * 33, 1.0f);
    auto* c = s.world().get<physics::StaticHeightfieldComponent>(t);
    c->heights = flat;
    ++c->revision;
    const std::uint32_t builds = nav.stats().builds;
    REQUIRE(nav.ensure_built(s).ok());
    CHECK(nav.stats().builds == builds + 1);
    REQUIRE(nav.find_path({42, 1, -5}, {58, 1, -5}, path));
    CHECK(path_length(path) == doctest::Approx(16.0f).epsilon(0.05));

    // Layer masks exclude fields like colliders.
    Scene masked;
    const Entity m = masked.create("Terrain");
    hf.layer = 3;
    masked.world().add<physics::StaticHeightfieldComponent>(m, hf);
    NavMeshSurfaceComponent surface;
    surface.layer_mask = 1u;
    masked.world().add<NavMeshSurfaceComponent>(m, surface);
    masked.update_transforms();
    NavigationSystem nav3;
    REQUIRE(nav3.build(masked).ok());
    CHECK_FALSE(nav3.valid());
}

TEST_CASE("navigation: static triangle-mesh colliders are walkable with their concave shape") {
    // Two 10 x 10 slabs at y = 0 separated by a 2 m gap along x, as one triangle-mesh collider.
    scene::ProceduralMesh pm;
    for (const float x0 : {-11.0f, 1.0f}) {
        const auto b = static_cast<std::uint32_t>(pm.positions.size());
        const float x1 = x0 + 10;
        pm.positions.insert(pm.positions.end(), {{x0, 0, -5}, {x1, 0, -5}, {x1, 0, 5}, {x0, 0, 5}});
        pm.indices.insert(pm.indices.end(), {b, b + 2, b + 1, b, b + 3, b + 2});
    }
    auto mesh = scene::publish_mesh(std::move(pm));
    REQUIRE(mesh.ok());
    Scene s;
    const Entity floor = s.create("Floor");
    s.world().add<scene::ProceduralMeshComponent>(floor, scene::ProceduralMeshComponent{*mesh});
    ColliderComponent c;
    c.shape = ColliderComponent::Shape::Mesh;
    s.world().add<ColliderComponent>(floor, c);

    // Without a physics world bound to the scene, mesh geometry is not resolved and is skipped.
    {
        NavigationSystem nav;
        (void)nav.build(s);
        CHECK(nav.stats().source_triangles == 0);
    }
    auto world = physics::PhysicsWorld::create({});
    REQUIRE(world.ok());
    physics::bind(s, world.value().get());
    NavigationSystem nav;
    REQUIRE(nav.build(s).ok());
    CHECK(nav.stats().source_triangles == 4);
    const auto left = nav.nearest({-6, 1, 0}, {1, 2, 1});
    REQUIRE(left.has_value());
    CHECK(std::fabs(left->y) < 0.25f);
    CHECK_FALSE(nav.nearest({0, 1, 0}, {0.3f, 2, 0.3f}).has_value()); // the gap is not walkable
    std::vector<math::Vec3> path;
    bool partial = false;
    (void)nav.find_path({-6, 0, 0}, {6, 0, 0}, path, &partial);
    CHECK(partial); // the slabs are not connected
    physics::bind(s, nullptr);
}
