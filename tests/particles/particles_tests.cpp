// SPDX-License-Identifier: Apache-2.0
// Particle emitter simulation tests (Slice 13).
#include "victus/particles/particles.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

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
    c.flipbook_columns = 4;
    c.flipbook_rows = 2;
    c.flipbook_frames = 6;
    c.flipbook_fps = 12;
    c.flipbook_random_start = true;
    c.velocity_stretch = 0.05f;
    c.trail_lifetime = 0.4f;
    c.trail_max_points = 9;
    c.trail_billboards = false;
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
    CHECK(lc->flipbook_columns == 4);
    CHECK(lc->flipbook_rows == 2);
    CHECK(lc->flipbook_frames == 6);
    CHECK(near(lc->flipbook_fps, 12));
    CHECK(lc->flipbook_random_start);
    CHECK(near(lc->velocity_stretch, 0.05f));
    CHECK(near(lc->trail_lifetime, 0.4f));
    CHECK(lc->trail_max_points == 9);
    CHECK_FALSE(lc->trail_billboards);
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

TEST_CASE("flipbook frames span the life once or loop at a fixed rate") {
    ParticleEmitterComponent c;
    CHECK(flipbook_frame_count(c) == 1);
    CHECK(flipbook_frame(c, 0.7f, 1.0f, 0) == 0);
    c.flipbook_columns = 4;
    c.flipbook_rows = 2;
    CHECK(flipbook_frame_count(c) == 8);
    CHECK(flipbook_frame(c, 0.0f, 1.0f, 0) == 0);
    CHECK(flipbook_frame(c, 0.5f, 1.0f, 0) == 4);
    CHECK(flipbook_frame(c, 0.999f, 1.0f, 0) == 7);
    CHECK(flipbook_frame(c, 1.0f, 1.0f, 0) == 7); // the last frame holds at the end of life
    CHECK(flipbook_frame(c, 0.5f, 1.0f, 6) == 2);  // start offset wraps
    c.flipbook_frames = 5;
    CHECK(flipbook_frame_count(c) == 5);
    CHECK(flipbook_frame(c, 0.999f, 1.0f, 0) == 4);
    c.flipbook_frames = 0;
    c.flipbook_fps = 10;
    CHECK(flipbook_frame(c, 0.25f, 1.0f, 0) == 2);
    CHECK(flipbook_frame(c, 0.85f, 1.0f, 0) == 0); // frame 8 wraps to 0
    c.flipbook_columns = 99;                         // clamped to 15
    CHECK(flipbook_frame_count(c) == 30);

    Billboard b;
    CHECK(flipbook_uv(b) == math::Vec4{0, 0, 1, 1});
    b.grid = 4 * 16 + 2;
    b.frame = 5; // column 1, row 1
    const math::Vec4 uv = flipbook_uv(b);
    CHECK(near(uv.x, 0.25f));
    CHECK(near(uv.y, 0.5f));
    CHECK(near(uv.z, 0.5f));
    CHECK(near(uv.w, 1.0f));
}

TEST_CASE("emitted billboards carry flipbook frames; the option keeps other randomness") {
    const auto run = [](bool flipbook, bool random_start) {
        Scene s;
        const Entity e = s.create("Smoke");
        auto& c = s.world().add<ParticleEmitterComponent>(e);
        c.rate = 0;
        c.burst = 32;
        c.lifetime_min = c.lifetime_max = 1.0f;
        c.seed = 7;
        if (flipbook) {
            c.flipbook_columns = 4;
            c.flipbook_rows = 4;
        }
        c.flipbook_random_start = random_start;
        ParticleSystem sys;
        sys.update(s, 0.0f);
        sys.update(s, 0.3f);
        return billboards_of(s, e);
    };
    const auto plain = run(false, false), book = run(true, false), random = run(true, true);
    REQUIRE(plain.size() == 32);
    REQUIRE(book.size() == 32);
    REQUIRE(random.size() == 32);
    std::set<int> starts;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        CHECK(plain[i].grid == 0);
        CHECK(plain[i].frame == 0);
        CHECK(book[i].grid == 4 * 16 + 4);
        CHECK(book[i].frame == 4); // 0.3 of the life into 16 frames
        CHECK(near(book[i].position.x, plain[i].position.x)); // flipbooks do not change the motion
        CHECK(near(book[i].size, plain[i].size));
        starts.insert(static_cast<int>(random[i].frame));
    }
    CHECK(starts.size() > 4); // random start frames vary per particle
}

