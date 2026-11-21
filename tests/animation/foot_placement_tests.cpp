// SPDX-License-Identifier: Apache-2.0
// Foot placement and foot locking (ADR-0163) on a procedural biped over analytic ground, and
// through the physics ground probe on real colliders.
#include "victus/animation/animation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/runtime/physics_ground_probe.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <functional>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Quat;
using math::Vec3;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kDt = 1.0f / 60.0f;
Vec3 world_pos(Scene& s, Entity e) { return s.world_matrix(e).transform_point({}); }

// Ground height as a function of (x, z); the probe returns where the ray meets it.
GroundProbe analytic(std::function<float(float, float)> height, std::function<Vec3(float, float)> normal = {}) {
    return [height, normal](Vec3 o, Vec3 d, float max, std::uint32_t, Entity) -> std::optional<GroundHit> {
        if (d.y > -0.99f) return std::nullopt;
        const float h = height(o.x, o.z);
        if (o.y < h || o.y - max > h) return std::nullopt;
        return GroundHit{{o.x, h, o.z}, normal ? normal(o.x, o.z) : Vec3{0, 1, 0}};
    };
}

// Root on the ground at the origin, hips 1 m up, legs 0.45 + 0.45 m with the knees slightly
// forward, ankles about 0.1 m above the ground, feet facing -Z.
struct Biped {
    Scene scene;
    Entity root, hips, thigh[2], shin[2], foot[2];
    Biped() {
        register_components();
        root = scene.create("Character");
        hips = scene.create("Hips", root);
        t(hips).position = {0, 1, 0};
        for (int i = 0; i < 2; ++i) {
            const float side = i == 0 ? -1.0f : 1.0f; // 0 = left (-X), 1 = right (+X)
            thigh[i] = scene.create(i == 0 ? "UpperLeg.L" : "UpperLeg.R", hips);
            shin[i] = scene.create(i == 0 ? "LowerLeg.L" : "LowerLeg.R", thigh[i]);
            foot[i] = scene.create(i == 0 ? "Foot.L" : "Foot.R", shin[i]);
            t(thigh[i]).position = {side * 0.15f, 0, 0};
            t(thigh[i]).rotation = Quat::axis_angle({1, 0, 0}, -0.1f);
            t(shin[i]).position = {0, -0.45f, 0};
            t(shin[i]).rotation = Quat::axis_angle({1, 0, 0}, 0.2f);
            t(foot[i]).position = {0, -0.45f, 0};
            t(foot[i]).rotation = Quat::axis_angle({1, 0, 0}, -0.1f); // foot level again
            auto& ik = scene.world().add<IkConstraintComponent>(foot[i]);
            ik.solver = IkSolver::TwoBone;
            scene.world().add<FootContactComponent>(foot[i]);
        }
        auto& fp = scene.world().add<FootPlacementComponent>(root);
        fp.pelvis = scene.uuid(hips);
        fp.smoothing = 0; // exact by default; smoothing has its own test
    }
    Transform& t(Entity e) { return *scene.world().get<Transform>(e); }
    FootPlacementComponent& fp() { return *scene.world().get<FootPlacementComponent>(root); }
    Vec3 ankle(int i) { return world_pos(scene, foot[i]); }
};

} // namespace

