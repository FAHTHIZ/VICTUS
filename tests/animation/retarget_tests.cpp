// SPDX-License-Identifier: Apache-2.0
// Retargeting (ADR-0075): bone-name mapping, rest-orientation correction, root translation
// scaling across differently proportioned and differently oriented rigs, and Animator
// clip_source integration.
#include "victus/animation/animation.hpp"
#include "victus/animation/retarget.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Quat;
using math::Vec3;
using scene::Entity;
using scene::Scene;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float angle_between(Quat a, Quat b) {
    const float d = std::min(1.0f, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    return 2.0f * std::acos(d);
}
bool near3(Vec3 a, Vec3 b, float eps = 1e-3f) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}

Channel rot(std::uint32_t node, std::vector<float> times, std::vector<Quat> qs) {
    Channel ch{.node = node, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = std::move(times)};
    for (const Quat& q : qs) ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
    return ch;
}
Channel tr(std::uint32_t node, std::vector<float> times, std::vector<Vec3> vs) {
    Channel ch{.node = node, .path = Path::Translation, .interpolation = Interpolation::Linear, .times = std::move(times)};
    for (const Vec3& v : vs) ch.values.insert(ch.values.end(), {v.x, v.y, v.z});
    return ch;
}

// World rotations/positions of a pose (rotation and translation only).
struct Pose {
    std::vector<Quat> r;
    std::vector<Vec3> p;
};
Pose world(const AnimationSet& set, const std::vector<NodePose>& local) {
    Pose w{std::vector<Quat>(local.size()), std::vector<Vec3>(local.size())};
    for (std::size_t i = 0; i < local.size(); ++i) {
        const std::int32_t pa = set.nodes[i].parent;
        if (pa < 0) {
            w.r[i] = local[i].rotation;
            w.p[i] = local[i].translation;
        } else {
            const auto pi = static_cast<std::size_t>(pa);
            w.r[i] = normalize(w.r[pi] * local[i].rotation);
            w.p[i] = w.p[pi] + w.r[pi].rotate(local[i].translation);
        }
    }
    return w;
}
std::vector<NodePose> sampled(const AnimationSet& set, const Clip& clip, float t) {
    std::vector<NodePose> pose(set.nodes.size());
    for (std::size_t i = 0; i < pose.size(); ++i) pose[i] = set.nodes[i].rest;
    sample(clip, t, pose);
    return pose;
}

// Source: Hips (y = 1) -> Spine -> Arm; bone axes aligned with the parent.
std::shared_ptr<AnimationSet> source_rig() {
    auto s = std::make_shared<AnimationSet>();
    s->nodes.push_back({.name = "mixamorig:Hips", .parent = -1, .rest = {.translation = {0, 1, 0}}});
    s->nodes.push_back({.name = "mixamorig:Spine", .parent = 0, .rest = {.translation = {0, 0.2f, 0}}});
    s->nodes.push_back({.name = "mixamorig:Left_Arm", .parent = 1, .rest = {.translation = {0.3f, 0.3f, 0}}});
    Clip wave{.name = "Wave", .duration = 1};
    wave.channels.push_back(rot(2, {0, 1}, {Quat{}, Quat::axis_angle({0, 1, 0}, kPi / 2)}));
    wave.channels.push_back(rot(0, {0, 1}, {Quat{}, Quat::axis_angle({0, 1, 0}, kPi / 2)}));
    wave.channels.push_back(tr(0, {0, 1}, {{0, 1, 0}, {0, 1, 1}})); // hips travel +1 z
    s->clips.push_back(std::move(wave));
    Clip idle{.name = "Idle", .duration = 1};
    idle.channels.push_back(rot(1, {0, 1}, {Quat{}, Quat::axis_angle({1, 0, 0}, 0.2f)}));
    s->clips.push_back(std::move(idle));
    return s;
}