TEST_CASE("velocity stretch lengthens moving particles along their motion") {
    Scene s;
    const Entity e = s.create("Sparks");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 8;
    c.shape = Shape::Point;
    c.angle = 0;
    c.speed_min = c.speed_max = 2;
    c.size_start = c.size_end = 0.2f;
    c.lifetime_min = c.lifetime_max = 10;
    c.velocity_stretch = 0.1f;
    // Emit along +X: rotate the entity's +Y axis onto +X.
    s.world().get<scene::Transform>(e)->rotation = math::Quat::axis_angle({0, 0, 1}, -1.5707963f);
    ParticleSystem sys;
    sys.update(s, 0.0f);
    std::vector<EmitterView> views;
    gather(s, views);
    REQUIRE(views.size() == 1);
    REQUIRE(views[0].velocities.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        const Billboard& b = views[0].billboards[i];
        CHECK(near(b.aspect, 1.0f + 2.0f * 0.1f / 0.2f, 1e-3f)); // length 0.2 + 2 m/s x 0.1 s
        CHECK(near(b.rotation, -1.5707963f, 1e-3f));              // up axis turned to +X
        CHECK(near(views[0].velocities[i].x, 2.0f, 1e-3f));
        CHECK(views[0].bounds.max.x - views[0].bounds.min.x >= 0.39f); // bounds cover the streak
    }
    // Without stretch there are no velocities and billboards stay square (`c` moved when the
    // system added its runtime state).
    s.world().get<ParticleEmitterComponent>(e)->velocity_stretch = 0;
    reset(s, e);
    sys.update(s, 0.0f);
    views.clear();
    gather(s, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0].velocities.empty());
    CHECK(views[0].billboards[0].aspect == 0);
}

TEST_CASE("stretched billboards aim along their on-screen motion for a camera") {
    const math::Mat4 front = math::Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    Billboard b;
    b.size = 0.2f;
    b.aspect = 3;
    aim_stretched(b, {1, 0, 0}, front);
    CHECK(near(b.rotation, -1.5707963f, 1e-3f)); // screen right
    CHECK(near(b.aspect, 3.0f, 1e-3f));
    b.aspect = 3;
    aim_stretched(b, {0, -1, 0}, front);
    CHECK(near(std::abs(b.rotation), 3.1415926f, 1e-3f)); // screen down
    b.aspect = 3;
    aim_stretched(b, {0, 0, 1}, front); // straight at the camera: no streak
    CHECK(near(b.aspect, 1.0f));
    b.aspect = 3;
    aim_stretched(b, math::normalize(Vec3{1, 0, 1}), front); // half across the view: shorter streak
    CHECK(near(b.aspect, 1.0f + 2.0f * 0.70710678f, 1e-3f));
    // Seen from above (looking down -Y, screen up = -Z), motion along +X is still screen right
    // and motion along -Z is screen up.
    const math::Mat4 top = math::Mat4::look_at({0, 5, 0}, {0, 0, 0}, {0, 0, -1});
    b.aspect = 3;
    aim_stretched(b, {1, 0, 0}, top);
    CHECK(near(b.rotation, -1.5707963f, 1e-3f));
    b.aspect = 3;
    aim_stretched(b, {0, 0, -1}, top);
    CHECK(near(b.rotation, 0.0f, 1e-3f));
    // Off-centre particles follow the perspective: left of centre, moving up and away from the
    // camera, a particle also drifts toward the centre (view p = (-2, 0, -5), d = (0, 1, -1):
    // screen direction (2, 5)).
    Billboard off;
    off.position = {-2, 0, 0};
    off.size = 0.2f;
    off.aspect = 3;
    aim_stretched(off, math::normalize(Vec3{0, 1, -1}), front);
    CHECK(near(off.rotation, std::atan2(-2.0f, 5.0f), 1e-3f));
}

namespace {

// One particle flying along +X at 1 m/s (gravity off), emitted at the origin.
Entity trail_emitter(Scene& s) {
    const Entity e = s.create("Trail");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 1;
    c.shape = Shape::Point;
    c.angle = 0;
    c.speed_min = c.speed_max = 1;
    c.size_start = c.size_end = 0.2f;
    c.color_start = c.color_end = {1, 0.5f, 0.25f, 0.8f};
    c.lifetime_min = c.lifetime_max = 100;
    c.trail_lifetime = 0.5f;
    c.trail_min_distance = 0.1f;
    c.trail_max_points = 64;
    s.world().get<scene::Transform>(e)->rotation = math::Quat::axis_angle({0, 0, 1}, -1.5707963f);
    return e;
}

EmitterView view_of(Scene& s, std::vector<EmitterView>& views) {
    views.clear();
    gather(s, views);
    REQUIRE(views.size() == 1);
    return views[0];
}

} // namespace

