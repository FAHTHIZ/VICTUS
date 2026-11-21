// SPDX-License-Identifier: Apache-2.0
// Simulation schedule (ADR-0165, GAP-09): system ordering constraints, cycle reporting, per-system
// intervals with exact accumulated time, and simulation LOD tiers with hysteresis, staggering and
// conserved per-entity time.
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/scene/reflect.hpp"
#include "victus/scene/scene.hpp"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace victus;
using namespace victus::gameplay;
using scene::Scene;

namespace {

constexpr float kDt = 1.0f / 60.0f;

scene::Entity at(Scene& s, math::Vec3 p, bool lod = true) {
    const scene::Entity e = s.create("Agent");
    s.world().get<scene::Transform>(e)->position = p;
    if (lod) s.world().add<SimulationLodComponent>(e);
    return e;
}

scene::Entity source(Scene& s, math::Vec3 p) {
    const scene::Entity e = s.create("Source");
    s.world().get<scene::Transform>(e)->position = p;
    s.world().add<SimulationLodSource>(e);
    return e;
}

} // namespace

TEST_CASE("schedule: registration order by default, after/before reorder within a phase") {
    Simulation sim;
    std::vector<std::string> ran;
    const auto rec = [&](std::string n) { return [&ran, n](Scene&, const FrameTime&) { ran.push_back(n); }; };
    sim.add_system("a", Phase::Update, rec("a"));
    sim.add_system("b", Phase::Update, rec("b"));
    sim.add_system("c", Phase::Update, rec("c"), {.after = {}, .before = {"a"}});
    sim.add_system("d", Phase::Update, rec("d"), {.after = {"b"}, .before = {}});
    sim.add_system("p", Phase::PreUpdate, rec("p"), {.after = {"a"}, .before = {}}); // other phase: ignored
    Scene s;
    sim.tick(s, kDt);
    CHECK(ran == std::vector<std::string>{"p", "c", "a", "b", "d"});
    CHECK(sim.schedule(Phase::Update) == std::vector<std::string>{"c", "a", "b", "d"});
    REQUIRE(sim.schedule_diagnostics().size() == 1);
    CHECK(sim.schedule_diagnostics()[0].find("'p': after 'a'") != std::string::npos);

    // Existing two-argument registrations keep their order; adding a system rebuilds the order.
    sim.add_system("e", Phase::Update, rec("e"), {.after = {}, .before = {"b"}});
    CHECK(sim.schedule(Phase::Update) == std::vector<std::string>{"c", "a", "e", "b", "d"});
}

TEST_CASE("schedule: a cycle keeps registration order and is reported") {
    Simulation sim;
    std::vector<std::string> ran;
    const auto rec = [&](std::string n) { return [&ran, n](Scene&, const FrameTime&) { ran.push_back(n); }; };
    sim.add_system("x", Phase::Update, rec("x"), {.after = {"y"}, .before = {}});
    sim.add_system("y", Phase::Update, rec("y"), {.after = {"x"}, .before = {}});
    sim.add_system("z", Phase::Update, rec("z"));
    Scene s;
    sim.tick(s, kDt);
    CHECK(ran == std::vector<std::string>{"z", "x", "y"});
    REQUIRE(sim.schedule_diagnostics().size() == 1);
    CHECK(sim.schedule_diagnostics()[0].find("cycle among: x, y") != std::string::npos);
}

TEST_CASE("schedule: interval systems run every Nth pass with the summed delta") {
    Simulation sim;
    int runs_a = 0, runs_b = 0;
    double time_a = 0, time_b = 0;
    sim.add_system("every3", Phase::Update, [&](Scene&, const FrameTime& t) { ++runs_a; time_a += t.dt; },
                   {.after = {}, .before = {}, .interval = 3, .offset = 0});
    sim.add_system("every3b", Phase::Update, [&](Scene&, const FrameTime& t) { ++runs_b; time_b += t.dt; },
                   {.after = {}, .before = {}, .interval = 3, .offset = 1});
    int fixed = 0;
    sim.add_system("fixed2", Phase::FixedUpdate, [&](Scene&, const FrameTime&) { ++fixed; },
                   {.after = {}, .before = {}, .interval = 2, .offset = 0});
    Scene s;
    for (int i = 0; i < 30; ++i) sim.tick(s, kDt);
    CHECK(runs_a == 10);
    CHECK(runs_b == 10);
    // Offsets stagger: "every3" runs on passes 0,3,..,27 and its last run covers 1 pass; "every3b"
    // on passes 2,5,..,29 so all 30 frames are accounted for.
    CHECK(time_a == doctest::Approx(28 * kDt).epsilon(1e-4));
    CHECK(time_b == doctest::Approx(30 * kDt).epsilon(1e-4));
    CHECK(fixed == static_cast<int>((sim.fixed_steps() + 1) / 2));
    CHECK(sim.find("every3")->runs == 10);
    CHECK(sim.find("every3")->passes == 30);

    // Disabled systems do not count passes; reset clears scheduler state.
    sim.find("every3")->enabled = false;
    sim.tick(s, kDt);
    CHECK(sim.find("every3")->passes == 30);
    sim.reset();
    CHECK(sim.find("every3b")->runs == 0);
    CHECK(sim.find("every3b")->pending_dt == 0);
}