// Target: a Z-up "Armature" (rotated -90 degrees about X, unmapped) above Hips at world height 2,
// an extra unmapped Chest between Spine and Arm, and an arm whose rest rotation differs.
const Quat kArmature = Quat::axis_angle({1, 0, 0}, -kPi / 2);
const Quat kArmRest = Quat::axis_angle({1, 0, 0}, kPi / 2);
std::shared_ptr<AnimationSet> target_rig() {
    auto t = std::make_shared<AnimationSet>();
    t->nodes.push_back({.name = "Armature", .parent = -1, .rest = {.rotation = kArmature}});
    t->nodes.push_back({.name = "Hips", .parent = 0, .rest = {.translation = {0, 0, 2}}}); // world (0, 2, 0)
    t->nodes.push_back({.name = "spine", .parent = 1, .rest = {.translation = {0, 0, 0.5f}}});
    t->nodes.push_back({.name = "Chest", .parent = 2, .rest = {.translation = {0, 0, 0.4f}}});
    t->nodes.push_back({.name = "LeftArm", .parent = 3, .rest = {.translation = {0.6f, 0, 0.2f}, .rotation = kArmRest}});
    Clip idle{.name = "Idle", .duration = 2}; // the target's own Idle wins over the source's
    idle.channels.push_back(rot(2, {0, 2}, {Quat{}, Quat{}}));
    t->clips.push_back(std::move(idle));
    return t;
}

} // namespace

TEST_CASE("retarget: bone names normalize and map exactly, loosely and explicitly") {
    CHECK(normalize_bone_name("mixamorig:LeftUpLeg") == "leftupleg");
    CHECK(normalize_bone_name("Armature|Left_Arm") == "leftarm");
    CHECK(normalize_bone_name("Spine.001") == "spine001");
    CHECK(normalize_bone_name("") .empty());
    const auto src = source_rig(), tgt = target_rig();
    const auto map = map_bones(*src, *tgt);
    REQUIRE(map.size() == 5);
    CHECK(map[0] == -1); // Armature
    CHECK(map[1] == 0);
    CHECK(map[2] == 1);  // "spine" ~ "mixamorig:Spine"
    CHECK(map[3] == -1); // Chest
    CHECK(map[4] == 2);  // "LeftArm" ~ "mixamorig:Left_Arm"
    RetargetOptions explicit_map;
    explicit_map.bone_map = {{"Chest", "mixamorig:Spine"}};
    const auto m2 = map_bones(*src, *tgt, explicit_map);
    CHECK(m2[3] == 1);
    CHECK(m2[2] == -1); // explicit mappings claim their source bone before automatic matches
}

TEST_CASE("retarget: identical rigs reproduce the source pose") {
    const auto src = source_rig();
    const auto map = map_bones(*src, *src);
    const Clip out = retarget_clip(*src, src->clips[0], *src, map);
    for (const float t : {0.0f, 0.3f, 1.0f}) {
        const auto a = sampled(*src, src->clips[0], t), b = sampled(*src, out, t);
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(angle_between(a[i].rotation, b[i].rotation) < 1e-3f);
            CHECK(near3(a[i].translation, b[i].translation));
        }
    }
}

TEST_CASE("retarget: world rotation changes carry over different rest orientations; proportions stay") {
    const auto src = source_rig(), tgt = target_rig();
    const auto map = map_bones(*src, *tgt);
    const Clip out = retarget_clip(*src, src->clips[0], *tgt, map);
    const Pose s0 = world(*src, sampled(*src, src->clips[0], 0)), t0 = world(*tgt, sampled(*tgt, out, 0));
    for (const float time : {0.25f, 0.5f, 1.0f}) {
        const Pose s = world(*src, sampled(*src, src->clips[0], time));
        const auto tl = sampled(*tgt, out, time);
        const Pose t = world(*tgt, tl);
        // Same world-space change for every mapped bone.
        for (const auto [ti, si] : {std::pair{1, 0}, std::pair{2, 1}, std::pair{4, 2}}) {
            const Quat ds = normalize(s.r[si] * s0.r[si].conjugate());
            const Quat dt = normalize(t.r[ti] * t0.r[ti].conjugate());
            CHECK(angle_between(ds, dt) < 2e-3f);
        }
        // Non-root bones keep the target's own lengths; unmapped bones keep their rest local pose.
        CHECK(near3(tl[4].translation, tgt->nodes[4].rest.translation));
        CHECK(near3(tl[2].translation, tgt->nodes[2].rest.translation));
        CHECK(angle_between(tl[3].rotation, Quat{}) < 1e-3f);
        CHECK(angle_between(tl[0].rotation, kArmature) < 1e-3f);
        // Root travel scaled by the root height ratio (2 / 1) and carried through the Z-up parent.
        const Vec3 travel_src = s.p[0] - s0.p[0];
        CHECK(near3(t.p[1] - t0.p[1], travel_src * 2.0f));
    }
    CHECK(near3(t0.p[1], {0, 2, 0}));
}