TEST_CASE("trails record points by distance, expire by age and taper toward the tail") {
    Scene s;
    const Entity e = trail_emitter(s);
    ParticleSystem sys;
    std::vector<EmitterView> views;
    sys.update(s, 0.0f); // spawn at the origin: a single point, no strip yet
    CHECK(view_of(s, views).ribbon_strips.empty());
    for (int i = 0; i < 20; ++i) sys.update(s, 0.05f); // 1 s at 1 m/s, a point every 0.1 m
    const EmitterView v = view_of(s, views);
    REQUIRE(v.ribbon_strips.size() == 1);
    const std::uint32_t n = v.ribbon_strips[0];
    REQUIRE(v.ribbon_points.size() == n);
    // Points older than 0.5 s expired: the trail spans about the last half metre.
    CHECK(n >= 5);
    CHECK(n <= 7);
    const RibbonPoint& tail = v.ribbon_points.front();
    const RibbonPoint& head = v.ribbon_points.back();
    CHECK(near(head.position.x, 1.0f, 1e-3f));
    CHECK(head.position.x - tail.position.x <= 0.5f + 1e-3f);
    CHECK(head.position.x - tail.position.x >= 0.35f);
    // Head: full width and alpha, v = 0; tail: tapered by its age.
    CHECK(near(head.v, 0));
    CHECK(near(head.width, 0.2f));
    CHECK(near(head.color.a, 0.8f));
    CHECK(near(head.color.r, 1.0f));
    CHECK(tail.v > 0.6f);
    CHECK(tail.width < head.width * 0.4f);
    CHECK(tail.color.a < 0.8f * 0.4f);
    for (std::uint32_t k = 1; k < n; ++k) {
        CHECK(v.ribbon_points[k].position.x > v.ribbon_points[k - 1].position.x); // tail to head
        CHECK(v.ribbon_points[k].v <= v.ribbon_points[k - 1].v);
    }
    CHECK(v.bounds.min.x <= tail.position.x);
    CHECK(v.bounds.max.x >= head.position.x);
    REQUIRE(v.billboards.size() == 1); // heads drawn by default

    // A full block drops its oldest points; a resize restarts the history.
    s.world().get<ParticleEmitterComponent>(e)->trail_max_points = 3;
    for (int i = 0; i < 10; ++i) sys.update(s, 0.05f);
    const EmitterView w = view_of(s, views);
    REQUIRE(w.ribbon_strips.size() == 1);
    CHECK(w.ribbon_strips[0] <= 4); // three recorded points + the live head
    CHECK(w.ribbon_points.back().position.x - w.ribbon_points.front().position.x <= 0.3f + 1e-3f);

    // Trails switched off: no ribbons.
    s.world().get<ParticleEmitterComponent>(e)->trail_lifetime = 0;
    sys.update(s, 0.05f);
    CHECK(view_of(s, views).ribbon_strips.empty());
}

TEST_CASE("trails follow local-space emitters, survive swap-removal and can draw alone") {
    Scene s;
    const Entity e = trail_emitter(s);
    {
        auto* c = s.world().get<ParticleEmitterComponent>(e);
        c->world_space = false;
        c->trail_billboards = false;
        c->burst = 0;
    }
    ParticleSystem sys;
    std::vector<EmitterView> views;
    sys.update(s, 0.0f);
    // Two particles with different lives: the first dies and the second is swapped into its slot,
    // carrying its own history.
    auto set_life = [&](float life, float speed) {
        auto* c = s.world().get<ParticleEmitterComponent>(e);
        c->lifetime_min = c->lifetime_max = life;
        c->speed_min = c->speed_max = speed;
    };
    set_life(0.3f, 3);
    emit(s, e, 1); // slot 0: fast, dies first
    sys.update(s, 0.0f);
    set_life(100, 1);
    emit(s, e, 1); // slot 1: moved into slot 0 with its history
    sys.update(s, 0.0f);
    for (int i = 0; i < 4; ++i) sys.update(s, 0.1f);
    CHECK(status(s, e).alive == 1);
    s.world().get<scene::Transform>(e)->position = {0, 10, 0}; // the emitter moved: its trails follow
    sys.update(s, 0.1f);
    const EmitterView v = view_of(s, views);
    CHECK(v.billboards.empty()); // trails only
    REQUIRE(v.ribbon_strips.size() == 1);
    for (const RibbonPoint& r : v.ribbon_points) {
        CHECK(near(r.position.y, 10.0f, 1e-3f));
        CHECK(r.position.x <= 0.5f + 1e-3f); // none of the dead fast particle's points
    }
    CHECK(near(v.ribbon_points.back().position.x, 0.5f, 1e-3f));
    CHECK(v.bounds.valid());
    CHECK(v.bounds.min.y > 9.8f);
}

