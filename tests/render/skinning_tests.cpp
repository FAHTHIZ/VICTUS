// SPDX-License-Identifier: Apache-2.0
// CPU skinning reference, dual quaternion skinning, weight tools and deformation metrics
// (render/skinning.hpp, ADR-0205).
#include "doctest.h"
#include "skin_test_meshes.hpp"
#include "victus/render/skinning.hpp"

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::render;
using math::Mat4;
using math::Quat;
using math::Vec3;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
using test::joint1;
using test::make_tube;
using test::mid_ring_radius;

bool near(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::fabs(a.c[c][r] - b.c[c][r]) > eps) return false;
    return true;
}

} // namespace

TEST_CASE("skinning: dual quaternions round-trip rigid and scaled joint matrices") {
    const Quat qs[] = {Quat{}, Quat::axis_angle({0, 1, 0}, 0.7f), Quat::axis_angle({1, 2, 3}, 2.9f),
                       Quat::axis_angle({0, 0, 1}, kPi), Quat::axis_angle({1, 0, 0}, -3.1f)};
    for (const Quat q : qs)
        for (const Vec3 s : {Vec3{1, 1, 1}, Vec3{0.5f, 0.5f, 0.5f}, Vec3{1, 2, 0.75f}}) {
            const Mat4 m = Mat4::trs({0.3f, -1.2f, 2.5f}, q, s);
            Vec3 scale;
            const DualQuat dq = to_dual_quat(m, &scale);
            CHECK(near(to_matrix(dq, scale), m));
            const Mat4 enc = encode_dq_joint(m);
            CHECK(is_dq_joint(enc));
            CHECK_FALSE(is_dq_joint(m));
            // A single full influence reproduces the joint exactly with either method.
            SkinVertex v;
            v.joints[0] = 0;
            const Mat4 pal[1] = {m};
            CHECK(near(blend_skin_matrix(v, pal, SkinningMethod::Linear), m));
            CHECK(near(blend_skin_matrix(v, pal, SkinningMethod::DualQuaternion), m));
        }
    // Mirrored joints keep their handedness.
    const Mat4 mirror = Mat4::trs({1, 0, 0}, Quat::axis_angle({0, 1, 0}, 0.4f), {-1, 1, 1});
    Vec3 s;
    const DualQuat mirror_dq = to_dual_quat(mirror, &s); // before reading s: argument order is unspecified
    CHECK(near(to_matrix(mirror_dq, s), mirror));
    std::vector<Mat4> palette = {mirror, Mat4::identity()};
    encode_dq_palette(palette);
    CHECK(is_dq_joint(palette[0]));
    CHECK(is_dq_joint(palette[1]));
    encode_dq_palette(palette); // idempotent
    CHECK(palette[1].c[0].w == doctest::Approx(1.0f));
}

