// SPDX-License-Identifier: Apache-2.0
// Particle emitter simulation tests (Slice 13).
#include "victus/particles/particles.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <cstring>

using namespace victus;
using namespace victus::particles;
using math::Vec3;
using scene::Entity;
using scene::Scene;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

std::vector<Billboard> billboards_of(Scene& s, Entity e) {
    std::vector<EmitterView> views;
    gather(s, views);
    for (const EmitterView& v : views)
        if (v.entity == e) return {v.billboards.begin(), v.billboards.end()};
    return {};
}

} // namespace

TEST_CASE("rate emission accumulates fractional particles exactly") {
    Scene s;
    const Entity e = s.create("Fx");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 30;
    c.lifetime_min = c.lifetime_max = 100;
    ParticleSystem sys;
    sys.update(s, 0.0f); // start
    for (int i = 0; i < 60; ++i) sys.update(s, 1.0f / 60.0f);
    // 60 frames of 1/60 s at 30/s = 30 particles (+-1 for float accumulation).
    const auto st = status(s, e);
    CHECK(st.valid);
    CHECK(st.alive >= 29);
    CHECK(st.alive <= 30);
    CHECK(sys.stats().emitters == 1);
    CHECK(sys.stats().alive == st.alive);
}

TEST_CASE("bursts, caps, lifetimes and non-looping cycles") {
    Scene s;
    const Entity e = s.create("Burst");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 500;
    c.max_particles = 64;
    c.duration = 1.0f;
    c.loop = false;
    c.lifetime_min = 0.5f;
    c.lifetime_max = 0.5f;
    ParticleSystem sys;
    sys.update(s, 0.0f);
    CHECK(status(s, e).alive == 64); // burst clamped to the cap
    CHECK(sys.stats().spawned == 64);
    sys.update(s, 0.4f);
    CHECK(status(s, e).alive == 64);
    sys.update(s, 0.2f); // age 0.6 > lifetime 0.5
    CHECK(status(s, e).alive == 0);
    CHECK(sys.stats().died == 64);
    sys.update(s, 0.6f); // cycle over: no new burst because loop is off
    CHECK(status(s, e).alive == 0);
    CHECK(status(s, e).finished);

    // Re-fetch: the first update added runtime state, which may move the component in the ECS.
    s.world().get<ParticleEmitterComponent>(e)->loop = true;
    reset(s, e);
    sys.update(s, 0.0f);
    CHECK(status(s, e).alive == 64);
    sys.update(s, 0.6f); // first burst died
    CHECK(status(s, e).alive == 0);
    sys.update(s, 0.5f); // t = 1.1: next cycle bursts again
    CHECK(status(s, e).alive == 64);
    CHECK(status(s, e).emitted == 192); // cumulative across reset()
}

TEST_CASE("emit queues extra particles even when not emitting") {
    Scene s;
    const Entity e = s.create("Dust");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.emitting = false;
    ParticleSystem sys;
    sys.update(s, 0.1f);
    CHECK(status(s, e).alive == 0);
    CHECK(emit(s, e, 12));
    sys.update(s, 0.0f);
    CHECK(status(s, e).alive == 12);
    CHECK_FALSE(emit(s, s.create("Plain"), 3));
    CHECK_FALSE(status(s, s.create("Other")).valid);
}

TEST_CASE("particles integrate gravity and drag and fade over their life") {
    Scene s;
    const Entity e = s.create("Spark");
    s.world().get<scene::Transform>(e)->position = {1, 2, 3};
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 1;
    c.shape = Shape::Point;
    c.angle = 0;
    c.speed_min = c.speed_max = 4;
    c.gravity = {0, -10, 0};
    c.lifetime_min = c.lifetime_max = 1;
    c.size_start = 1;
    c.size_end = 0;
    c.color_start = {1, 0.5f, 0, 1};
    c.color_end = {0, 0, 1, 0};
    c.intensity = 2;
    ParticleSystem sys;
    sys.update(s, 0.0f);
    auto b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    CHECK(near(b[0].position.x, 1));
    CHECK(near(b[0].position.y, 2));
    CHECK(near(b[0].size, 1));
    CHECK(near(b[0].color.r, 2)); // HDR intensity
    CHECK(near(b[0].color.a, 1));
    sys.update(s, 0.5f);
    b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    // Semi-implicit Euler: v = 4 - 10*0.5 = -1, y = 2 + (-1)*0.5.
    CHECK(near(b[0].position.y, 1.5f));
    CHECK(near(b[0].size, 0.5f));
    CHECK(near(b[0].color.a, 0.5f));
    CHECK(near(b[0].color.b, 1.0f)); // 0.5 * intensity 2

    // Drag damps velocity exponentially.
    auto& c2 = *s.world().get<ParticleEmitterComponent>(e);
    c2.gravity = {};
    c2.drag = 2;
    reset(s, e);
    sys.update(s, 0.0f);
    sys.update(s, 0.25f);
    b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    CHECK(near(b[0].position.y, 2 + 4 * std::exp(-0.5f) * 0.25f, 1e-3f));
}