// ---- GPU particles (ADR-0097): CPU bookkeeping, turbulence and the reference kernel ----

namespace {

const GpuEmitterView* gpu_view(Scene& s, Entity e, std::vector<EmitterView>& views) {
    gather(s, views);
    for (const EmitterView& v : views)
        if (v.entity == e) return v.gpu;
    return nullptr;
}

ParticleEmitterComponent& gpu_emitter(Scene& s, Entity e) {
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.gpu = true;
    c.rate = 200;
    c.burst = 0;
    c.max_particles = 64;
    c.lifetime_min = 0.2f;
    c.lifetime_max = 0.6f;
    c.seed = 1234;
    return c;
}

} // namespace

TEST_CASE("gpu emitters keep exact slot bookkeeping and deterministic spawn records") {
    const auto run = [](std::vector<GpuSpawnRecord>& all, std::vector<std::uint32_t>& alive) {
        Scene s;
        const Entity e = s.create("Gpu");
        gpu_emitter(s, e);
        ParticleSystem sys;
        sys.update(s, 0.0f);
        std::uint64_t seen = 0;
        double now = 0;
        for (int f = 0; f < 120; ++f) {
            sys.update(s, 1.0f / 60.0f);
            now += 1.0 / 60.0;
            std::vector<EmitterView> views;
            const GpuEmitterView* v = gpu_view(s, e, views);
            REQUIRE(v != nullptr);
            CHECK(views.front().billboards.empty());
            CHECK(v->capacity == 64);
            CHECK(std::abs(v->time - now) < 1e-5); // float steps summed in double
            // Records are ascending, every live record owns a distinct slot.
            std::set<std::uint32_t> slots;
            std::uint32_t live = 0;
            for (std::size_t i = 0; i < v->records.size(); ++i) {
                const GpuSpawnRecord& r = v->records[i];
                if (i > 0) CHECK(r.index == v->records[i - 1].index + 1);
                CHECK(r.particle.meta[0] < 64);
                CHECK(r.time <= v->time + 1e-12);
                if (r.time + r.particle.life > v->time) {
                    ++live;
                    CHECK(slots.insert(r.particle.meta[0]).second);
                }
                if (r.index >= seen) all.push_back(r);
            }
            CHECK(live == v->alive);
            CHECK(status(s, e).alive == v->alive);
            alive.push_back(v->alive);
            seen = v->spawned;
            CHECK(sys.stats().gpu_emitters == 1);
            CHECK(sys.stats().gpu_alive == v->alive);
        }
    };
    std::vector<GpuSpawnRecord> a, b;
    std::vector<std::uint32_t> alive_a, alive_b;
    run(a, alive_a);
    run(b, alive_b);
    REQUIRE(a.size() == b.size());
    CHECK(a.size() > 100);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(GpuSpawnRecord)) == 0);
    CHECK(alive_a == alive_b);
    // 200/s with a mean life of 0.4 s wants ~80 live particles: the 64-slot cap holds.
    CHECK(*std::max_element(alive_a.begin(), alive_a.end()) == 64);
}

TEST_CASE("gpu emitters reset, resize, finish and switch back to the CPU") {
    Scene s;
    const Entity e = s.create("Gpu");
    auto& c = gpu_emitter(s, e);
    ParticleSystem sys;
    sys.update(s, 0.0f);
    for (int i = 0; i < 10; ++i) sys.update(s, 0.05f);
    std::vector<EmitterView> views;
    const GpuEmitterView* v = gpu_view(s, e, views);
    REQUIRE(v != nullptr);
    const std::uint32_t gen = v->generation;
    const std::uint64_t instance = v->instance;
    CHECK(v->alive > 0);
    CHECK(views.front().bounds.valid());
    // reset() drops every particle: a new generation the renderer clears on.
    CHECK(reset(s, e));
    sys.update(s, 0.0f);
    v = gpu_view(s, e, views);
    CHECK(v->generation != gen);
    CHECK(v->instance == instance);
    CHECK(status(s, e).alive == 0);
    // A capacity change is also a new generation.
    const std::uint32_t gen2 = v->generation;
    s.world().get<ParticleEmitterComponent>(e)->max_particles = 16;
    sys.update(s, 0.05f);
    v = gpu_view(s, e, views);
    CHECK(v->generation != gen2);
    CHECK(v->capacity == 16);
    CHECK(v->alive <= 16);
    // A non-looping cycle finishes once its last particle has died.
    auto* cc = s.world().get<ParticleEmitterComponent>(e);
    cc->loop = false;
    cc->duration = 0.1f;
    CHECK(reset(s, e));
    for (int i = 0; i < 40 && !status(s, e).finished; ++i) sys.update(s, 0.05f);
    CHECK(status(s, e).finished);
    CHECK(status(s, e).alive == 0);
    // Back on the CPU path the emitter simulates billboards again.
    cc->gpu = false;
    cc->loop = true;
    CHECK(reset(s, e));
    for (int i = 0; i < 5; ++i) sys.update(s, 0.05f);
    views.clear();
    gather(s, views);
    CHECK(views.front().gpu == nullptr);
    CHECK(!views.front().billboards.empty());
    (void)c;
}