TEST_CASE("skinning: dual quaternions keep a twisted forearm's volume where linear blending collapses") {
    const MeshData tube = make_tube();
    std::vector<Vertex3D> lbs, dqs;
    // 170 degree roll of the child joint about the bone axis: the candy-wrapper case.
    const Mat4 pal[2] = {Mat4::identity(), joint1(Quat::axis_angle({1, 0, 0}, math::radians(170.0f)))};
    skin_vertices(tube, pal, SkinningMethod::Linear, lbs);
    skin_vertices(tube, pal, SkinningMethod::DualQuaternion, dqs);
    const float r_lbs = mid_ring_radius(lbs), r_dqs = mid_ring_radius(dqs);
    MESSAGE("twist 170: mid-ring radius LBS " << r_lbs << " DQS " << r_dqs << " (rest 0.2)");
    CHECK(r_lbs < 0.04f);                              // collapsed to a fraction of the rest radius
    CHECK(r_dqs == doctest::Approx(0.2f).epsilon(0.02)); // DQS keeps it
    const DeformationMetrics ml = measure_deformation(tube.vertices, lbs, tube.indices);
    const DeformationMetrics md = measure_deformation(tube.vertices, dqs, tube.indices);
    MESSAGE("twist 170: volume LBS " << ml.volume_ratio << " DQS " << md.volume_ratio << ", collapsed tris LBS "
                                       << ml.collapsed_triangles << " DQS " << md.collapsed_triangles);
    CHECK(ml.volume_ratio < 0.85f);
    CHECK(md.volume_ratio == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(ml.collapsed_triangles > md.collapsed_triangles);
    CHECK(md.collapsed_triangles == 0);
}

TEST_CASE("skinning: dual quaternions keep a bent elbow's volume better than linear blending") {
    const MeshData tube = make_tube();
    std::vector<Vertex3D> lbs, dqs;
    for (const float deg : {60.0f, 90.0f, 135.0f}) {
        const Mat4 pal[2] = {Mat4::identity(), joint1(Quat::axis_angle({0, 0, 1}, math::radians(deg)))};
        skin_vertices(tube, pal, SkinningMethod::Linear, lbs);
        skin_vertices(tube, pal, SkinningMethod::DualQuaternion, dqs);
        const std::uint32_t joints[2] = {0, 1};
        // The blended region only: weights of both joints between 0.05 and 0.95.
        const float vl = measure_deformation(tube.vertices, lbs, tube.indices).volume_ratio;
        const float vd = measure_deformation(tube.vertices, dqs, tube.indices).volume_ratio;
        const float local_l = local_volume_ratio(tube, lbs, joints);
        MESSAGE("bend " << deg << ": volume LBS " << vl << " DQS " << vd << " (local LBS " << local_l << ")");
        CHECK(std::fabs(1.0f - vd) < std::fabs(1.0f - vl));
        CHECK(vl < 1.0f);
    }
}

TEST_CASE("skinning: skinned normals follow the joint rotation and rest poses are exact") {
    const MeshData tube = make_tube();
    std::vector<Vertex3D> out;
    const Mat4 rest[2] = {};
    for (const auto method : {SkinningMethod::Linear, SkinningMethod::DualQuaternion}) {
        skin_vertices(tube, rest, method, out);
        float err = 0;
        for (std::size_t i = 0; i < out.size(); ++i) err = std::max(err, math::length(out[i].position - tube.vertices[i].position));
        CHECK(err < 1e-5f);
    }
    // Rotating everything by one joint rotates normals with it.
    const Quat q = Quat::axis_angle({0, 1, 0}, 1.0f);
    const Mat4 both[2] = {Mat4::rotation(q), Mat4::rotation(q)};
    skin_vertices(tube, both, SkinningMethod::DualQuaternion, out);
    for (std::size_t i = 0; i < out.size(); i += 37) {
        const Vec3 expect = q.rotate(tube.vertices[i].normal);
        CHECK(math::length(out[i].normal - expect) < 1e-4f);
    }
}

TEST_CASE("skinning: weight analysis, normalisation and influence limits") {
    MeshData m = make_tube(2.0f, 0.2f, 5, 8);
    // Break a few vertices on purpose.
    m.skin[0].weights[0] = 0.5f;
    m.skin[0].weights[1] = 0.2f; // sums 0.7
    m.skin[1] = SkinVertex{};
    m.skin[1].weights[0] = 0.0f; // no weight
    m.skin[2].joints[1] = 9;     // out of range
    m.skin[2].weights[0] = 0.5f;
    m.skin[2].weights[1] = 0.5f;
    m.skin[3].joints[0] = 0;
    m.skin[3].joints[1] = 1;
    m.skin[3].joints[2] = 2;
    m.skin[3].joints[3] = 3;
    m.skin[3].weights[0] = 0.6f;
    m.skin[3].weights[1] = 0.3f;
    m.skin[3].weights[2] = 0.098f;
    m.skin[3].weights[3] = 0.002f; // tiny
    const SkinWeightReport r = analyze_skin_weights(m, 4);
    CHECK(r.vertices == m.vertices.size());
    CHECK(r.unnormalized == 2); // vertex 0 (0.7) and vertex 1 (0)
    CHECK(r.zero_weight == 1);
    CHECK(r.out_of_range == 1);
    CHECK(r.tiny_influences == 1);
    CHECK(r.influence_histogram[4] == 1);
    CHECK(r.joint_vertices[2] == 1);
    CHECK(r.joint_vertices[3] == 1);
    CHECK(r.unweighted_joints() == 0);
    CHECK(r.max_weight_error == doctest::Approx(1.0f));

    CHECK(normalize_skin_weights(m) == 2);
    CHECK(m.skin[0].weights[0] == doctest::Approx(0.5f / 0.7f));
    CHECK(m.skin[1].weights[0] == doctest::Approx(1.0f));
    const SkinWeightReport n = analyze_skin_weights(m, 4);
    CHECK(n.unnormalized == 0);
    CHECK(n.zero_weight == 0);

    CHECK(limit_skin_influences(m, 2) >= 1);
    const SkinVertex& v3 = m.skin[3];
    CHECK(v3.weights[2] == 0.0f);
    CHECK(v3.weights[3] == 0.0f);
    CHECK(v3.weights[0] + v3.weights[1] == doctest::Approx(1.0f));
    CHECK(v3.weights[0] == doctest::Approx(0.6f / 0.9f));

    const auto w1 = joint_weights(m, 1);
    CHECK(w1.size() == m.vertices.size());
    const auto heat = weight_heatmap_colors(m, 1);
    const auto dom = weight_heatmap_colors(m, kDominantJoint);
    CHECK(heat.size() == m.vertices.size());
    CHECK(dom.size() == m.vertices.size());
    // Fully weighted vertices read red, unweighted ones dark blue.
    const std::size_t last_ring = m.vertices.size() - 3; // a vertex of the far end, weight 1 on joint 1
    CHECK((heat[last_ring] & 0xff) == 255);
    CHECK(((heat[8] >> 16) & 0xff) < 64); // a near-end vertex (weight 0): no red, dim blue
}
