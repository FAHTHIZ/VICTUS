// SPDX-License-Identifier: Apache-2.0
// Procedural city (ADR-0202, GAP-20): layout, meshes, scene build, traffic and pedestrians.
#include "victus/procgen/city.hpp"

#include "victus/animation/crowd.hpp"
#include "victus/core/json.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/scene.hpp"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace victus;
using namespace victus::procgen;
using math::Vec3;

namespace {

bool inside(const Rect& in, const Rect& out, float eps = 1e-3f) {
    return in.min.x >= out.min.x - eps && in.min.y >= out.min.y - eps && in.max.x <= out.max.x + eps && in.max.y <= out.max.y + eps;
}

bool overlaps(const Rect& a, const Rect& b, float eps = 1e-3f) {
    return a.min.x < b.max.x - eps && b.min.x < a.max.x - eps && a.min.y < b.max.y - eps && b.min.y < a.max.y - eps;
}

City make(std::uint32_t seed, std::uint32_t cols = 5, std::uint32_t rows = 4) {
    CitySettings s;
    s.seed = seed;
    s.columns = cols;
    s.rows = rows;
    auto c = generate_city(s);
    REQUIRE(c);
    return std::move(c).value();
}


} // namespace

TEST_CASE("procgen city: deterministic layout with nested, contained buildings") {
    const City a = make(7);
    const City b = make(7);
    const City c = make(8);
    REQUIRE(a.buildings.size() == b.buildings.size());
    REQUIRE(a.props.size() == b.props.size());
    for (std::size_t i = 0; i < a.buildings.size(); ++i) {
        CHECK(a.buildings[i].lot.min.x == b.buildings[i].lot.min.x);
        CHECK(a.buildings[i].height() == b.buildings[i].height());
    }
    CHECK(a.xs == b.xs);
    CHECK(a.xs != c.xs); // a different seed is a different city

    CHECK(a.blocks.size() == 20);
    CHECK(a.xs.size() == 6);
    CHECK(a.zs.size() == 5);
    CHECK(a.roads.nodes.size() == 30);
    CHECK(a.roads.roads.size() == 2 * 5 * 4 + 5 + 4);
    CHECK(a.walks.size() == a.blocks.size());
    CHECK(a.buildings.size() > 60);
    CHECK(a.props.size() > 100);

    std::size_t parks = 0, tall = 0, tiered = 0;
    for (const Block& bl : a.blocks) {
        parks += bl.park;
        CHECK(inside(bl.inner, bl.kerb));
    }
    for (std::size_t i = 0; i < a.buildings.size(); ++i) {
        const Building& bd = a.buildings[i];
        REQUIRE(bd.block < a.blocks.size());
        const Block& bl = a.blocks[bd.block];
        CHECK_FALSE(bl.park);
        CHECK(inside(bd.lot, bl.inner));
        REQUIRE_FALSE(bd.tiers.empty());
        CHECK(inside(bd.tiers[0].footprint, bd.lot));
        for (std::size_t t = 1; t < bd.tiers.size(); ++t) {
            CHECK(inside(bd.tiers[t].footprint, bd.tiers[t - 1].footprint));
            CHECK(bd.tiers[t].top > bd.tiers[t - 1].top);
        }
        CHECK(bd.floors >= a.settings.floors_min);
        CHECK(bd.floors <= a.settings.floors_max);
        CHECK(bd.style < kStyleCount);
        tall += bd.floors >= 12;
        tiered += bd.tiers.size() > 1;
        for (std::size_t j = i + 1; j < a.buildings.size(); ++j)
            if (a.buildings[j].block == bd.block) CHECK_FALSE(overlaps(bd.lot, a.buildings[j].lot));
    }
    CHECK(tall > 0);   // downtown rises
    CHECK(tiered > 0); // and steps back
    MESSAGE("seed 7: " << a.buildings.size() << " buildings, " << parks << " parks, " << tall << " tall, " << tiered << " tiered, "
                       << a.props.size() << " props");

    // Walk loops close and wrap.
    const WalkLoop& wl = a.walks[0];
    REQUIRE(wl.length > 0);
    Vec3 t0;
    const Vec3 p0 = wl.point_at(0.0f, &t0);
    const Vec3 p1 = wl.point_at(wl.length);
    const Vec3 p2 = wl.point_at(wl.length * 2.0f + 1.0f);
    const Vec3 p3 = wl.point_at(1.0f);
    CHECK(std::abs(p0.x - p1.x) < 1e-2f);
    CHECK(std::abs(p0.z - p1.z) < 1e-2f);
    CHECK(std::abs(p2.x - p3.x) < 1e-2f);
    CHECK(std::abs(p2.z - p3.z) < 1e-2f);
    CHECK(std::abs(math::dot(t0, t0) - 1.0f) < 1e-3f);
    // On the sidewalk: between the block's kerb and inner rects.
    const Block& bl0 = a.blocks[0];
    for (float s = 0; s < wl.length; s += 5.0f) {
        const Vec3 p = wl.point_at(s);
        CHECK(p.x >= bl0.kerb.min.x - 1e-2f);
        CHECK(p.x <= bl0.kerb.max.x + 1e-2f);
        CHECK(p.z >= bl0.kerb.min.y - 1e-2f);
        CHECK(p.z <= bl0.kerb.max.y + 1e-2f);
        const bool in_inner = p.x > bl0.inner.min.x + 1e-2f && p.x < bl0.inner.max.x - 1e-2f && p.z > bl0.inner.min.y + 1e-2f &&
                              p.z < bl0.inner.max.y - 1e-2f;
        CHECK_FALSE(in_inner);
    }

    // The road network makes a valid lane graph.
    const auto g = traffic::build_lane_graph(a.roads);
    CHECK(g.lanes.size() > a.roads.roads.size() * 2);
    CHECK_FALSE(g.signals.empty());
}