TEST_CASE("turbulence is a divergence-free swirling field") {
    ParticleEmitterComponent e;
    e.turbulence = 3;
    e.turbulence_frequency = 0.7f;
    float max_div = 0, mean_mag = 0;
    const float h = 1e-3f;
    int n = 0;
    for (float x = -2; x <= 2; x += 0.9f)
        for (float y = -2; y <= 2; y += 0.8f)
            for (float z = -2; z <= 2; z += 1.1f) {
                const Vec3 p{x, y, z};
                const float ph = 0.37f;
                const float div = (turbulence_at(e, p + Vec3{h, 0, 0}, ph).x - turbulence_at(e, p - Vec3{h, 0, 0}, ph).x +
                                   turbulence_at(e, p + Vec3{0, h, 0}, ph).y - turbulence_at(e, p - Vec3{0, h, 0}, ph).y +
                                   turbulence_at(e, p + Vec3{0, 0, h}, ph).z - turbulence_at(e, p - Vec3{0, 0, h}, ph).z) /
                                  (2 * h);
                max_div = std::max(max_div, std::abs(div));
                mean_mag += math::length(turbulence_at(e, p, ph));
                ++n;
            }
    mean_mag /= static_cast<float>(n);
    MESSAGE("turbulence: mean |a| " << mean_mag << ", max |div| " << max_div);
    CHECK(mean_mag > 0.5f);
    CHECK(max_div < 0.05f); // finite-difference noise only (an exact field has zero divergence)
    // The field evolves with time and is off at zero amplitude.
    CHECK(math::length(turbulence_at(e, {0.3f, 0.2f, 0.1f}, 0.0f) - turbulence_at(e, {0.3f, 0.2f, 0.1f}, 1.0f)) > 0.1f);
    e.turbulence = 0;
    CHECK(math::length(turbulence_at(e, {1, 2, 3}, 0.5f)) == 0.0f);
    // Phase wraps so it stays precise for long-running emitters.
    e.turbulence_speed = 2;
    CHECK(turbulence_phase(e, 1e6) < 6.2832f);
    CHECK(turbulence_phase(e, 1e6) >= 0.0f);
}

TEST_CASE("gpu reference kernel integrates ballistic motion and writes billboards") {
    ParticleEmitterComponent e;
    e.gravity = {0, -10, 0};
    e.drag = 0;
    e.size_start = 1;
    e.size_end = 0;
    e.color_start = {1, 0, 0, 1};
    e.color_end = {0, 0, 1, 0};
    e.intensity = 2;
    GpuParticle p;
    p.velocity = {2, 5, 0};
    p.life = 2;
    p.spin = 1;
    // Integrated in frame-sized pieces, as the GPU does across frames.
    for (int i = 0; i < 60; ++i) gpu_integrate(e, p, 1.0f / 60.0f, i / 60.0);
    CHECK(near(p.age, 1.0f, 1e-4f));
    CHECK(near(p.position.x, 2.0f, 1e-3f));
    CHECK(near(p.velocity.y, -5.0f, 1e-3f));
    CHECK(std::abs(p.position.y - (5.0f - 5.0f)) < 0.2f); // semi-implicit Euler, h = 1/60 s
    CHECK(near(p.rotation, 1.0f, 1e-4f));
    const Billboard b = gpu_billboard(e, p, math::Mat4::identity(), math::Mat4::identity());
    CHECK(near(b.size, 0.5f, 1e-3f));
    CHECK(near(b.color.r, 1.0f, 1e-3f)); // halfway, times intensity 2
    CHECK(near(b.color.b, 1.0f, 1e-3f));
    CHECK(near(b.color.a, 0.5f, 1e-3f));
    gpu_integrate(e, p, 1.5f, 1.0);
    CHECK(gpu_billboard(e, p, math::Mat4::identity(), math::Mat4::identity()).size == 0.0f); // dead
    // Turbulence off leaves the CPU path untouched: the field adds nothing.
    CHECK(math::length(turbulence_at(e, p.position, 0.3f)) == 0.0f);
}

