// SPDX-License-Identifier: Apache-2.0
// Inverse kinematics (ADR-0073): solver properties and the IKConstraint scene component.
#include "victus/animation/animation.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Quat;
using math::Vec3;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
bool near(Vec3 a, Vec3 b, float eps = 1e-3f) { return math::length(a - b) <= eps; }
float angle_between(Vec3 a, Vec3 b) {
    return std::acos(std::clamp(math::dot(math::normalize(a), math::normalize(b)), -1.0f, 1.0f));
}
Vec3 world_pos(Scene& s, Entity e) { return s.world_matrix(e).transform_point({}); }
Vec3 world_dir(Scene& s, Entity e, Vec3 local) { return math::normalize(s.world_matrix(e).transform_vector(local)); }

// Hip at (0, 2, 0); knee 1 m below, bent slightly forward (+Z); foot 1 m below the knee.
struct Leg {
    Scene scene;
    Entity root, hip, knee, foot;
    Leg() {
        root = scene.create("Character");
        hip = scene.create("Hip", root);
        knee = scene.create("Knee", hip);
        foot = scene.create("Foot", knee);
        t(hip).position = {0, 2, 0};
        t(knee).position = {0, -1, 0};
        t(hip).rotation = Quat::axis_angle({1, 0, 0}, -0.2f); // knee forward
        t(knee).rotation = Quat::axis_angle({1, 0, 0}, 0.4f);  // shin back
        t(foot).position = {0, -1, 0};
    }
    Transform& t(Entity e) { return *scene.world().get<Transform>(e); }
    IkConstraintComponent& ik() {
        if (!scene.world().has<IkConstraintComponent>(foot)) scene.world().add<IkConstraintComponent>(foot);
        return *scene.world().get<IkConstraintComponent>(foot);
    }
};

} // namespace

TEST_CASE("ik: rotation_between handles general, parallel and opposite directions") {
    const Quat q = rotation_between({1, 0, 0}, {0, 1, 0});
    CHECK(near(q.rotate({1, 0, 0}), {0, 1, 0}));
    CHECK(near(rotation_between({0, 0, 2}, {0, 0, 5}).rotate({0, 0, 1}), {0, 0, 1}));
    const Quat flip = rotation_between({0, 1, 0}, {0, -3, 0});
    CHECK(near(flip.rotate({0, 1, 0}), {0, -1, 0}));
    CHECK(near(rotation_between({}, {1, 0, 0}).w, 1.0f)); // degenerate input: identity
}

TEST_CASE("ik: two-bone solve reaches, keeps bone lengths and bends toward the hint") {
    const Vec3 root{0, 2, 0}, mid{0, 1, 0.2f}, tip{0, 0, 0};
    const Vec3 target{0.4f, 0.6f, 0.3f};
    const TwoBoneResult r = solve_two_bone(root, mid, tip, target, {0, 1, 5});
    CHECK(r.reached);
    CHECK(near(r.tip, target));
    CHECK(near(math::length(r.mid - root), math::length(mid - root)));
    CHECK(near(math::length(r.tip - r.mid), math::length(tip - mid)));
    CHECK(r.mid.z > 0.3f); // bends toward the pole in front

    const TwoBoneResult back = solve_two_bone(root, mid, tip, target, {0, 1, -5});
    CHECK(near(back.tip, target));
    CHECK(back.mid.z < r.mid.z); // pole behind: the knee bends the other way

    // Out of reach: straight toward the target, short of it, softened by max_extension.
    const Vec3 far{0, -3, 0};
    const TwoBoneResult o = solve_two_bone(root, mid, tip, far, mid, 0.99f);
    CHECK_FALSE(o.reached);
    CHECK(near(math::length(o.tip - root), (math::length(mid - root) + math::length(tip - mid)) * 0.99f, 2e-3f));
    CHECK(angle_between(o.tip - root, far - root) < 1e-3f);

    // Target on the root: clamped to the shortest reach, never NaN.
    const TwoBoneResult in = solve_two_bone(root, mid, tip, root, mid);
    CHECK_FALSE(in.reached);
    CHECK(std::isfinite(in.mid.x + in.mid.y + in.mid.z + in.tip.x + in.tip.y + in.tip.z));
}