TEST_CASE("procgen city: settings are validated") {
    CitySettings s;
    s.columns = 0;
    CHECK_FALSE(generate_city(s));
    s = {};
    s.block_min = 120;
    s.block_max = 80;
    CHECK_FALSE(generate_city(s));
    s = {};
    s.block_min = s.block_max = 18; // too small for two avenues and sidewalks
    CHECK_FALSE(generate_city(s));
    s = {};
    s.floors_min = 10;
    s.floors_max = 3;
    CHECK_FALSE(generate_city(s));
    s = {};
    s.columns = s.rows = 1;
    CHECK(generate_city(s));
}

TEST_CASE("procgen city: meshes are outward-facing and split into cells") {
    const City city = make(3, 6, 6);
    const auto one = build_city_meshes(city, 0.0f);
    CHECK(one.size() == kCityLayerCount);
    const auto cells = build_city_meshes(city, 150.0f);
    CHECK(cells.size() > one.size());
    std::size_t tris_one = 0, tris_cells = 0;
    for (const auto& m : one) tris_one += m.mesh.indices.size() / 3;
    for (const auto& m : cells) tris_cells += m.mesh.indices.size() / 3;
    CHECK(tris_one == tris_cells); // splitting moves geometry, it does not add or drop any
    MESSAGE("6x6 city: " << tris_one << " triangles, " << cells.size() << " cell meshes");

    std::size_t bad = 0, degenerate = 0, total = 0, outside = 0;
    for (const auto& m : one) {
        const auto& me = m.mesh;
        REQUIRE(me.normals.size() == me.positions.size());
        REQUIRE(me.colors.size() == me.positions.size());
        REQUIRE(me.indices.size() % 3 == 0);
        for (std::size_t i = 0; i < me.indices.size(); i += 3) {
            const Vec3 a = me.positions[me.indices[i]], b = me.positions[me.indices[i + 1]], c = me.positions[me.indices[i + 2]];
            const Vec3 n = math::cross(b - a, c - a);
            const float len = std::sqrt(math::dot(n, n));
            ++total;
            if (len < 1e-6f) {
                ++degenerate;
                continue;
            }
            // Counter-clockwise front faces agree with the shading normal.
            if (math::dot(n, me.normals[me.indices[i]]) <= 0.0f) ++bad;
        }
        REQUIRE(me.bounds.valid());
        for (const Vec3& p : me.positions)
            if (p.x < me.bounds.min.x - 1e-3f || p.y > me.bounds.max.y + 1e-3f || p.y < -1e-3f) ++outside;
    }
    CHECK(bad == 0);
    CHECK(degenerate == 0);
    CHECK(outside == 0);
    CHECK(total > 10000);
}

