// SPDX-License-Identifier: Apache-2.0
// Reference creature and mechanical rigs (assets/reference_rigs.hpp, ADR-0207): skeleton
// structure and roles, weight hygiene, deformation quality under every clip with linear and dual
// quaternion skinning, rigidity of hard-surface parts, and the CPU cost of posing and skinning.
// The GPU side (gpu_rig_deformation_tests) renders the same poses in the runtime renderer.
#include "doctest.h"
#include "victus/assets/reference_rigs.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace victus;
using namespace victus::assets;
using math::Quat;
using math::Vec3;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct ClipMetrics {
    render::DeformationMetrics lbs, dqs;
};

ClipMetrics measure(const CookedModel& m, const animation::Clip* clip, float t,
                    std::span<const std::pair<std::uint32_t, Quat>> overrides = {}) {
    const render::MeshData& d = m.meshes[0].data;
    const auto rest = pose_palette(m, pose_world(m, nullptr, 0));
    const auto posed = pose_palette(m, pose_world(m, clip, t, overrides));
    std::vector<render::Vertex3D> v_rest, v_l, v_d;
    render::skin_vertices(d, rest, render::SkinningMethod::Linear, v_rest);
    render::skin_vertices(d, posed, render::SkinningMethod::Linear, v_l);
    render::skin_vertices(d, posed, render::SkinningMethod::DualQuaternion, v_d);
    return {render::measure_deformation(v_rest, v_l, d.indices), render::measure_deformation(v_rest, v_d, d.indices)};
}

std::uint32_t node_of(const CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<std::uint32_t>(i);
    FAIL("missing node " << name);
    return 0;
}

} // namespace

TEST_CASE("reference rigs: the quadruped is a valid, fully weighted creature skeleton") {
    const CookedModel m = make_reference_rig(ReferenceRig::Quadruped);
    REQUIRE(validate_model(m).ok());
    const SkeletonInfo info = describe_skeleton(m);
    MESSAGE("quadruped: " << info.joints << " joints, " << info.sockets << " sockets, depth " << info.max_depth << ", "
                          << m.meshes[0].data.vertices.size() << " vertices, " << m.triangle_count() << " triangles");
    CHECK(info.joints == 38);
    CHECK(info.sockets == 2);
    CHECK(info.unweighted_joints == 0);
    CHECK(info.count(kBoneTail) == 7);
    CHECK(info.count(kBoneLeg) == 10); // upper/lower legs, thighs and hocks (feet, toes and shoulders have their own roles)
    CHECK(info.count(kBoneJaw) == 1);
    CHECK(info.count(kBoneEar) == 2);
    CHECK(info.count(kBoneSecondary) == 1); // Dyn.Belly
    CHECK(info.max_depth >= 8);             // Rig > Hips > Tail1..7
    const render::SkinWeightReport w = render::analyze_skin_weights(m.meshes[0].data, m.skins[0].joints.size());
    CHECK(w.unnormalized == 0);
    CHECK(w.out_of_range == 0);
    CHECK(w.zero_weight == 0);
    CHECK(w.influence_histogram[3] + w.influence_histogram[4] == 0); // two-bone blends only
    CHECK(w.unweighted_joints() == 0);
    REQUIRE(m.animations.size() == 3);
}

TEST_CASE("reference rigs: the quadruped keeps its volume and surface through every clip with both methods") {
    // Measured, not assumed: DQS is not uniformly better. On the Sit clip (hind knees bent ~125
    // degrees) DQS collapses a couple of triangles on the inside of the bend where LBS collapses
    // none (the DQS bulge pushes the inner surface into itself), while on twists it is far better
    // (next test). Both stay within 0.1% collapsed triangles on authored clips.
    const CookedModel m = make_reference_rig(ReferenceRig::Quadruped);
    for (const auto& clip : m.animations) {
        float worst_vol_l = 0, worst_vol_d = 0;
        std::size_t col_l = 0, col_d = 0;
        for (int i = 0; i <= 10; ++i) {
            const ClipMetrics c = measure(m, &clip, clip.duration * static_cast<float>(i) / 10.0f);
            worst_vol_l = std::max(worst_vol_l, std::abs(c.lbs.volume_ratio - 1));
            worst_vol_d = std::max(worst_vol_d, std::abs(c.dqs.volume_ratio - 1));
            col_l = std::max(col_l, c.lbs.collapsed_triangles);
            col_d = std::max(col_d, c.dqs.collapsed_triangles);
        }
        MESSAGE(clip.name << ": worst volume change LBS " << worst_vol_l << " DQS " << worst_vol_d << ", worst collapsed tris LBS "
                          << col_l << " DQS " << col_d);
        CHECK(worst_vol_d < 0.05f); // authored clips stay within 5% volume
        CHECK(worst_vol_l < 0.05f);
        const std::size_t tris = m.meshes[0].data.indices.size() / 3;
        CHECK(col_l * 1000 <= tris);
        CHECK(col_d * 1000 <= tris);
    }
}