TEST_CASE("foot placement: feet follow steps and holes, the pelvis drops for the lower foot") {
    Biped b;
    const Vec3 rest[2] = {b.ankle(0), b.ankle(1)};
    CHECK(rest[0].y == doctest::Approx(rest[1].y));
    CHECK(rest[0].y > 0.05f);
    CHECK(rest[0].y < 0.15f);

    SUBCASE("a step under the left foot raises it; the pelvis stays") {
        AnimationSystem sys({.ground = analytic([](float x, float) { return x < 0 ? 0.15f : 0.0f; })});
        sys.update(b.scene, kDt);
        CHECK(sys.stats().feet.characters == 1);
        CHECK(sys.stats().feet.feet == 2);
        CHECK(sys.stats().feet.grounded == 2);
        CHECK(sys.stats().feet.invalid == 0);
        CHECK(b.ankle(0).y == doctest::Approx(rest[0].y + 0.15f).epsilon(0.005));
        CHECK(b.ankle(1).y == doctest::Approx(rest[1].y).epsilon(0.005));
        CHECK(world_pos(b.scene, b.hips).y == doctest::Approx(1.0f));
        // Feet stay under themselves horizontally.
        CHECK(std::abs(b.ankle(0).x - rest[0].x) < 0.01f);
    }
    SUBCASE("a hole under the right foot lowers the pelvis so the foot reaches") {
        AnimationSystem sys({.ground = analytic([](float x, float) { return x > 0 ? -0.2f : 0.0f; })});
        sys.update(b.scene, kDt);
        CHECK(world_pos(b.scene, b.hips).y == doctest::Approx(0.8f).epsilon(0.001));
        CHECK(b.ankle(1).y == doctest::Approx(rest[1].y - 0.2f).epsilon(0.005));
        CHECK(b.ankle(0).y == doctest::Approx(rest[0].y).epsilon(0.005)); // knee bends more instead
        CHECK(b.t(b.hips).position.y == doctest::Approx(0.8f));
    }
    SUBCASE("the pelvis drop is capped") {
        b.fp().max_pelvis_drop = 0.1f;
        AnimationSystem sys({.ground = analytic([](float x, float) { return x > 0 ? -0.3f : 0.0f; })});
        sys.update(b.scene, kDt);
        CHECK(world_pos(b.scene, b.hips).y == doctest::Approx(0.9f).epsilon(0.001));
    }
    SUBCASE("no ground within the probe leaves the animated pose") {
        AnimationSystem sys({.ground = analytic([](float, float) { return -5.0f; })});
        sys.update(b.scene, kDt);
        CHECK(sys.stats().feet.grounded == 0);
        CHECK(b.ankle(0).y == doctest::Approx(rest[0].y));
        CHECK(world_pos(b.scene, b.hips).y == doctest::Approx(1.0f));
    }
    SUBCASE("without a probe (edit-mode preview) the feet keep the animated pose, wherever the body moves") {
        AnimationSystem sys;
        sys.update(b.scene, kDt);
        CHECK(sys.stats().feet.grounded == 0);
        b.t(b.root).position = {2, 0, 3}; // the IK targets must not stay behind
        sys.update(b.scene, kDt);
        CHECK(b.t(b.hips).position.y == doctest::Approx(1.0f));
        CHECK(math::length(b.ankle(0) - (rest[0] + Vec3{2, 0, 3})) < 1e-3f);
    }
}

TEST_CASE("foot placement: procedural rigs do not drift, and smoothing converges without popping") {
    Biped b;
    const Vec3 rest = b.ankle(1);
    AnimationSystem sys({.ground = analytic([](float x, float) { return x > 0 ? -0.2f : 0.0f; })});
    SUBCASE("repeated frames keep the same solution") {
        for (int i = 0; i < 120; ++i) sys.update(b.scene, kDt);
        CHECK(world_pos(b.scene, b.hips).y == doctest::Approx(0.8f).epsilon(0.001));
        CHECK(b.ankle(1).y == doctest::Approx(rest.y - 0.2f).epsilon(0.005));
    }
    SUBCASE("smoothing") {
        b.fp().smoothing = 10.0f;
        sys.update(b.scene, kDt);
        const float first = world_pos(b.scene, b.hips).y;
        CHECK(first < 1.0f);
        CHECK(first > 0.95f); // one frame moves a fraction (1 - e^-1/6 = 15 %) of the way
        float prev = first;
        bool monotonic = true;
        for (int i = 0; i < 120; ++i) {
            sys.update(b.scene, kDt);
            const float y = world_pos(b.scene, b.hips).y;
            monotonic = monotonic && y <= prev + 1e-5f;
            prev = y;
        }
        CHECK(monotonic);
        CHECK(prev == doctest::Approx(0.8f).epsilon(0.002));
    }
}