TEST_CASE("retarget: dense clips resample to the rate cap; no mapped bones gives no set") {
    const auto src = source_rig(), tgt = target_rig();
    Clip dense{.name = "Dense", .duration = 1};
    std::vector<float> times;
    std::vector<Quat> qs;
    for (int i = 0; i <= 1000; ++i) {
        times.push_back(static_cast<float>(i) / 1000.0f);
        qs.push_back(Quat::axis_angle({0, 1, 0}, static_cast<float>(i) / 1000.0f));
    }
    dense.channels.push_back(rot(2, times, qs));
    const Clip out = retarget_clip(*src, dense, *tgt, map_bones(*src, *tgt), {.max_sample_rate = 30});
    REQUIRE_FALSE(out.channels.empty());
    CHECK(out.channels[0].times.size() <= 32);
    CHECK(validate(out, tgt->nodes.size()));

    AnimationSet other;
    other.nodes.push_back({.name = "Tail", .parent = -1, .rest = {}});
    RetargetReport rep;
    CHECK(retarget_set(*src, other, {}, &rep) == nullptr);
    CHECK(rep.mapped_bones == 0);
    REQUIRE(rep.unmapped.size() == 1);
    CHECK(rep.unmapped[0] == "Tail");

    const auto merged = retarget_set(*src, *tgt, {}, &rep);
    REQUIRE(merged);
    CHECK(rep.mapped_bones == 3);
    CHECK(rep.clips == 1);         // Wave
    CHECK(rep.skipped_clips == 1); // Idle: the target's own
    CHECK(merged->clips.size() == 2);
    CHECK(merged->clips[static_cast<std::size_t>(merged->find_clip("Idle"))].duration == 2.0f);
    for (const Clip& c : merged->clips) CHECK(validate(c, merged->nodes.size()));
}

TEST_CASE("retarget: Animator clip_source plays another model's clips on this rig") {
    Scene scene;
    AnimationSystem system;
    const Uuid src_id = Uuid::generate(), tgt_id = Uuid::generate();
    system.add_set(src_id, source_rig());
    system.add_set(tgt_id, target_rig());
    const Entity root = scene.create("Character");
    const Entity armature = scene.create("Armature", root);
    const Entity hips = scene.create("Hips", armature);
    const Entity spine = scene.create("spine", hips);
    const Entity chest = scene.create("Chest", spine);
    const Entity arm = scene.create("LeftArm", chest);
    auto& a = scene.world().add<AnimatorComponent>(root);
    a.model = tgt_id;
    a.clip = "Wave";
    a.crossfade = 0;
    system.update(scene, 0.5f);
    const bool played_before = system.status(scene, root).valid && system.status(scene, root).duration > 0;
    CHECK_FALSE(played_before); // own clips lack Wave
    scene.world().get<AnimatorComponent>(root)->clip_source = src_id;
    system.update(scene, 0);
    system.play(scene, root, "Wave", 0);
    system.update(scene, 0.5f);
    const AnimatorStatus st = system.status(scene, root);
    CHECK(st.valid);
    CHECK(st.clip == "Wave");
    CHECK(st.duration == doctest::Approx(1.0f));
    const auto clips = system.clips(scene, root);
    CHECK(std::find(clips.begin(), clips.end(), "Wave") != clips.end());
    // Hips moved half of the scaled travel in world z: local -y under the Z-up armature.
    CHECK(near3(scene.world().get<scene::Transform>(hips)->position, {0, -1, 2}));
    CHECK(angle_between(scene.world().get<scene::Transform>(armature)->rotation, Quat{}) < 1e-3f); // unanimated: untouched
    CHECK(near3(scene.world().get<scene::Transform>(arm)->position, {0.6f, 0, 0.2f}));
    (void)chest;
    system.invalidate(src_id); // retargeted sets involving the model are rebuilt on next use
    system.clear_cache();
}