TEST_CASE("ik: FABRIK reaches reachable targets and straightens toward unreachable ones") {
    std::vector<Vec3> p{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}};
    const std::vector<Vec3> orig = p;
    const FabrikResult r = solve_fabrik(p, {2, 2, 1}, 20, 1e-4f);
    CHECK(r.reached);
    CHECK(r.iterations >= 1);
    CHECK(near(p.back(), {2, 2, 1}, 2e-4f));
    CHECK(near(p.front(), orig.front()));
    for (std::size_t i = 0; i + 1 < p.size(); ++i) CHECK(near(math::length(p[i + 1] - p[i]), 1.0f));

    std::vector<Vec3> q = orig;
    const FabrikResult u = solve_fabrik(q, {10, 0, 0});
    CHECK_FALSE(u.reached);
    CHECK(near(q.back(), {4, 0, 0}));
    CHECK(near(u.error, 6.0f));
}

TEST_CASE("ik: FABRIK joint limits keep every bend inside its cone (ADR-0161)") {
    const auto angle = [](Vec3 a, Vec3 b) {
        return std::acos(std::clamp(math::dot(math::normalize(a), math::normalize(b)), -1.0f, 1.0f));
    };
    const std::vector<Vec3> orig{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}};
    const float limit = 20.0f * 3.14159265f / 180.0f;
    for (const Vec3 target : {Vec3{2, 2, 1}, Vec3{3, 1, 0}, Vec3{0, -2, 0.5f}, Vec3{10, 0, 0}}) {
        CAPTURE(target.x);
        CAPTURE(target.y);
        std::vector<Vec3> free = orig, lim = orig;
        const FabrikResult rf = solve_fabrik(free, target, 30, 1e-4f);
        const FabrikResult rl = solve_fabrik(lim, target, 30, 1e-4f, limit);
        CHECK(near(lim.front(), orig.front()));
        for (std::size_t i = 0; i + 1 < lim.size(); ++i) CHECK(near(math::length(lim[i + 1] - lim[i]), 1.0f));
        CHECK(angle(lim[1] - lim[0], orig[1] - orig[0]) <= limit + 1e-3f);
        for (std::size_t i = 1; i + 1 < lim.size(); ++i) CHECK(angle(lim[i + 1] - lim[i], lim[i] - lim[i - 1]) <= limit + 1e-3f);
        CHECK(rl.error + 1e-4f >= rf.error); // a limit never gets closer than the free chain
        CHECK(std::isfinite(rl.error));
    }
    // A target the cones allow is still reached (limited chains converge more slowly: ~2 mm after
    // 30 iterations here, measured).
    std::vector<Vec3> soft = orig;
    const FabrikResult rs = solve_fabrik(soft, {0.6f, 3.9f, 0}, 30, 1e-4f, limit);
    CHECK(rs.error < 0.005f);
    // 180 degrees behaves like no limit.
    std::vector<Vec3> a = orig, b = orig;
    (void)solve_fabrik(a, {2, 2, 1}, 20, 1e-4f);
    (void)solve_fabrik(b, {2, 2, 1}, 20, 1e-4f, 3.14159265f);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(near(a[i], b[i]));

    // The IKConstraint field applies it in the scene: a tail never folds back on itself.
    Scene scene;
    std::vector<Entity> tail{scene.create("Tail0")};
    for (int i = 1; i < 5; ++i) {
        tail.push_back(scene.create("Tail" + std::to_string(i), tail.back()));
        scene.world().get<Transform>(tail.back())->position = {0, 1, 0};
    }
    auto& c = scene.world().add<IkConstraintComponent>(tail.back());
    c.solver = IkSolver::Chain;
    c.chain_length = 4;
    c.iterations = 30;
    c.max_bend = 25.0f;
    c.target_position = {0, -1, 0.3f}; // behind the root: a free chain would fold over
    (void)apply_ik(scene);
    std::vector<Vec3> w;
    for (const Entity e : tail) w.push_back(world_pos(scene, e));
    for (std::size_t i = 1; i + 1 < w.size(); ++i)
        CHECK(angle(w[i + 1] - w[i], w[i] - w[i - 1]) <= 25.0f * 3.14159265f / 180.0f + 2e-3f);
    CHECK(angle(w[1] - w[0], {0, 1, 0}) <= 25.0f * 3.14159265f / 180.0f + 2e-3f);
}

TEST_CASE("ik: aim rotation points an axis at the target and respects the angle clamp") {
    const Quat q = aim_rotation({0, 0, 0}, {0, 0, 1}, {5, 0, 0}, kPi);
    CHECK(near(q.rotate({0, 0, 1}), {1, 0, 0}));
    const Quat c = aim_rotation({0, 0, 0}, {0, 0, 1}, {5, 0, 0}, kPi / 6);
    CHECK(near(angle_between(c.rotate({0, 0, 1}), {0, 0, 1}), kPi / 6));
    CHECK(near(aim_rotation({1, 1, 1}, {0, 0, 1}, {1, 1, 1}, kPi).w, 1.0f));
}