TEST_CASE("simulation lod: tiers by distance with hysteresis") {
    SimulationLodSettings s; // 30 / 80 / 200, hysteresis 0.1
    CHECK(simulation_lod_tier(s, 10, 0, false) == 0);
    CHECK(simulation_lod_tier(s, 50, 0, false) == 1);
    CHECK(simulation_lod_tier(s, 150, 0, false) == 2);
    CHECK(simulation_lod_tier(s, 500, 0, false) == 3);
    CHECK(simulation_lod_tier(s, INFINITY, 0, false) == 3);
    // Placed at tier 0, it stays until 33 m; then back only below 27 m.
    CHECK(simulation_lod_tier(s, 32, 0, true) == 0);
    CHECK(simulation_lod_tier(s, 34, 0, true) == 1);
    CHECK(simulation_lod_tier(s, 28, 1, true) == 1);
    CHECK(simulation_lod_tier(s, 26, 1, true) == 0);
    // Large jumps cross several tiers at once.
    CHECK(simulation_lod_tier(s, 1000, 0, true) == 3);
    CHECK(simulation_lod_tier(s, 1, 3, true) == 0);
}

TEST_CASE("simulation lod: due frames are staggered and per-entity time is conserved") {
    register_components();
    Scene s;
    Simulation sim;
    add_builtin_systems(sim);
    source(s, {0, 0, 0});
    std::vector<scene::Entity> near, far;
    for (int i = 0; i < 64; ++i) near.push_back(at(s, {static_cast<float>(i % 8), 0, 5}));
    for (int i = 0; i < 64; ++i) far.push_back(at(s, {500.0f + static_cast<float>(i), 0, 0}));
    const scene::Entity plain = at(s, {600, 0, 0}, false);

    // A LOD-aware system that integrates time per entity.
    std::vector<int> due_per_frame;
    std::vector<double> total(256, 0.0);
    sim.add_system("test.integrate", Phase::Update, [&](Scene& sc, const FrameTime& t) {
        int due = 0;
        sc.world().each<SimulationLodComponent>([&](scene::Entity e, SimulationLodComponent&) {
            const LodTick lt = simulation_lod_tick(sc, e, t.dt);
            if (!lt.due) return;
            ++due;
            total[e.index] += lt.dt;
        });
        due_per_frame.push_back(due);
        CHECK(simulation_lod_tick(sc, plain, t.dt).due); // no component: always due
    });
    sim.tick(s, kDt); // first frame: world transforms not yet computed, all at origin -> tier 0
    const int frames = 64;
    for (int i = 0; i < frames; ++i) sim.tick(s, kDt);

    const SimulationLodStats* st = simulation_lod_stats(s);
    REQUIRE(st);
    CHECK(st->entities == 128);
    CHECK(st->sources == 1);
    CHECK(st->tiers[0] == 64);
    CHECK(st->tiers[3] == 64);
    // Far agents tick every 8th frame; staggering keeps every frame's load close to 64 + 64/8.
    for (std::size_t f = 4; f < due_per_frame.size(); ++f) {
        CHECK(due_per_frame[f] >= 64);
        CHECK(due_per_frame[f] <= 64 + 24);
    }
    // Time is conserved: after a final full-rate frame everyone has simulated the same time.
    simulation_lod_settings(s).enabled = false;
    sim.tick(s, kDt);
    const double expected = (frames + 2) * static_cast<double>(kDt);
    for (const auto e : near) CHECK(total[e.index] == doctest::Approx(expected).epsilon(1e-4));
    for (const auto e : far) CHECK(total[e.index] == doctest::Approx(expected).epsilon(1e-4));
}