TEST_CASE("emission follows the entity transform; local space follows it afterwards") {
    Scene s;
    const Entity parent = s.create("Rig");
    s.world().get<scene::Transform>(parent)->position = {10, 0, 0};
    const Entity e = s.create("Fx", parent);
    s.world().get<scene::Transform>(e)->rotation = math::Quat::axis_angle({0, 0, 1}, -3.14159265f * 0.5f); // +Y -> +X
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 1;
    c.shape = Shape::Point;
    c.angle = 0;
    c.speed_min = c.speed_max = 1;
    c.lifetime_min = c.lifetime_max = 10;
    ParticleSystem sys;
    sys.update(s, 0.0f);
    sys.update(s, 1.0f);
    auto b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    CHECK(near(b[0].position.x, 11, 1e-3f));
    CHECK(near(b[0].position.y, 0, 1e-3f));

    // World space: moving the emitter leaves the particle behind.
    s.world().get<scene::Transform>(parent)->position = {20, 0, 0};
    sys.update(s, 0.0f);
    CHECK(near(billboards_of(s, e)[0].position.x, 11, 1e-3f));
    // Local space: the particle moves with the emitter.
    s.world().get<ParticleEmitterComponent>(e)->world_space = false;
    reset(s, e);
    sys.update(s, 0.0f);
    sys.update(s, 1.0f);
    s.world().get<scene::Transform>(parent)->position = {30, 0, 0};
    sys.update(s, 0.0f);
    CHECK(near(billboards_of(s, e)[0].position.x, 31, 1e-3f));
}

TEST_CASE("shapes stay inside their volume and bounds cover every billboard") {
    for (const Shape shape : {Shape::Point, Shape::Sphere, Shape::Cone, Shape::Box}) {
        Scene s;
        const Entity e = s.create("Fx");
        auto& c = s.world().add<ParticleEmitterComponent>(e);
        c.shape = shape;
        c.rate = 0;
        c.burst = 200;
        c.max_particles = 200;
        c.radius = 0.5f;
        c.box = {2, 1, 0.5f};
        c.speed_min = c.speed_max = 0;
        c.size_start = c.size_end = 0.1f;
        ParticleSystem sys;
        sys.update(s, 0.0f);
        std::vector<EmitterView> views;
        gather(s, views);
        REQUIRE(views.size() == 1);
        REQUIRE(views[0].billboards.size() == 200);
        for (const Billboard& b : views[0].billboards) {
            const Vec3 p = b.position;
            switch (shape) {
            case Shape::Point: CHECK(math::length(p) < 1e-5f); break;
            case Shape::Sphere: CHECK(math::length(p) <= 0.5f + 1e-4f); break;
            case Shape::Cone:
                CHECK(near(p.y, 0));
                CHECK(std::sqrt(p.x * p.x + p.z * p.z) <= 0.5f + 1e-4f);
                break;
            case Shape::Box:
                CHECK(std::abs(p.x) <= 1.0f);
                CHECK(std::abs(p.y) <= 0.5f);
                CHECK(std::abs(p.z) <= 0.25f);
                break;
            }
            CHECK(views[0].bounds.min.x <= p.x - 0.05f + 1e-5f);
            CHECK(views[0].bounds.max.y >= p.y + 0.05f - 1e-5f);
        }
    }
}

