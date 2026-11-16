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
};
static_assert(sizeof(Vertex3D) == 48);

struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<std::uint32_t> indices; // triangle list, counter-clockwise front faces
    [[nodiscard]] math::Aabb bounds() const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    void append(const MeshData& other, const math::Mat4& transform);
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