TEST_CASE("simulation lod: sources, camera fallback, bias, always_full and pause") {
    register_components();
    Scene s;
    Simulation sim;
    add_builtin_systems(sim);
    const scene::Entity a = at(s, {100, 0, 0});
    // No source and no camera: everything stays at full rate.
    sim.tick(s, kDt);
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 0);
    CHECK(simulation_lod_stats(s)->sources == 0);

    // A primary camera at the origin becomes the source.
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    sim.tick(s, kDt);
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 2);

    // An explicit source replaces the camera; its radius scale widens the tiers.
    const scene::Entity src = source(s, {0, 0, 0});
    s.world().get<SimulationLodSource>(src)->radius_scale = 4.0f; // 100 m reads as 25 m
    sim.tick(s, kDt);
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 0);
    s.world().get<SimulationLodSource>(src)->radius_scale = 1.0f;

    // Bias and always_full.
    s.world().get<SimulationLodComponent>(a)->bias = -80.0f; // reads as 20 m
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 0);
    s.world().get<SimulationLodComponent>(a)->bias = 1000.0f;
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 3);
    s.world().get<SimulationLodComponent>(a)->always_full = true;
    sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->tier == 0);
    CHECK(s.world().get<SimulationLodComponent>(a)->due);

    // Paused games accumulate nothing.
    const float pending = s.world().get<SimulationLodComponent>(a)->pending_dt;
    set_game_paused(s, true);
    for (int i = 0; i < 10; ++i) sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(a)->pending_dt == pending);
}

TEST_CASE("simulation lod: built-in rotator turns far entities by the same total angle") {
    register_components();
    Scene s;
    Simulation sim;
    add_builtin_systems(sim);
    source(s, {0, 0, 0});
    const scene::Entity near = at(s, {1, 0, 0});
    const scene::Entity far = at(s, {900, 0, 0});
    for (const auto e : {near, far}) s.world().add<RotatorComponent>(e, RotatorComponent{{0, 1, 0}, 90.0f});
    for (int i = 0; i < 2; ++i) sim.tick(s, kDt); // settle tiers
    // Run until the far rotator is due on the same frame (its interval is 8).
    simulation_lod_settings(s).intervals = {1, 1, 1, 8};
    for (int i = 0; i < 96; ++i) sim.tick(s, kDt);
    CHECK(s.world().get<SimulationLodComponent>(far)->tier == 3);
    simulation_lod_settings(s).enabled = false;
    sim.tick(s, kDt); // flush every pending delta
    const math::Quat qn = s.world().get<scene::Transform>(near)->rotation;
    const math::Quat qf = s.world().get<scene::Transform>(far)->rotation;
    const float dot = std::fabs(qn.x * qf.x + qn.y * qf.y + qn.z * qf.z + qn.w * qf.w);
    CHECK(dot == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("simulation lod: components round-trip through the scene format") {
    register_components();
    Scene s;
    const scene::Entity a = at(s, {1, 2, 3});
    s.world().get<SimulationLodComponent>(a)->bias = -12.5f;
    s.world().get<SimulationLodComponent>(a)->always_full = true;
    const scene::Entity src = source(s, {0, 0, 0});
    s.world().get<SimulationLodSource>(src)->radius_scale = 2.5f;
    const json::Value doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* c = t.world().get<SimulationLodComponent>(t.find(s.uuid(a)));
    REQUIRE(c);
    CHECK(c->bias == -12.5f);
    CHECK(c->always_full);
    const auto* so = t.world().get<SimulationLodSource>(t.find(s.uuid(src)));
    REQUIRE(so);
    CHECK(so->radius_scale == 2.5f);
}

TEST_CASE("simulation lod: assignment cost for 20,000 agents (Release evidence)") {
    register_components();
    Scene s;
    Simulation sim;
    add_builtin_systems(sim);
    source(s, {0, 0, 0});
    for (int i = 0; i < 20000; ++i) {
        const float a = static_cast<float>(i) * 0.37f;
        const float r = static_cast<float>(i % 400);
        at(s, {std::cos(a) * r, 0, std::sin(a) * r});
    }
    for (int i = 0; i < 3; ++i) sim.tick(s, kDt);
    const auto t0 = std::chrono::steady_clock::now();
    const int frames = 30;
    for (int i = 0; i < frames; ++i) update_simulation_lod(s, sim.time());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    const SimulationLodStats* st = simulation_lod_stats(s);
    REQUIRE(st);
    MESSAGE("simlod.assign 20000 agents: " << ms << " ms/frame; tiers " << st->tiers[0] << "/" << st->tiers[1] << "/"
                                           << st->tiers[2] << "/" << st->tiers[3]);
    CHECK(st->entities == 20000);
    CHECK(st->tiers[0] + st->tiers[1] + st->tiers[2] + st->tiers[3] == 20000);
}
