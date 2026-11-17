// SPDX-License-Identifier: Apache-2.0
// CPU-side mesh data, procedural primitives and tangent generation.
#pragma once
#include "victus/core/math.hpp"

#include <cstdint>
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

struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<std::uint32_t> indices; // triangle list, counter-clockwise front faces
    std::vector<SkinVertex> skin;       // empty, or one entry per vertex for skinned meshes
    [[nodiscard]] math::Aabb bounds() const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    // Appends transformed geometry. Skin data is kept only when both meshes are skinned (or this
    // one is empty); otherwise it is dropped so vertex and skin counts always match.
    void append(const MeshData& other, const math::Mat4& transform);
    [[nodiscard]] bool skinned() const noexcept { return !skin.empty() && skin.size() == vertices.size(); }
};

// Recomputes per-vertex tangents from positions/uvs (Lengyel's method) with Gram-Schmidt.
void compute_tangents(MeshData& mesh);
// Smooth normals from triangle geometry (area weighted).
void compute_normals(MeshData& mesh);

[[nodiscard]] MeshData make_cube(math::Vec3 half_extents = {0.5f, 0.5f, 0.5f});
[[nodiscard]] MeshData make_uv_sphere(float radius = 0.5f, std::uint32_t segments = 32, std::uint32_t rings = 16);
// Plane in XZ facing +Y, uv repeated `uv_scale` times.
[[nodiscard]] MeshData make_plane(math::Vec2 size = {10, 10}, std::uint32_t subdivisions = 1, float uv_scale = 1.0f);
[[nodiscard]] MeshData make_cylinder(float radius = 0.5f, float height = 1.0f, std::uint32_t segments = 32);

} // namespace victus::render
