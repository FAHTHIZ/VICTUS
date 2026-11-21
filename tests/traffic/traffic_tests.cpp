// SPDX-License-Identifier: Apache-2.0
// Traffic (ADR-0167, GAP-22): road networks, lane graphs, signals and IDM agents.
#include "victus/traffic/traffic.hpp"

#include "victus/scene/reflect.hpp"
#include "victus/scene/scene.hpp"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

using namespace victus;
using namespace victus::traffic;
using math::Vec3;

namespace {

constexpr float kDt = 1.0f / 30.0f;

std::size_t count_connectors(const LaneGraph& g) {
    return static_cast<std::size_t>(std::count_if(g.lanes.begin(), g.lanes.end(), [](const Lane& l) { return l.connector; }));
}

// Smallest bumper gap between consecutive agents sharing a lane.
float min_same_lane_gap(scene::Scene& s) {
    std::map<std::uint32_t, std::vector<std::pair<float, float>>> by_lane;
    s.world().each<TrafficAgentComponent>([&](const TrafficAgentComponent& c) { by_lane[c.lane].push_back({c.distance, c.length}); });
    float best = 1e9f;
    for (auto& [lane, v] : by_lane) {
        std::sort(v.begin(), v.end());
        for (std::size_t i = 1; i < v.size(); ++i) best = std::min(best, v[i].first - v[i].second - v[i - 1].first);
    }
    return best;
}

} // namespace

TEST_CASE("traffic: grid network, validation and JSON round trip") {
    const RoadNetwork net = grid_network(3, 2, 50.0f);
    CHECK(net.nodes.size() == 6);
    CHECK(net.roads.size() == 7); // 2 rows x 2 + 3 columns x 1
    CHECK(validate(net).empty());
    CHECK(net.nodes[0].position.x == doctest::Approx(-50.0f));
    CHECK(net.nodes[5].position.z == doctest::Approx(25.0f));

    const std::string text = json::stringify(road_network_to_json(net));
    auto back = parse_road_network(std::string_view(text));
    REQUIRE(back.ok());
    CHECK(back->nodes.size() == net.nodes.size());
    REQUIRE(back->roads.size() == net.roads.size());
    CHECK(back->roads[3].a == net.roads[3].a);
    CHECK(back->roads[3].b == net.roads[3].b);
    CHECK(back->roads[3].speed_limit == doctest::Approx(net.roads[3].speed_limit));

    RoadNetwork bad = net;
    bad.roads.push_back({0, 99});
    bad.roads.push_back({1, 1});
    bad.roads.push_back({0, 1, 0, 0});
    CHECK(validate(bad).size() == 3);
    CHECK_FALSE(parse_road_network(std::string_view(R"({"format":"victus.roads","version":2})")).ok());
    CHECK_FALSE(parse_road_network(std::string_view(R"({"format":"other","version":1})")).ok());
    CHECK_FALSE(parse_road_network(std::string_view(
                    R"({"format":"victus.roads","version":1,"nodes":[{"position":[0,0,0]}],"roads":[{"a":0,"b":3}]})"))
                    .ok());
}

TEST_CASE("traffic: lane graph topology, lane sides and signals") {
    const RoadNetwork net = grid_network(3, 3, 60.0f);
    const LaneGraph g = build_lane_graph(net);
    // 12 two-way roads -> 24 driving lanes. Connectors: corners 2x1x4, edges 3x2x4, centre 4x3.
    CHECK(g.lanes.size() - count_connectors(g) == 24);
    CHECK(count_connectors(g) == 8 + 24 + 12);
    CHECK(g.signals.size() == 5);

    for (const Lane& l : g.lanes) {
        CHECK(l.length > 0.5f);
        CHECK_FALSE(l.next.empty()); // every lane continues somewhere on a grid
        if (l.connector) CHECK(l.speed_limit <= 13.9f + 1e-3f);
    }
    // A lane driving +X sits on its right, i.e. +Z of the road axis (right = forward x up).
    bool found = false;
    for (const Lane& l : g.lanes) {
        if (l.connector) continue;
        const Vec3 d = l.points.back() - l.points.front();
        if (d.x > 10 && std::abs(d.z) < 1e-3f) {
            const Road& r = net.roads[l.road];
            CHECK(l.points.front().z == doctest::Approx(net.nodes[r.a].position.z + 1.75f));
            found = true;
        }
    }
    CHECK(found);

    LaneGraphSettings lh;
    lh.left_hand = true;
    const LaneGraph gl = build_lane_graph(net, lh);
    for (const Lane& l : gl.lanes) {
        if (l.connector) continue;
        const Vec3 d = l.points.back() - l.points.front();
        if (d.x > 10 && std::abs(d.z) < 1e-3f) CHECK(l.points.front().z == doctest::Approx(net.nodes[net.roads[l.road].a].position.z - 1.75f));
    }

    // Connectors are continuous with their lanes.
    for (std::uint32_t i = 0; i < g.lanes.size(); ++i) {
        const Lane& l = g.lanes[i];
        for (std::uint32_t n : l.next) CHECK(math::length(g.lanes[n].points.front() - l.points.back()) < 1e-3f);
    }

    // Split phase: at any time at most one approach of a signal is not red, and each gets green.
    const Signal& sig = g.signals[0];
    std::vector<int> greens(sig.phases, 0);
    for (double t = 0; t < sig.cycle() * 2; t += 0.25) {
        std::vector<bool> open(sig.phases, false);
        for (std::uint32_t i = 0; i < g.lanes.size(); ++i) {
            const Lane& l = g.lanes[i];
            if (!l.connector || l.signal != 0) continue;
            const Light light = g.light(i, t);
            if (light != Light::Red) open[l.phase] = true;
            if (light == Light::Green) ++greens[l.phase];
        }
        CHECK(std::count(open.begin(), open.end(), true) <= 1);
    }
    for (int gcount : greens) CHECK(gcount > 0);

    const auto lines = lane_graph_lines(g);
    CHECK(lines.size() % 2 == 0);
    CHECK(lines.size() > g.lanes.size() * 2 - 1);
}