TEST_CASE("ik: two-bone constraint places the foot on the target in the scene") {
    Leg leg;
    auto& c = leg.ik();
    c.target_position = {0.3f, 0.5f, 0.0f};
    const float thigh = math::length(world_pos(leg.scene, leg.knee) - world_pos(leg.scene, leg.hip));
    const IkStats st = apply_ik(leg.scene);
    CHECK(st.constraints == 1);
    CHECK(st.solved == 1);
    CHECK(st.unreachable == 0);
    CHECK(near(world_pos(leg.scene, leg.foot), leg.ik().target_position)); // (re-fetched: IK adds runtime state)
    CHECK(near(world_pos(leg.scene, leg.hip), {0, 2, 0}));
    CHECK(near(math::length(world_pos(leg.scene, leg.knee) - world_pos(leg.scene, leg.hip)), thigh));
    CHECK(world_pos(leg.scene, leg.knee).z > 0.1f); // kept the animated forward knee
    CHECK(leg.t(leg.knee).position == Vec3{0, -1, 0}); // translations untouched

    // A pole entity behind the leg flips the bend.
    const Entity pole = leg.scene.create("Pole");
    leg.t(pole).position = {0, 1, -3};
    leg.ik().pole = leg.scene.uuid(pole);
    apply_ik(leg.scene);
    CHECK(near(world_pos(leg.scene, leg.foot), leg.ik().target_position));
    CHECK(world_pos(leg.scene, leg.knee).z < -0.1f);

    // A target entity with match_rotation also orients the foot.
    const Entity target = leg.scene.create("Target");
    leg.t(target).position = {-0.2f, 0.4f, 0.2f};
    leg.t(target).rotation = Quat::axis_angle({0, 1, 0}, 1.0f);
    leg.ik().target = leg.scene.uuid(target);
    leg.ik().match_rotation = true;
    apply_ik(leg.scene);
    CHECK(near(world_pos(leg.scene, leg.foot), {-0.2f, 0.4f, 0.2f}));
    CHECK(near(world_dir(leg.scene, leg.foot, {0, 0, 1}), leg.t(target).rotation.rotate({0, 0, 1})));
}

TEST_CASE("ik: partial weight on a procedural rig is stable from frame to frame") {
    Leg leg;
    auto& c = leg.ik();
    c.target_position = {0.5f, 0.5f, 0.2f};
    c.weight = 0.5f;
    const Quat hip0 = leg.t(leg.hip).rotation;
    apply_ik(leg.scene);
    const Quat hip1 = leg.t(leg.hip).rotation;
    const Vec3 foot1 = world_pos(leg.scene, leg.foot);
    CHECK_FALSE(near(foot1, leg.ik().target_position)); // half way only
    for (int i = 0; i < 10; ++i) apply_ik(leg.scene);
    CHECK(leg.t(leg.hip).rotation == hip1); // no drift toward the full solution
    CHECK(near(world_pos(leg.scene, leg.foot), foot1));

    // Weight 0 is skipped; an external pose change is taken as the new input.
    leg.t(leg.hip).rotation = hip0;
    leg.ik().weight = 1.0f;
    apply_ik(leg.scene);
    CHECK(near(world_pos(leg.scene, leg.foot), leg.ik().target_position));
}

