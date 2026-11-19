// SPDX-License-Identifier: Apache-2.0
// Particle emitter simulation tests (Slice 13).
#include "victus/particles/particles.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

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
