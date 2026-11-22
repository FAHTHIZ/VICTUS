// SPDX-License-Identifier: Apache-2.0
// Skinning maths shared by the GPU path, the CPU fallback and the deformation tooling (ADR-0205).
//
// * Linear blend skinning (LBS): the weighted sum of joint matrices, what every skinned shader did
//   before ADR-0205. Cheap; candy-wraps under twist and loses volume at bent joints.
// * Dual quaternion skinning (DQS, Kavan et al. 2007): joint transforms are blended as unit dual
//   quaternions, which keeps twisted forearms and bent elbows from collapsing. Joint scale is
//   blended separately and applied before the rotation (uniform or axis-aligned scale; sheared
//   joint matrices fall back to their nearest rigid transform plus scale).
//
// The GPU palette keeps one Mat4 per joint. A DQS palette entry stores the joint as
//   column 0 = real quaternion (x, y, z, w), column 1 = dual quaternion, column 2 = scale (xyz),
//   column 3 = (0, 0, 0, -1)
// An affine LBS matrix always has column 3 w = 1, so the skinned vertex shaders pick the method
// from the first influence's marker: one pipeline serves both methods and a draw's palette decides.
//
// The CPU functions are the reference the GPU shaders are tested against, the fallback for
// devices or passes that cannot skin on the GPU, and the measurement tools of the deformation
// diagnostics (Studio's Skeleton panel, `victus skeleton --stress`).
#pragma once
#include "victus/core/math.hpp"
#include "victus/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace victus::render {

enum class SkinningMethod : std::uint8_t { Linear, DualQuaternion };
[[nodiscard]] std::string_view skinning_method_name(SkinningMethod m) noexcept;

// Unit dual quaternion: rotation `real`, translation encoded in `dual` (dual = 0.5 * t * real).
struct DualQuat {
    math::Quat real{};
    math::Quat dual{0, 0, 0, 0};
};
// Rigid part of an affine matrix (scale removed) as a dual quaternion; `scale` receives the
// per-axis scale. Sheared matrices are orthonormalised (Gram-Schmidt) first.
[[nodiscard]] DualQuat to_dual_quat(const math::Mat4& m, math::Vec3* scale = nullptr) noexcept;
[[nodiscard]] math::Mat4 to_matrix(const DualQuat& dq, math::Vec3 scale = {1, 1, 1}) noexcept;

// GPU palette encoding of one skin matrix for DualQuaternion draws (see the file comment).
[[nodiscard]] math::Mat4 encode_dq_joint(const math::Mat4& skin) noexcept;
[[nodiscard]] inline bool is_dq_joint(const math::Mat4& m) noexcept { return m.c[3].w < 0.0f; }
// Rewrites a whole LBS palette in place.
void encode_dq_palette(std::span<math::Mat4> palette) noexcept;

// The skin matrix of one vertex: LBS or DQS of up to four influences. `palette` holds plain skin
// matrices (not the DQ encoding). Joints outside the palette contribute the identity.
[[nodiscard]] math::Mat4 blend_skin_matrix(const SkinVertex& v, std::span<const math::Mat4> palette,
                                           SkinningMethod method) noexcept;
// CPU skinning of every vertex of `mesh` (positions, normals, tangents; colours and uvs copied).
// Non-skinned meshes are copied unchanged. `out` is resized to the vertex count.
void skin_vertices(const MeshData& mesh, std::span<const Vertex3D> base, std::span<const math::Mat4> palette,
                   SkinningMethod method, std::vector<Vertex3D>& out);
inline void skin_vertices(const MeshData& mesh, std::span<const math::Mat4> palette, SkinningMethod method,
                          std::vector<Vertex3D>& out) {
    skin_vertices(mesh, mesh.vertices, palette, method, out);
}

// ---- weights -----------------------------------------------------------------------------------

// Per-vertex influence statistics of a skinned mesh (Studio's weight inspection, validation).
struct SkinWeightReport {
    std::size_t vertices{0};
    std::size_t unnormalized{0};      // weights summing further than 1e-3 from 1
    std::size_t zero_weight{0};       // no positive weight (the shader binds them to joint 0)
    std::size_t out_of_range{0};      // weighted joint index >= joint_count
    std::size_t tiny_influences{0};   // influences below kTinyWeight (candidates for pruning)
    std::array<std::size_t, 5> influence_histogram{}; // vertices with 0..4 non-zero influences
    float max_weight_error{0};        // largest |sum - 1|
    std::vector<std::uint32_t> joint_vertices; // per joint: vertices it influences (weight > 0)
    std::vector<float> joint_weight;           // per joint: summed weight
    [[nodiscard]] std::size_t unweighted_joints() const noexcept; // joints influencing no vertex
};
inline constexpr float kTinyWeight = 1.0f / 255.0f; // below one step of an 8-bit weight
[[nodiscard]] SkinWeightReport analyze_skin_weights(const MeshData& mesh, std::size_t joint_count);
// Renormalises every vertex's weights to sum 1 (zero-weight vertices bind fully to joint 0).
// Returns the number of vertices changed.
std::size_t normalize_skin_weights(MeshData& mesh) noexcept;
// Drops influences below `min_weight` and beyond `max_influences` (1..4), keeping the strongest,
// and renormalises. Returns the number of vertices changed.
std::size_t limit_skin_influences(MeshData& mesh, std::uint32_t max_influences, float min_weight = kTinyWeight) noexcept;
// Weight of `joint` at every vertex (0 for vertices it does not influence).
[[nodiscard]] std::vector<float> joint_weights(const MeshData& mesh, std::uint32_t joint);
// Heat-map vertex colours (packed like Vertex3D::color): blue (0) .. green .. red (1) of `joint`'s
// weight, or (joint = kDominantJoint) a distinct hue per vertex's strongest joint.
inline constexpr std::uint32_t kDominantJoint = 0xffffffffu;
[[nodiscard]] std::vector<std::uint32_t> weight_heatmap_colors(const MeshData& mesh, std::uint32_t joint);

// ---- deformation metrics ----------------------------------------------------------------------

// How a deformed mesh differs from its rest shape. Computed on the triangle list (LOD 0).
struct DeformationMetrics {
    float rest_volume{0}, deformed_volume{0}; // signed enclosed volume (closed meshes; open ones approximate)
    float volume_ratio{1};                    // deformed / rest
    float min_area_ratio{1}, max_area_ratio{1}; // per-triangle area change extremes
    std::size_t collapsed_triangles{0};       // area below 10% of rest (candy-wrapping, pinching)
    std::size_t flipped_triangles{0};         // face normal opposite the mean of its skinned vertex normals (folds)
    float max_displacement{0};                // largest vertex movement (metres)
};
// `rest` and `deformed` are vertex arrays of the same mesh (same count); `indices` its triangles.
[[nodiscard]] DeformationMetrics measure_deformation(std::span<const Vertex3D> rest, std::span<const Vertex3D> deformed,
                                                     std::span<const std::uint32_t> indices);
// Signed volume enclosed by a triangle list (positive for outward-facing counter-clockwise triangles).
[[nodiscard]] float signed_volume(std::span<const Vertex3D> vertices, std::span<const std::uint32_t> indices) noexcept;
// Volume of the vertices influenced by `joint` (weight >= min_weight) as a fraction of rest:
// local volume change around one joint, the quantity elbow/knee/shoulder diagnostics report.
// Uses the triangles whose three vertices all pass the weight test.
[[nodiscard]] float local_volume_ratio(const MeshData& mesh, std::span<const Vertex3D> deformed,
                                       std::span<const std::uint32_t> joints, float min_weight = 0.05f);

} // namespace victus::render