TEST_CASE("foot placement: feet tilt to the ground normal, limited by max_slope") {
    Biped b;
    const float slope = 0.3f; // rises toward +Z
    const Vec3 n = math::normalize(Vec3{0, 1, -slope});
    AnimationSystem sys({.ground = analytic([&](float, float z) { return z * slope; }, [&](float, float) { return n; })});
    sys.update(b.scene, kDt);
    const auto foot_up = [&](int i) { return math::normalize(b.scene.world_matrix(b.foot[i]).transform_vector({0, 1, 0})); };
    const float angle = std::acos(std::clamp(math::dot(foot_up(0), n), -1.0f, 1.0f));
    CHECK(angle < 0.02f);
    SUBCASE("max_slope clamps the tilt") {
        b.fp().max_slope = 5.0f;
        for (int i = 0; i < 3; ++i) sys.update(b.scene, kDt);
        const float tilt = std::acos(std::clamp(math::dot(foot_up(1), {0, 1, 0}), -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
        CHECK(tilt == doctest::Approx(5.0f).epsilon(0.05));
    }
    SUBCASE("align_to_ground off keeps the animated foot rotation") {
        b.fp().align_to_ground = false;
        for (int i = 0; i < 3; ++i) sys.update(b.scene, kDt);
        CHECK(math::dot(foot_up(1), {0, 1, 0}) > 0.9999f);
    }
}

TEST_CASE("foot locking: a slow planted foot does not slide until it drifts too far") {
    Biped b;
    AnimationSystem sys({.ground = analytic([](float, float) { return 0.0f; })});
    sys.update(b.scene, kDt);
    const Vec3 planted = b.ankle(0);
    // The character creeps forward at 0.1 m/s with the legs not animated: an in-place pose that
    // would slide the feet along the ground.
    const float speed = 0.1f;
    int frames = 0;
    for (; frames < 60; ++frames) {
        b.t(b.root).position.z -= speed * kDt;
        sys.update(b.scene, kDt);
    }
    CHECK(sys.stats().feet.locked == 2);
    CHECK(std::abs(b.ankle(0).z - planted.z) < 0.005f); // stayed put while the body moved 0.1 m
    CHECK(std::abs(b.ankle(0).x - planted.x) < 0.005f);
    // Keep creeping: after release_distance (0.25 m) the foot lets go and catches up.
    for (; frames < 240; ++frames) {
        b.t(b.root).position.z -= speed * kDt;
        sys.update(b.scene, kDt);
    }
    const float behind = b.ankle(0).z - (world_pos(b.scene, b.root).z + planted.z);
    CHECK(std::abs(behind) < 0.26f); // never further than the release distance from the animation

    SUBCASE("a fast foot never locks") {
        for (int i = 0; i < 30; ++i) {
            b.t(b.root).position.z -= 2.0f * kDt;
            sys.update(b.scene, kDt);
        }
        CHECK(sys.stats().feet.locked == 0);
    }
    SUBCASE("lock_feet off lets the feet follow the body") {
        b.fp().lock_feet = false;
        const Vec3 before = b.ankle(1);
        for (int i = 0; i < 60; ++i) {
            b.t(b.root).position.z -= speed * kDt;
            sys.update(b.scene, kDt);
        }
        CHECK(sys.stats().feet.locked == 0);
        CHECK(b.ankle(1).z < before.z - 0.05f);
    }
}

TEST_CASE("foot placement: one-step humanoid setup finds the pelvis and both feet, and removes cleanly") {
    Biped b;
    // Start bare: only the skeleton.
    REQUIRE(remove_foot_placement(b.scene, b.root) == 5); // FootPlacement + 2 x (FootContact + IKConstraint)
    CHECK_FALSE(b.scene.world().has<FootPlacementComponent>(b.root));
    const Entity toe = b.scene.create("Toe.L", b.foot[0]);
    b.t(toe).position = {0, -0.08f, -0.12f};
    CHECK(make_humanoid_foot_placement(b.scene, b.root) == 2);
    CHECK(b.scene.world().get<FootPlacementComponent>(b.root)->pelvis == b.scene.uuid(b.hips));
    CHECK(b.scene.world().has<FootContactComponent>(b.foot[1]));
    CHECK_FALSE(b.scene.world().has<FootContactComponent>(toe));
    CHECK(b.scene.world().get<IkConstraintComponent>(b.foot[0])->solver == IkSolver::TwoBone);
    AnimationSystem sys({.ground = analytic([](float x, float) { return x > 0 ? -0.2f : 0.0f; })});
    sys.update(b.scene, kDt);
    CHECK(sys.stats().feet.grounded == 2);
    CHECK(sys.stats().feet.invalid == 0);
    CHECK(make_humanoid_foot_placement(b.scene, b.foot[0]) == 0); // nothing foot-like below a foot
}

TEST_CASE("foot placement: misconfigured feet are reported, components serialize") {
    Biped b;
    b.scene.world().get<IkConstraintComponent>(b.foot[1])->solver = IkSolver::Chain;
    AnimationSystem sys({.ground = analytic([](float, float) { return 0.0f; })});
    sys.update(b.scene, kDt);
    CHECK(sys.stats().feet.feet == 1);
    CHECK(sys.stats().feet.invalid == 1);

    b.fp().probe_down = 0.9f;
    b.fp().lock_feet = false;
    b.scene.world().get<FootContactComponent>(b.foot[0])->lock = false;
    const json::Value doc = b.scene.save();
    Scene copy;
    REQUIRE(copy.load(doc));
    const Entity root = copy.find(b.scene.uuid(b.root));
    const auto* fp = copy.world().get<FootPlacementComponent>(root);
    REQUIRE(fp);
    CHECK(fp->probe_down == doctest::Approx(0.9f));
    CHECK_FALSE(fp->lock_feet);
    CHECK(fp->pelvis == b.scene.uuid(b.hips));
    const auto* fc = copy.world().get<FootContactComponent>(copy.find(b.scene.uuid(b.foot[0])));
    REQUIRE(fc);
    CHECK_FALSE(fc->lock);
}

TEST_CASE("foot placement: the physics ground probe plants feet on colliders and ignores the character") {
    Biped b;
    physics::register_components();
    // Floor at y = 0 and a 0.2 m box under the left foot.
    const auto box = [&](Vec3 pos, Vec3 half) {
        const Entity e = b.scene.create("Box");
        b.t(e).position = pos;
        physics::ColliderComponent c;
        c.size = half * 2.0f;
        b.scene.world().add<physics::ColliderComponent>(e, c);
    };
    box({0, -0.5f, 0}, {5, 0.5f, 5});
    box({-0.3f, 0.1f, 0}, {0.2f, 0.1f, 0.5f});
    // The character's own capsule must not count as ground.
    // (thin, so it stands between the feet without touching the step).
    b.scene.world().add<physics::CharacterControllerComponent>(b.root).radius = 0.08f;
    auto pw = physics::PhysicsWorld::create({});
    REQUIRE(pw.ok());
    auto world = std::move(pw.value());
    world->step(b.scene, kDt);
    const Vec3 rest = b.ankle(1);
    AnimationSystem sys({.ground = runtime::physics_ground_probe(*world)});
    sys.update(b.scene, kDt);
    CHECK(sys.stats().feet.grounded == 2);
    CHECK(b.ankle(0).y == doctest::Approx(rest.y + 0.2f).epsilon(0.01));
    CHECK(b.ankle(1).y == doctest::Approx(rest.y).epsilon(0.01));
    // Straight down through the capsule: the character is ignored, the floor is found.
    const GroundProbe probe = runtime::physics_ground_probe(*world);
    const auto through = probe({0, 3, 0}, {0, -1, 0}, 5, physics::kAllLayers, b.root);
    REQUIRE(through);
    CHECK(through->point.y == doctest::Approx(0.0f).epsilon(0.01));
    const auto blocked = world->raycast({0, 3, 0}, {0, -1, 0}, 5);
    REQUIRE(blocked);
    CHECK(blocked->point.y > 1.0f); // without the ignore the capsule is hit
}