TEST_CASE("procgen city: builds into a scene with transient content, traffic and pedestrians") {
    register_components();
    scene::Scene s;
    const scene::Entity owner = s.create("City");
    const scene::Entity car = s.create("Car", owner); // vehicle template
    s.world().add<scene::MeshRendererComponent>(car);
    CityComponent cc;
    cc.columns = 4;
    cc.rows = 3;
    cc.vehicles = 20;
    cc.pedestrians = 50;
    cc.pedestrian_model = Uuid{1, 2};
    s.world().add<CityComponent>(owner, cc);
    const std::string before = json::stringify(s.save());

    gameplay::Simulation sim;
    add_procgen_systems(sim);
    traffic::add_traffic_systems(sim);
    REQUIRE(sim.find("procgen.update") != nullptr);
    sim.tick(s, 1.0f / 30.0f);

    const CityStats* st = city_stats(s);
    REQUIRE(st != nullptr);
    REQUIRE(scene_city(s) != nullptr);
    CHECK(st->blocks == 12);
    CHECK(st->meshes >= 3);
    CHECK(st->triangles > 1000);
    CHECK(st->colliders == 1 + 12 + [&] {
              std::uint32_t n = 0;
              for (const auto& b : scene_city(s)->buildings) n += static_cast<std::uint32_t>(b.tiers.size());
              return n;
          }());
    CHECK(st->vehicles == 20);
    CHECK(st->pedestrians == 50);
    CHECK(s.world().count<animation::CrowdAgentComponent>() == 50);
    CHECK(s.world().count<PedestrianComponent>() == 50);
    CHECK(s.world().count<traffic::TrafficAgentComponent>() == 20);
    CHECK(s.world().count<physics::ColliderComponent>() == st->colliders);
    CHECK_FALSE(s.world().get<scene::MeshRendererComponent>(car)->visible); // template hidden
    CHECK(s.world().get<CityComponent>(owner)->built);

    // Generated content is not saved: the document only gains the built flag.
    const std::string after = json::stringify(s.save());
    CHECK(after.size() < before.size() + 64);
    CHECK(after.find("Generated City") == std::string::npos);
    CHECK(after.find("Pedestrian\"") == std::string::npos);

    // A second tick builds nothing more; pedestrians walk along their sidewalks.
    scene::Entity ped{};
    s.world().each<PedestrianComponent>([&](scene::Entity e, PedestrianComponent&) {
        if (ped == scene::Entity{}) ped = e;
    });
    const Vec3 p0 = s.world().get<scene::Transform>(ped)->position;
    const float speed = std::abs(s.world().get<PedestrianComponent>(ped)->speed);
    for (int i = 0; i < 30; ++i) sim.tick(s, 1.0f / 30.0f);
    CHECK(s.world().count<PedestrianComponent>() == 50);
    const Vec3 p1 = s.world().get<scene::Transform>(ped)->position;
    const Vec3 d = p1 - p0;
    const float moved = std::sqrt(math::dot(d, d));
    CHECK(moved > speed * 0.5f);
    CHECK(moved < speed * 1.05f);

    // Loading the saved scene and ticking rebuilds the same city.
    scene::Scene s2;
    REQUIRE(s2.load(json::parse(before).value()));
    sim.tick(s2, 1.0f / 30.0f);
    REQUIRE(city_stats(s2) != nullptr);
    CHECK(city_stats(s2)->buildings == st->buildings);
    CHECK(city_stats(s2)->triangles == st->triangles);
}

TEST_CASE("procgen city: generation time") {
    CitySettings s;
    s.columns = s.rows = 12;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto city = generate_city(s);
    REQUIRE(city);
    const auto t1 = clock::now();
    const auto meshes = build_city_meshes(*city, 200.0f);
    const auto t2 = clock::now();
    std::size_t tris = 0;
    for (const auto& m : meshes) tris += m.mesh.indices.size() / 3;
    const double gen = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double mesh = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("[perf] city 12x12: %zu buildings, %zu props, %zu meshes, %zu triangles; generate %.2f ms, meshes %.2f ms\n",
                city->buildings.size(), city->props.size(), meshes.size(), tris, gen, mesh);
    CHECK(gen < 500.0);
    CHECK(mesh < 5000.0);
}