TEST_CASE("traffic: a lone car accelerates to the limit and U-turns at a dead end") {
    RoadNetwork net;
    net.add_node({0, 0, 0});
    net.add_node({0, 0, 400});
    net.add_road({0, 1, 1, 1, 3.5f, 13.9f});
    scene::Scene s;
    set_traffic_graph(s, std::make_shared<LaneGraph>(build_lane_graph(net)));
    const auto cars = spawn_traffic(s, 1, 3);
    REQUIRE(cars.size() == 1);
    auto* c = s.world().get<TrafficAgentComponent>(cars[0]);
    c->max_speed = 20.0f; // the lane limit governs
    float peak = 0;
    bool turned = false;
    for (int f = 0; f < 30 * 90; ++f) {
        update_traffic(s, kDt);
        peak = std::max(peak, c->speed);
        if (traffic_graph(s)->lanes[c->lane].connector) turned = true;
    }
    MESSAGE("lone car peak speed " << peak << " m/s");
    CHECK(peak == doctest::Approx(13.9f).epsilon(0.03));
    CHECK(peak <= 13.9f + 1e-3f);
    CHECK(turned); // it drove onto a U-turn connector at the end
    CHECK(c->turns >= 2);
    const Vec3 p = s.world().get<scene::Transform>(cars[0])->position;
    CHECK(std::isfinite(p.x));
    CHECK(std::abs(p.x) < 5.0f);
}

TEST_CASE("traffic: queues at lights without collisions and stays deterministic") {
    const RoadNetwork net = grid_network(4, 4, 70.0f);
    auto graph = std::make_shared<LaneGraph>(build_lane_graph(net));
    auto run = [&](std::vector<Vec3>& final_positions, TrafficStats& peak_wait, float& min_gap, double& seconds) {
        scene::Scene s;
        set_traffic_graph(s, graph);
        const auto cars = spawn_traffic(s, 120, 42);
        REQUIRE(cars.size() == 120);
        min_gap = 1e9f;
        std::vector<float> travelled(cars.size(), 0);
        std::vector<Vec3> last(cars.size());
        for (std::size_t i = 0; i < cars.size(); ++i) last[i] = s.world().get<scene::Transform>(cars[i])->position;
        for (int f = 0; f < 30 * 180; ++f) {
            update_traffic(s, kDt);
            const TrafficStats* st = traffic_stats(s);
            if (st->waiting_at_light > peak_wait.waiting_at_light) peak_wait = *st;
            seconds += st->update_ms;
            min_gap = std::min(min_gap, min_same_lane_gap(s));
            for (std::size_t i = 0; i < cars.size(); ++i) {
                const Vec3 p = s.world().get<scene::Transform>(cars[i])->position;
                travelled[i] += math::length(p - last[i]);
                last[i] = p;
            }
        }
        final_positions = last;
        const float least = *std::min_element(travelled.begin(), travelled.end());
        MESSAGE("least distance driven in 180 s: " << least << " m; mean speed " << traffic_stats(s)->mean_speed);
        CHECK(least > 100.0f); // nobody is gridlocked for the whole run
    };
    std::vector<Vec3> a, b;
    TrafficStats wa, wb;
    float gap_a = 0, gap_b = 0;
    double ms_a = 0, ms_b = 0;
    run(a, wa, gap_a, ms_a);
    run(b, wb, gap_b, ms_b);
    MESSAGE("peak waiting at lights " << wa.waiting_at_light << ", min same-lane gap " << gap_a << " m, update "
                                      << ms_a / (30 * 180) << " ms/frame");
    CHECK(wa.waiting_at_light > 0);
    CHECK(gap_a > 0.0f);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].z == b[i].z);
    }
}