TEST_CASE("sub-emitters burst where source particles die or are born, deterministically") {
    const auto setup = [](Scene& s, Entity& src, Entity& dst) {
        src = s.create("Firework");
        dst = s.create("Sparks");
        s.world().get<scene::Transform>(dst)->position = {5, 0, 0}; // spawns follow the events, not this
        auto& c = s.world().add<ParticleEmitterComponent>(src);
        c.rate = 0;
        c.burst = 10;
        c.shape = Shape::Point;
        c.angle = 30;
        c.speed_min = c.speed_max = 2;
        c.lifetime_min = c.lifetime_max = 0.5f;
        c.sub_emitter = s.uuid(dst);
        c.sub_emitter_count = 3;
        c.sub_emitter_inherit_velocity = 0.5f;
        auto& t = s.world().add<ParticleEmitterComponent>(dst);
        t.rate = 0;
        t.shape = Shape::Point;
        t.speed_min = t.speed_max = 0;
        t.lifetime_min = t.lifetime_max = 10;
    };
    Scene s;
    Entity src, dst;
    setup(s, src, dst);
    ParticleSystem sys;
    sys.update(s, 0.0f);
    CHECK(status(s, src).alive == 10);
    sys.update(s, 0.25f);
    CHECK(sys.stats().sub_emitter_events == 0);
    sys.update(s, 0.25f); // age 0.5: every source particle dies
    CHECK(status(s, src).alive == 0);
    CHECK(sys.stats().sub_emitter_events == 10);
    CHECK(sys.stats().sub_emitter_dropped == 0);
    CHECK(status(s, dst).alive == 0); // delivered on the target's next update
    sys.update(s, 0.0f);
    CHECK(status(s, dst).alive == 30);
    CHECK(sys.stats().spawned == 30);
    // Speed 2 for 0.5 s from the origin: every spark starts 1 m out, within the 30 degree cone.
    const auto sparks = billboards_of(s, dst);
    REQUIRE(sparks.size() == 30);
    for (const Billboard& b : sparks) {
        CHECK(near(math::length(b.position), 1.0f, 1e-3f));
        CHECK(b.position.y >= std::cos(30.0f * 3.14159265f / 180.0f) - 1e-3f);
    }
    // Inherited velocity (0.5 x 2 m/s outward) moves them a further 0.1 m in 0.1 s.
    sys.update(s, 0.1f);
    for (const Billboard& b : billboards_of(s, dst)) CHECK(near(math::length(b.position), 1.1f, 1e-3f));

    // Birth trigger, a chain (sparks -> embers on birth) and a self-reference that is dropped.
    const auto run = [&](jobs::JobSystem* js) {
        Scene sc;
        Entity a, b;
        setup(sc, a, b);
        const Entity embers = sc.create("Embers");
        auto& ec = sc.world().add<ParticleEmitterComponent>(embers);
        ec.rate = 0;
        ec.shape = Shape::Sphere;
        ec.lifetime_min = 0.2f;
        ec.lifetime_max = 0.6f;
        auto* bc = sc.world().get<ParticleEmitterComponent>(b);
        bc->sub_emitter = sc.uuid(embers);
        bc->sub_emitter_trigger = SubEmitterTrigger::Birth;
        bc->sub_emitter_count = 2;
        auto* ac = sc.world().get<ParticleEmitterComponent>(a);
        ac->rate = 40;
        ac->loop = true;
        const Entity self = sc.create("Self");
        auto& sf = sc.world().add<ParticleEmitterComponent>(self);
        sf.rate = 30;
        sf.lifetime_min = sf.lifetime_max = 0.1f;
        sf.sub_emitter = sc.uuid(self);
        std::uint32_t seed = 1; // UUIDs differ between runs: fix the streams
        sc.world().each<ParticleEmitterComponent>([&](Entity, ParticleEmitterComponent& pc) { pc.seed = seed++; });
        ParticleSystem ps({.jobs = js, .parallel_threshold = 2});
        std::uint32_t events = 0, dropped = 0;
        for (int f = 0; f < 90; ++f) {
            ps.update(sc, 1.0f / 60.0f);
            events += ps.stats().sub_emitter_events;
            dropped += ps.stats().sub_emitter_dropped;
        }
        CHECK(events > 0);
        CHECK(dropped > 0); // the self-targeting emitter
        CHECK(status(sc, embers).alive > 0);
        CHECK(status(sc, self).alive <= 4);
        std::vector<Billboard> all = billboards_of(sc, b);
        const auto e2 = billboards_of(sc, embers);
        all.insert(all.end(), e2.begin(), e2.end());
        return all;
    };
    const auto x = run(nullptr);
    jobs::JobSystem js(2);
    const auto y = run(&js);
    REQUIRE(!x.empty());
    REQUIRE(x.size() == y.size());
    CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(Billboard)) == 0);
}

