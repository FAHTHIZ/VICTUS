// SPDX-License-Identifier: Apache-2.0
// CPU-side mesh data, procedural primitives and tangent generation.
#pragma once
#include "victus/core/math.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace victus::render {

struct Vertex3D {
    math::Vec3 position;
    math::Vec3 normal{0, 1, 0};
    math::Vec2 uv;
    math::Vec4 tangent{1, 0, 0, 1}; // xyz tangent, w bitangent sign
    // Per-vertex colour, RGBA8 with red in the lowest byte. RGB is sRGB-encoded (decoded to linear
    // in the vertex shader, which keeps precision in dark tones); alpha is linear. Multiplies the
    // material base colour. White (the default) leaves materials unchanged.
    std::uint32_t color{0xFFFFFFFFu};
};
static_assert(sizeof(Vertex3D) == 52);

// Packs a linear colour into the Vertex3D::color encoding (sRGB RGB, linear alpha).
[[nodiscard]] std::uint32_t pack_vertex_color(const math::Color& linear) noexcept;
// Packs already sRGB-encoded 8-bit channels (e.g. authored palette bytes).
[[nodiscard]] constexpr std::uint32_t pack_vertex_color_srgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                                              std::uint8_t a = 255) noexcept {
    return std::uint32_t{r} | (std::uint32_t{g} << 8) | (std::uint32_t{b} << 16) | (std::uint32_t{a} << 24);
}
// Inverse of pack_vertex_color.
[[nodiscard]] math::Color unpack_vertex_color(std::uint32_t packed) noexcept;

// Skinning influences for one vertex (vertex buffer slot 2 of skinned meshes): up to four joint
// indices into the mesh's skin palette with weights summing to 1.
struct SkinVertex {
    std::uint16_t joints[4]{0, 0, 0, 0};
    float weights[4]{1, 0, 0, 0};
};
static_assert(sizeof(SkinVertex) == 24);

// Keeps the four largest influences of (joint, weight) pairs and normalizes them; vertices with
// no positive weight bind fully to joint 0.
[[nodiscard]] SkinVertex make_skin_vertex(const std::uint32_t* joints, const float* weights, std::size_t count) noexcept;

// A coarser level of detail of a mesh: its own triangle list over the mesh's vertices, and the
// geometric error of the simplification in the mesh's own units (how far, at most, the surface
// moved), which the renderer projects to pixels to pick a level.
struct MeshLod {
    std::vector<std::uint32_t> indices;
    float error{0.0f};
};

// Most levels a mesh carries (LOD 0, the full mesh, plus kMaxMeshLods - 1 coarser ones).
inline constexpr std::uint32_t kMaxMeshLods = 8;

// A morph target (blend shape, glTF 2.0 "targets"): sparse per-vertex offsets added to the base
// mesh scaled by the target's weight. `vertices` are ascending vertex indices; `positions` and
// (when present) `normals` hold one delta per entry.
struct MorphTarget {
    std::string name;
    std::vector<std::uint32_t> vertices;
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals; // empty, or one per entry of `vertices`
};

// Most morph targets per mesh (ARKit faces use 52; glTF has no limit).
inline constexpr std::uint32_t kMaxMorphTargets = 256;

struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<std::uint32_t> indices; // triangle list, counter-clockwise front faces (LOD 0)
    std::vector<SkinVertex> skin;       // empty, or one entry per vertex for skinned meshes
    // LOD 1..n, coarsest last, sharing `vertices` (and `skin`); errors never decrease. Collision,
    // navigation and anything else that reads `indices` always sees the full mesh.
    std::vector<MeshLod> lods;
    // Morph targets over `vertices` and their default weights (one per target; glTF mesh
    // "weights", zeros when absent).
    std::vector<MorphTarget> morphs;
    std::vector<float> morph_weights;
    [[nodiscard]] math::Aabb bounds() const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    // Appends transformed geometry. Skin data is kept only when both meshes are skinned (or this
    // one is empty); otherwise it is dropped so vertex and skin counts always match. LODs and
    // morph targets are dropped (they no longer describe the merged mesh).
    void append(const MeshData& other, const math::Mat4& transform);
    [[nodiscard]] bool skinned() const noexcept { return !skin.empty() && skin.size() == vertices.size(); }
};

// Validates morph targets: ascending in-range vertex indices, matching delta counts, finite
// values, at most kMaxMorphTargets, and one default weight per target (or none). Empty string
// when valid, otherwise what is wrong.
[[nodiscard]] std::string validate_morphs(const MeshData& mesh);

// Writes `mesh.vertices` deformed by its morph targets at `weights` (one per target; missing
// entries count as 0) into `out`: positions and normals get the weighted deltas, normals are
// renormalized and tangents re-orthogonalized against them. Returns false (and copies the base
// vertices) when every weight is 0. Weights are not clamped (glTF allows negative and >1).
bool apply_morphs(const MeshData& mesh, std::span<const float> weights, std::vector<Vertex3D>& out);

// Recomputes per-vertex tangents from positions/uvs (Lengyel's method) with Gram-Schmidt.
void compute_tangents(MeshData& mesh);
// Smooth normals from triangle geometry (area weighted).
void compute_normals(MeshData& mesh);

// Reorders triangles for the GPU's post-transform vertex cache and against overdraw, then
// reorders vertices (and the skin stream) into first-use order for fetch locality. Geometry and
// every LOD keep the same triangles; only their order and the vertex numbering change.
void optimize_mesh(MeshData& mesh);

struct LodSettings {
    std::uint32_t max_lods{3};      // coarser levels to build (0 = none), at most kMaxMeshLods - 1
    float ratio{0.5f};              // triangle budget of each level relative to the previous one
    float max_error{0.05f};         // largest error allowed, relative to the mesh's extent
    std::uint32_t min_triangles{24}; // stop before a level would drop below this
    bool lock_borders{false};       // keep open boundary edges (e.g. tiles that must stay seamless)
};

// Builds `mesh.lods` with meshoptimizer's simplifier (positions, normals and UVs weighted), each
// level optimised for the vertex cache. Levels that would not remove at least 10% more triangles
// than the previous one are not kept. Returns the number of levels built.
std::uint32_t generate_lods(MeshData& mesh, const LodSettings& settings = {});

[[nodiscard]] MeshData make_cube(math::Vec3 half_extents = {0.5f, 0.5f, 0.5f});
[[nodiscard]] MeshData make_uv_sphere(float radius = 0.5f, std::uint32_t segments = 32, std::uint32_t rings = 16);
// Plane in XZ facing +Y, uv repeated `uv_scale` times.
[[nodiscard]] MeshData make_plane(math::Vec2 size = {10, 10}, std::uint32_t subdivisions = 1, float uv_scale = 1.0f);
[[nodiscard]] MeshData make_cylinder(float radius = 0.5f, float height = 1.0f, std::uint32_t segments = 32);

} // namespace victus::render