TEST_CASE("traffic: agent component is reflected and resumes after a graph swap") {
    register_components();
    CHECK(scene::find_schema("TrafficAgent") != nullptr);
    scene::Scene s;
    set_traffic_graph(s, std::make_shared<LaneGraph>(build_lane_graph(grid_network(2, 2, 50.0f))));
    const auto cars = spawn_traffic(s, 4, 9);
    for (int f = 0; f < 60; ++f) update_traffic(s, kDt);
    set_traffic_graph(s, std::make_shared<LaneGraph>(build_lane_graph(grid_network(2, 2, 50.0f))));
    for (auto e : cars) CHECK(s.world().get<TrafficAgentComponent>(e)->lane == kNone);
    update_traffic(s, kDt);
    for (auto e : cars) CHECK(s.world().get<TrafficAgentComponent>(e)->lane != kNone);
}

TEST_CASE("traffic: 2,000 agents on a 10x10 grid (measurement)") {
    scene::Scene s;
    set_traffic_graph(s, std::make_shared<LaneGraph>(build_lane_graph(grid_network(10, 10, 80.0f, 2))));
    const auto cars = spawn_traffic(s, 2000, 5);
    REQUIRE(cars.size() == 2000);
    for (int f = 0; f < 30; ++f) update_traffic(s, kDt);
    double total = 0;
    const int frames = 120;
    for (int f = 0; f < frames; ++f) {
        update_traffic(s, kDt);
        total += traffic_stats(s)->update_ms;
    }
    MESSAGE("2,000 agents: " << total / frames << " ms per update, mean speed " << traffic_stats(s)->mean_speed << " m/s");
    CHECK(traffic_stats(s)->agents == 2000);
    CHECK(min_same_lane_gap(s) > 0.0f);
}

TEST_CASE("traffic: authored TrafficNetwork builds a grid or loads a .vroads file") {
    register_components();
    CHECK(scene::find_schema("TrafficNetwork") != nullptr);
    {
        scene::Scene s;
        const scene::Entity owner = s.create("Traffic");
        TrafficNetworkComponent n;
        n.columns = 3;
        n.rows = 3;
        n.block = 60;
        n.vehicles = 12;
        s.world().add<TrafficNetworkComponent>(owner, n);
        const scene::Entity templ = s.create("Car", owner);
        s.world().add<scene::MeshRendererComponent>(templ).primitive = "cube";
        CHECK(build_authored_network(s));
        REQUIRE(traffic_graph(s) != nullptr);
        CHECK(traffic_graph(s)->signals.size() == 5);
        CHECK(s.world().count<TrafficAgentComponent>() == 12);
        CHECK_FALSE(s.world().get<scene::MeshRendererComponent>(templ)->visible);
        std::size_t visible = 0;
        s.world().each<scene::MeshRendererComponent>([&](const scene::MeshRendererComponent& m) { visible += m.visible ? 1 : 0; });
        CHECK(visible == 12);
        CHECK(s.world().get<TrafficNetworkComponent>(owner)->built);
        const auto* before = traffic_graph(s);
        CHECK(build_authored_network(s)); // already built: nothing changes
        CHECK(traffic_graph(s) == before);
        CHECK(s.world().count<TrafficAgentComponent>() == 12);

        gameplay::Simulation sim;
        add_traffic_systems(sim);
        CHECK(sim.find("traffic.update") != nullptr);
    }
    {
        RoadNetwork net;
        net.add_node({0, 0, 0});
        net.add_node({100, 0, 0});
        net.add_node({100, 0, 100});
        net.add_road({0, 1});
        net.add_road({1, 2});
        const std::string text = json::stringify(road_network_to_json(net));
        scene::Scene s;
        const scene::Entity owner = s.create("Traffic");
        TrafficNetworkComponent n;
        n.network = "roads/town.vroads";
        n.vehicles = 3;
        s.world().add<TrafficNetworkComponent>(owner, n);
        std::string asked;
        CHECK(build_authored_network(s, [&](std::string_view p) -> Result<std::string> {
            asked = std::string(p);
            return text;
        }));
        CHECK(asked == "roads/town.vroads");
        REQUIRE(traffic_graph(s) != nullptr);
        CHECK(traffic_graph(s)->signals.empty());
        CHECK(s.world().count<TrafficAgentComponent>() == 3);

        scene::Scene bad;
        TrafficNetworkComponent nb;
        nb.network = "missing.vroads";
        bad.world().add<TrafficNetworkComponent>(bad.create("T"), nb);
        CHECK_FALSE(build_authored_network(bad, [](std::string_view) -> Result<std::string> {
            return make_error(ErrorCode::NotFound, "missing");
        }));
        CHECK(traffic_graph(bad) == nullptr);
    }
}