TEST_CASE("sub-emitters feed GPU and local-space targets; missing targets are counted") {
    Scene s;
    const Entity src = s.create("Src");
    const Entity dst = s.create("GpuDst");
    const Entity loc = s.create("LocalDst");
    s.world().get<scene::Transform>(loc)->position = {0, 3, 0};
    auto& c = s.world().add<ParticleEmitterComponent>(src);
    c.rate = 0;
    c.burst = 4;
    c.shape = Shape::Point;
    c.speed_min = c.speed_max = 0;
    c.lifetime_min = c.lifetime_max = 0.1f;
    c.sub_emitter = s.uuid(dst);
    c.sub_emitter_count = 5;
    auto& g = s.world().add<ParticleEmitterComponent>(dst);
    g.gpu = true;
    g.shape = Shape::Point;
    g.rate = 0;
    g.max_particles = 64;
    g.speed_min = g.speed_max = 0;
    g.lifetime_min = g.lifetime_max = 5;
    auto& l = s.world().add<ParticleEmitterComponent>(loc);
    l.rate = 0;
    l.shape = Shape::Point;
    l.world_space = false;
    l.speed_min = l.speed_max = 0;
    l.lifetime_min = l.lifetime_max = 5;
    s.world().get<scene::Transform>(src)->position = {1, 0, 0};
    ParticleSystem sys;
    sys.update(s, 0.0f);
    sys.update(s, 0.2f); // all four die at (1, 0, 0)
    CHECK(sys.stats().sub_emitter_events == 4);
    sys.update(s, 0.0f);
    std::vector<EmitterView> views;
    gather(s, views);
    const GpuEmitterView* v = nullptr;
    for (const auto& ev : views)
        if (ev.entity == dst) v = ev.gpu;
    REQUIRE(v != nullptr);
    CHECK(v->alive == 20);
    REQUIRE(v->records.size() == 20);
    for (const GpuSpawnRecord& r : v->records) {
        CHECK(near(r.particle.position.x, 1.0f));
        CHECK(near(r.particle.position.y, 0.0f));
    }

    // Retarget to the local-space emitter: particles are stored relative to it, drawn in world space.
    s.world().get<ParticleEmitterComponent>(src)->sub_emitter = s.uuid(loc);
    reset(s, src);
    sys.update(s, 0.0f);
    sys.update(s, 0.2f);
    sys.update(s, 0.0f);
    const auto lb = billboards_of(s, loc);
    REQUIRE(lb.size() == 20);
    for (const Billboard& b : lb) {
        CHECK(near(b.position.x, 1.0f));
        CHECK(near(b.position.y, 0.0f));
    }
    s.world().get<scene::Transform>(loc)->position = {0, 4, 0}; // local-space particles follow it
    sys.update(s, 0.0f);
    for (const Billboard& b : billboards_of(s, loc)) CHECK(near(b.position.y, 1.0f));

    // A target that no longer exists drops the events.
    s.world().get<ParticleEmitterComponent>(src)->sub_emitter = Uuid::generate();
    reset(s, src);
    sys.update(s, 0.0f);
    sys.update(s, 0.2f);
    CHECK(sys.stats().sub_emitter_events == 0);
    CHECK(sys.stats().sub_emitter_dropped == 4);
}