TEST_CASE("reference rigs: an extreme creature pose pinches with LBS and holds with DQS") {
    // Tail rolled 120 degrees per joint (corkscrew), neck turned 100 degrees, a hind leg folded
    // past its range: the limits a creature rig meets in gameplay (ragdolls, procedural looks).
    const CookedModel m = make_reference_rig(ReferenceRig::Quadruped);
    std::vector<std::pair<std::uint32_t, Quat>> pose;
    for (int i = 1; i <= 7; ++i) pose.emplace_back(node_of(m, "Tail" + std::to_string(i)), Quat::axis_angle({0, 0, 1}, 2.1f));
    pose.emplace_back(node_of(m, "Neck1"), Quat::axis_angle({0, 1, 0}, 1.0f));
    pose.emplace_back(node_of(m, "Neck2"), Quat::axis_angle({0, 1, 0}, 0.75f) * Quat::axis_angle({0, 0, 1}, 1.2f));
    pose.emplace_back(node_of(m, "HindLowerLeg.L"), Quat::axis_angle({1, 0, 0}, 2.6f));
    pose.emplace_back(node_of(m, "FrontLowerLeg.R"), Quat::axis_angle({0, 1, 0}, 2.4f));
    const ClipMetrics c = measure(m, nullptr, 0, pose);
    MESSAGE("extreme creature pose: volume LBS " << c.lbs.volume_ratio << " DQS " << c.dqs.volume_ratio << ", collapsed LBS "
                                                 << c.lbs.collapsed_triangles << " DQS " << c.dqs.collapsed_triangles
                                                 << ", min area LBS " << c.lbs.min_area_ratio << " DQS " << c.dqs.min_area_ratio);
    CHECK(c.lbs.collapsed_triangles > 0);
    CHECK(c.dqs.collapsed_triangles < c.lbs.collapsed_triangles);
    CHECK(std::abs(c.dqs.volume_ratio - 1) < std::abs(c.lbs.volume_ratio - 1));
}

TEST_CASE("reference rigs: the mech's rigid parts never deform, whatever the method or pose") {
    const CookedModel m = make_reference_rig(ReferenceRig::Mech);
    REQUIRE(validate_model(m).ok());
    const SkeletonInfo info = describe_skeleton(m);
    CHECK(info.joints == 13);
    CHECK(info.count(kBoneMechanical) >= 12); // Mech.* and Wheel.*
    CHECK(info.sockets == 1);
    const render::SkinWeightReport w = render::analyze_skin_weights(m.meshes[0].data, m.skins[0].joints.size());
    CHECK(w.influence_histogram[1] == w.vertices); // every vertex rigid on one joint
    for (const auto& clip : m.animations)
        for (int i = 0; i <= 8; ++i) {
            const ClipMetrics c = measure(m, &clip, clip.duration * static_cast<float>(i) / 8.0f);
            for (const auto* d : {&c.lbs, &c.dqs}) {
                CHECK(d->min_area_ratio == doctest::Approx(1.0f).epsilon(1e-3));
                CHECK(d->max_area_ratio == doctest::Approx(1.0f).epsilon(1e-3));
                CHECK(d->volume_ratio == doctest::Approx(1.0f).epsilon(1e-3));
                CHECK(d->flipped_triangles == 0);
            }
        }
}

TEST_CASE("reference rigs: CPU pose, palette and skinning cost") {
    for (const ReferenceRig r : {ReferenceRig::Quadruped, ReferenceRig::Mech}) {
        const CookedModel m = make_reference_rig(r);
        const render::MeshData& d = m.meshes[0].data;
        constexpr int kIters = 200;
        std::vector<render::Vertex3D> out;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<math::Mat4> pal;
        for (int i = 0; i < kIters; ++i) pal = pose_palette(m, pose_world(m, &m.animations[0], 0.005f * static_cast<float>(i)));
        const double pose_ms = ms_since(t0) / kIters;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) render::skin_vertices(d, pal, render::SkinningMethod::Linear, out);
        const double lbs_ms = ms_since(t0) / kIters;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) render::skin_vertices(d, pal, render::SkinningMethod::DualQuaternion, out);
        const double dqs_ms = ms_since(t0) / kIters;
        const std::size_t palette_bytes = pal.size() * sizeof(math::Mat4);
        const std::size_t skin_bytes = d.skin.size() * sizeof(render::SkinVertex);
        const std::string label = r == ReferenceRig::Mech ? "mech" : "quadruped";
        MESSAGE(label << ": pose+palette " << pose_ms << " ms, CPU skin LBS " << lbs_ms << " ms, DQS " << dqs_ms << " ms ("
                << d.vertices.size() << " verts); palette " << palette_bytes << " B, skin stream " << skin_bytes << " B");
        CHECK(palette_bytes == m.skins[0].joints.size() * 64);
        CHECK(sizeof(render::SkinVertex) == 24);
        CHECK(pose_ms < 5.0);
    }
}