TEST_CASE("ik: chain and look-at constraints") {
    Scene scene;
    // A five-joint tail along +Y from the origin.
    Entity base = scene.create("Tail0");
    std::vector<Entity> tail{base};
    for (int i = 1; i < 5; ++i) {
        tail.push_back(scene.create("Tail" + std::to_string(i), tail.back()));
        scene.world().get<Transform>(tail.back())->position = {0, 1, 0};
    }
    auto& chain = scene.world().add<IkConstraintComponent>(tail.back());
    chain.solver = IkSolver::Chain;
    chain.chain_length = 4;
    chain.iterations = 30;
    chain.tolerance = 1e-4f;
    chain.target_position = {1.5f, 2.5f, 1.0f};
    IkStats st = apply_ik(scene);
    CHECK(st.solved == 1);
    CHECK(near(world_pos(scene, tail.back()), {1.5f, 2.5f, 1.0f}, 2e-3f));
    for (std::size_t i = 0; i + 1 < tail.size(); ++i)
        CHECK(near(math::length(world_pos(scene, tail[i + 1]) - world_pos(scene, tail[i])), 1.0f));

    // Head (aim +Z) on a neck: the turn is shared and the head ends up facing the target.
    const Entity neck = scene.create("Neck");
    const Entity head = scene.create("Head", neck);
    scene.world().get<Transform>(neck)->position = {10, 1.5f, 0};
    scene.world().get<Transform>(head)->position = {0, 0.2f, 0};
    auto& look = scene.world().add<IkConstraintComponent>(head);
    look.solver = IkSolver::LookAt;
    look.chain_length = 1;
    look.max_angle = 180;
    look.target_position = {20, 1.7f, 0}; // +X, level with the head
    apply_ik(scene);
    CHECK(near(world_dir(scene, head, {0, 0, 1}), {1, 0, 0}, 2e-3f));
    const Vec3 neck_fwd = world_dir(scene, neck, {0, 0, 1});
    CHECK(near(angle_between(neck_fwd, {0, 0, 1}), kPi / 4, 2e-3f)); // half of the 90 degree turn

    auto& look2 = *scene.world().get<IkConstraintComponent>(head);
    look2.max_angle = 30;
    look2.chain_length = 0;
    scene.world().get<Transform>(neck)->rotation = {};
    scene.world().get<Transform>(head)->rotation = {};
    apply_ik(scene);
    CHECK(near(angle_between(world_dir(scene, head, {0, 0, 1}), {0, 0, 1}), kPi / 6, 2e-3f));
}

TEST_CASE("ik: invalid constraints are counted and leave the pose alone") {
    Leg leg;
    auto& c = leg.ik();
    c.solver = IkSolver::Chain;
    c.chain_length = 9; // deeper than the hierarchy
    const Quat before = leg.t(leg.hip).rotation;
    IkStats st = apply_ik(leg.scene);
    CHECK(st.invalid == 1);
    CHECK(st.solved == 0);
    CHECK(leg.t(leg.hip).rotation == before);

    leg.ik().solver = IkSolver::TwoBone;
    leg.ik().target = Uuid::generate(); // no such entity
    st = apply_ik(leg.scene);
    CHECK(st.invalid == 1);
    leg.ik().enabled = false;
    CHECK(apply_ik(leg.scene).constraints == 0);
}

TEST_CASE("ik: IKConstraint serializes and runs after the animated pose") {
    register_components();
    Leg leg;
    auto& c = leg.ik();
    c.solver = IkSolver::TwoBone;
    c.weight = 0.75f;
    c.target_position = {0.1f, 0.4f, 0.3f};
    c.max_extension = 0.98f;
    const json::Value doc = leg.scene.save();
    Scene copy;
    REQUIRE(copy.load(doc));
    const Entity foot = copy.find_by_name("Foot");
    const auto* loaded = copy.world().get<IkConstraintComponent>(foot);
    REQUIRE(loaded);
    CHECK(loaded->solver == IkSolver::TwoBone);
    CHECK(near(loaded->weight, 0.75f));
    CHECK(near(loaded->target_position, {0.1f, 0.4f, 0.3f}));
    CHECK(near(loaded->max_extension, 0.98f));

    // An Animator swings the hip every frame; IK still lands the foot after each update.
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Hip", .parent = -1, .rest = {.translation = {0, 2, 0}}});
    Clip swing{.name = "Swing", .duration = 1.0f};
    Channel ch{.node = 0, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, 1}};
    for (const float a : {-0.5f, 0.5f}) {
        const Quat q = Quat::axis_angle({1, 0, 0}, a);
        ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
    }
    swing.channels.push_back(ch);
    set->clips.push_back(swing);
    const Uuid model = Uuid::generate();
    AnimationSystem system;
    system.add_set(model, set);
    auto& a = leg.scene.world().add<AnimatorComponent>(leg.root);
    a.model = model;
    a.clip = "Swing";
    leg.ik().weight = 1.0f;
    for (int i = 0; i < 4; ++i) {
        system.update(leg.scene, 0.2f);
        CHECK(system.stats().ik.solved == 1);
        CHECK(near(world_pos(leg.scene, leg.foot), leg.ik().target_position));
    }
    std::size_t lines = 0;
    ik_lines(leg.scene, [&](Vec3, Vec3) { ++lines; });
    CHECK(lines == 5); // two bones + three target cross strokes
}