TEST_CASE("over-life colour gradients and size curves replace the linear ramps (ADR-0209)") {
    ParticleEmitterComponent e;
    e.size_start = 2;
    e.size_end = 0;
    e.color_start = {1, 0, 0, 1};
    e.color_end = {0, 0, 1, 0};
    // Empty curves keep the old linear behaviour exactly.
    CHECK(near(size_at(e, 0.25f), 1.5f));
    CHECK(near(color_at(e, 0.25f).r, 0.75f));
    CHECK(near(max_size(e), 2));
    // Fire: white -> orange -> transparent dark red, with a step at 0.8.
    e.color_over_life = {{0.0f, {1, 1, 1, 1}}, {0.4f, {1, 0.5f, 0, 1}}, {0.8f, {0.5f, 0, 0, 0.6f}}, {0.8f, {0.2f, 0, 0, 0.3f}}, {1.0f, {0, 0, 0, 0}}};
    CHECK(near(color_at(e, -1).g, 1));       // before the first key: held
    CHECK(near(color_at(e, 0.2f).g, 0.75f)); // halfway to the second key
    CHECK(near(color_at(e, 0.6f).r, 0.75f));
    CHECK(near(color_at(e, 0.8f).r, 0.5f));  // equal times step: t == 0.8 hits the first
    CHECK(near(color_at(e, 0.9f).r, 0.1f));
    CHECK(near(color_at(e, 2).a, 0));        // after the last key: held
    // Size: pops to 3x, then settles at 1x.
    e.size_over_life = {{0.0f, 0.5f}, {0.1f, 3.0f}, {1.0f, 1.0f}};
    CHECK(near(size_at(e, 0.05f), 2 * 1.75f));
    CHECK(near(size_at(e, 0.55f), 2 * 2.0f));
    CHECK(near(size_at(e, 1.0f), 2));
    CHECK(near(max_size(e), 6));
    // Only kMaxCurveKeys keys count, in both lookups.
    std::vector<math::Vec2> many;
    for (int i = 0; i < 12; ++i) many.push_back({static_cast<float>(i) / 11.0f, static_cast<float>(i)});
    e.size_over_life = many;
    CHECK(near(size_at(e, 1.0f), 2 * 7.0f));
    CHECK(near(max_size(e), 14));
}

TEST_CASE("emitters draw billboards through the over-life curves; curves serialize") {
    register_components();
    Scene s;
    const Entity e = s.create("Smoke");
    auto& c = s.world().add<ParticleEmitterComponent>(e);
    c.rate = 0;
    c.burst = 1;
    c.shape = Shape::Point;
    c.speed_min = c.speed_max = 0;
    c.lifetime_min = c.lifetime_max = 1;
    c.size_start = 1;
    c.intensity = 2;
    c.color_over_life = {{0, {0, 0, 0, 0}}, {0.5f, {1, 0.5f, 0.25f, 1}}, {1, {0, 0, 0, 0}}};
    c.size_over_life = {{0, 0}, {0.5f, 4}};
    ParticleSystem sys;
    sys.update(s, 0.0f);
    sys.update(s, 0.25f);
    auto b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    CHECK(near(b[0].size, 2));
    CHECK(near(b[0].color.r, 1)); // 0.5 x intensity 2
    CHECK(near(b[0].color.a, 0.5f));
    sys.update(s, 0.5f);
    b = billboards_of(s, e);
    REQUIRE(b.size() == 1);
    CHECK(near(b[0].size, 4)); // held after the last point
    CHECK(near(b[0].color.g, 0.5f));
    std::vector<EmitterView> views;
    gather(s, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0].bounds.valid());

    const auto keys = s.world().get<ParticleEmitterComponent>(e)->color_over_life;
    CHECK(keys.size() == 3);
    const std::string text = json::stringify(s.save());
    CHECK(text.find("color_over_life") != std::string::npos);
    Scene loaded;
    REQUIRE(loaded.load(json::parse(text).value()).ok());
    const auto* lc = loaded.world().get<ParticleEmitterComponent>(loaded.find_by_name("Smoke"));
    REQUIRE(lc);
    CHECK(lc->color_over_life == keys);
    CHECK(lc->size_over_life.size() == 2);
    CHECK(near(lc->size_over_life[1].y, 4));
    // Malformed keys are rejected with the field named; four numbers mean opaque rgb.
    const auto* schema = scene::find_schema("ParticleEmitter");
    REQUIRE(schema);
    ParticleEmitterComponent tmp;
    auto bad = json::parse(R"({"color_over_life": [[0, 1, 1]]})");
    REQUIRE(bad.ok());
    const auto st = schema->load(&tmp, bad.value());
    CHECK_FALSE(st.ok());
    auto rgb = json::parse(R"({"color_over_life": [[0.5, 1, 0, 0]]})");
    REQUIRE(schema->load(&tmp, rgb.value()).ok());
    REQUIRE(tmp.color_over_life.size() == 1);
    CHECK(near(tmp.color_over_life[0].color.a, 1));
}

TEST_CASE("gpu reference billboards use the over-life curves") {
    ParticleEmitterComponent e;
    e.gpu = true;
    e.size_start = 0.5f;
    e.color_over_life = {{0, {1, 1, 0, 1}}, {1, {0, 0, 1, 0}}};
    e.size_over_life = {{0, 1}, {1, 3}};
    GpuParticle p;
    p.age = 0.5f;
    p.life = 2;
    p.size_scale = 1;
    const Billboard b = gpu_billboard(e, p, math::Mat4::identity(), math::Mat4::identity());
    CHECK(near(b.size, 0.5f * 1.5f));
    CHECK(near(b.color.r, 0.75f));
    CHECK(near(b.color.a, 0.75f));
}