TEST_CASE("cone spread limits directions to the half angle") {
    Scene s;
    const Entity e = s.create("Fx");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.shape = Shape::Point;
    c.angle = 30;
    c.rate = 0;
    c.burst = 256;
    c.speed_min = c.speed_max = 1;
    c.lifetime_min = c.lifetime_max = 10;
    ParticleSystem sys;
    sys.update(s, 0.0f);
    sys.update(s, 1.0f);
    const float cos30 = std::cos(30.0f * 3.14159265f / 180.0f);
    float min_cos = 1;
    for (const Billboard& b : billboards_of(s, e)) {
        const float cy = b.position.y / math::length(b.position);
        CHECK(cy >= cos30 - 1e-4f);
        min_cos = std::min(min_cos, cy);
    }
    CHECK(min_cos < 0.95f); // actually spreads
}

TEST_CASE("simulation is deterministic and identical serially or on jobs") {
    const auto run = [](jobs::JobSystem* js) {
        Scene s;
        for (int i = 0; i < 24; ++i) {
            const Entity e = s.create("Fx" + std::to_string(i));
            s.world().get<scene::Transform>(e)->position = {static_cast<float>(i), 0, 0};
            auto& c = s.world().add<ParticleEmitterComponent>(e);
            c.seed = static_cast<std::uint32_t>(i + 1);
            c.shape = Shape::Sphere;
            c.rate = 120;
            c.gravity = {0, -3, 0};
            c.drag = 0.5f;
            c.spin_min = -90;
            c.spin_max = 90;
            c.size_variation = 0.5f;
        }
        ParticleSystem sys({.jobs = js, .parallel_threshold = 2});
        for (int f = 0; f < 90; ++f) sys.update(s, 1.0f / 60.0f);
        std::vector<EmitterView> views;
        gather(s, views);
        std::vector<Billboard> all;
        for (const auto& v : views) all.insert(all.end(), v.billboards.begin(), v.billboards.end());
        return all;
    };
    const auto a = run(nullptr);
    const auto b = run(nullptr);
    jobs::JobSystem js(2);
    const auto c = run(&js);
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Billboard)) == 0);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(Billboard)) == 0);
}

TEST_CASE("seed 0 derives distinct streams from entity UUIDs") {
    Scene s;
    const Entity a = s.create("A");
    const Entity b = s.create("B");
    for (const Entity e : {a, b}) {
        auto& c = s.world().add<ParticleEmitterComponent>(e);
        c.rate = 0;
        c.burst = 8;
        c.shape = Shape::Sphere;
    }
    ParticleSystem sys;
    sys.update(s, 0.0f);
    const auto ba = billboards_of(s, a);
    const auto bb = billboards_of(s, b);
    REQUIRE(ba.size() == 8);
    REQUIRE(bb.size() == 8);
    CHECK(std::memcmp(ba.data(), bb.data(), sizeof(Billboard) * 8) != 0);
}

TEST_CASE("emitter components serialize; runtime state is rebuilt") {
    register_components();
    Scene s;
    const Entity e = s.create("Fire");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.shape = Shape::Box;
    c.blend = Blend::Additive;
    c.rate = 42;
    c.color_start = {4, 2, 0.5f, 1};
    c.texture = Uuid::generate();
    c.layer = 3;
    ParticleSystem sys;
    sys.update(s, 0.5f);
    const std::string text = json::stringify(s.save());
    CHECK(text.find("ParticleEmitter") != std::string::npos);
    Scene loaded;
    REQUIRE(loaded.load(json::parse(text).value()).ok());
    const Entity le = loaded.find_by_name("Fire");
    const auto* lc = loaded.world().get<ParticleEmitterComponent>(le);
    REQUIRE(lc);
    CHECK(lc->shape == Shape::Box);
    CHECK(lc->blend == Blend::Additive);
    CHECK(near(lc->rate, 42));
    CHECK(near(lc->color_start.r, 4));
    CHECK(lc->texture == c.texture);
    CHECK(lc->layer == 3);
    CHECK(status(loaded, le).alive == 0);

    // Removing the component drops its state; the system installs into a Simulation.
    gameplay::Simulation sim;
    add_particle_systems(sim, sys);
    sim.tick(loaded, 1.0f / 60.0f);
    CHECK(sys.stats().emitters == 1);
    loaded.world().remove<ParticleEmitterComponent>(le);
    sim.tick(loaded, 1.0f / 60.0f);
    CHECK(sys.stats().emitters == 0);
}
