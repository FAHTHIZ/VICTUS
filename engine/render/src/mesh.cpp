// SPDX-License-Identifier: Apache-2.0
#include "victus/render/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace victus::render {
using namespace math;

namespace {
std::uint32_t to_byte(float v) noexcept {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
} // namespace

std::uint32_t pack_vertex_color(const Color& c) noexcept {
    return to_byte(linear_to_srgb(c.r)) | (to_byte(linear_to_srgb(c.g)) << 8) | (to_byte(linear_to_srgb(c.b)) << 16) |
           (to_byte(c.a) << 24);
}

Color unpack_vertex_color(std::uint32_t p) noexcept {
    auto ch = [&](int shift) { return static_cast<float>((p >> shift) & 0xFFu) / 255.0f; };
    return {srgb_to_linear(ch(0)), srgb_to_linear(ch(8)), srgb_to_linear(ch(16)), ch(24)};
}

Aabb MeshData::bounds() const noexcept {
    Aabb box;
    for (const auto& v : vertices) box.expand(v.position);
    return box;
}

SkinVertex make_skin_vertex(const std::uint32_t* joints, const float* weights, std::size_t count) noexcept {
    SkinVertex out;
    out.weights[0] = 0.0f;
    // Insertion into a 4-slot list sorted by descending weight.
    std::uint32_t best_j[4]{0, 0, 0, 0};
    float best_w[4]{0, 0, 0, 0};
    for (std::size_t i = 0; i < count; ++i) {
        const float w = weights[i];
        if (!(w > 0.0f)) continue;
        std::uint32_t j = joints[i];
        float wv = w;
        for (int k = 0; k < 4; ++k) {
            if (wv > best_w[k]) {
                std::swap(wv, best_w[k]);
                std::swap(j, best_j[k]);
            }
        }
    }
    const float sum = best_w[0] + best_w[1] + best_w[2] + best_w[3];
    if (!(sum > 0.0f)) {
        out.weights[0] = 1.0f;
        return out;
    }
    for (int k = 0; k < 4; ++k) {
        out.joints[k] = static_cast<std::uint16_t>(std::min<std::uint32_t>(best_j[k], 0xffffu));
        out.weights[k] = best_w[k] / sum;
    }
    return out;
}

void MeshData::append(const MeshData& other, const Mat4& transform) {
    const auto base = static_cast<std::uint32_t>(vertices.size());
    const bool keep_skin = other.skinned() && (vertices.empty() || skinned());
    if (keep_skin) skin.insert(skin.end(), other.skin.begin(), other.skin.end());
    else skin.clear();
    const Mat4 normal_m = transpose(inverse(transform));
    for (Vertex3D v : other.vertices) {
        v.position = transform.transform_point(v.position);
        v.normal = normalize(normal_m.transform_vector(v.normal));
        Vec3 t = normalize(transform.transform_vector(v.tangent.xyz()));
        v.tangent = Vec4(t, v.tangent.w);
        vertices.push_back(v);
    }
    for (auto i : other.indices) indices.push_back(base + i);
}

void compute_normals(MeshData& mesh) {
    for (auto& v : mesh.vertices) v.normal = {};
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        Vec3 n = cross(b.position - a.position, c.position - a.position); // area weighted
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto& v : mesh.vertices) v.normal = length(v.normal) > 0 ? normalize(v.normal) : Vec3{0, 1, 0};
}

void compute_tangents(MeshData& mesh) {
    std::vector<Vec3> tan(mesh.vertices.size()), bitan(mesh.vertices.size());
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        const auto& a = mesh.vertices[ia];
        const auto& b = mesh.vertices[ib];
        const auto& c = mesh.vertices[ic];
        Vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        Vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        Vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        Vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (auto idx : {ia, ib, ic}) {
            tan[idx] += t;
            bitan[idx] += bt;
        }
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& v = mesh.vertices[i];
        Vec3 t = tan[i] - v.normal * dot(v.normal, tan[i]);
        if (length(t) < 1e-8f) {
            // Degenerate UVs: pick any vector orthogonal to the normal.
            Vec3 axis = std::fabs(v.normal.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            t = cross(axis, v.normal);
        }
        t = normalize(t);
        // glTF convention: bitangent = cross(N, T) * w points towards decreasing v (texture "up").
        float w = dot(cross(v.normal, t), bitan[i]) > 0.0f ? -1.0f : 1.0f;
        v.tangent = Vec4(t, w);
    }
}

MeshData make_cube(Vec3 h) {
    MeshData m;
    struct Face {
        Vec3 n, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const auto& f : faces) {
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        const Vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto c : corners) {
            Vertex3D v;
            Vec3 p = f.n + f.u * c.x + f.v * c.y;
            v.position = p * h;
            v.normal = f.n;
            v.uv = {c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f};
            v.tangent = Vec4(f.u, 1.0f);
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    return m;
}

MeshData make_uv_sphere(float radius, std::uint32_t segments, std::uint32_t rings) {
    MeshData m;
    segments = segments < 3 ? 3 : segments;
    rings = rings < 2 ? 2 : rings;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        float v = static_cast<float>(r) / static_cast<float>(rings);
        float phi = v * kPi;
        for (std::uint32_t s = 0; s <= segments; ++s) {
            float u = static_cast<float>(s) / static_cast<float>(segments);
            float theta = u * kTau;
            Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)};
            Vertex3D vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = {u, v};
            vert.tangent = Vec4(normalize(Vec3{-std::sin(theta), 0, -std::cos(theta)}), 1.0f);
            m.vertices.push_back(vert);
        }
    }
    const std::uint32_t stride = segments + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            std::uint32_t a = r * stride + s, b = a + stride;
            if (r != 0) m.indices.insert(m.indices.end(), {a, b, a + 1});
            if (r != rings - 1) m.indices.insert(m.indices.end(), {a + 1, b, b + 1});
        }
    }
    return m;
}

MeshData make_plane(Vec2 size, std::uint32_t subdivisions, float uv_scale) {
    MeshData m;
    subdivisions = subdivisions == 0 ? 1 : subdivisions;
    const std::uint32_t n = subdivisions + 1;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            float fx = static_cast<float>(x) / static_cast<float>(subdivisions);
            float fz = static_cast<float>(z) / static_cast<float>(subdivisions);
            Vertex3D v;
            v.position = {(fx - 0.5f) * size.x, 0.0f, (fz - 0.5f) * size.y};
            v.normal = {0, 1, 0};
            v.uv = {fx * uv_scale, fz * uv_scale};
            v.tangent = {1, 0, 0, 1};
            m.vertices.push_back(v);
        }
    }
    for (std::uint32_t z = 0; z < subdivisions; ++z) {
        for (std::uint32_t x = 0; x < subdivisions; ++x) {
            std::uint32_t a = z * n + x, b = a + n;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return m;
}

MeshData make_cylinder(float radius, float height, std::uint32_t segments) {
    MeshData m;
    segments = segments < 3 ? 3 : segments;
    const float hh = height * 0.5f;
    for (std::uint32_t s = 0; s <= segments; ++s) {
        float u = static_cast<float>(s) / static_cast<float>(segments);
        float t = u * kTau;
        Vec3 n{std::cos(t), 0, -std::sin(t)};
        for (float y : {-hh, hh}) {
            Vertex3D v;
            v.position = {n.x * radius, y, n.z * radius};
            v.normal = n;
            v.uv = {u, y > 0 ? 0.0f : 1.0f};
            v.tangent = Vec4(Vec3{-std::sin(t), 0, -std::cos(t)}, 1.0f);
            m.vertices.push_back(v);
        }
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        std::uint32_t a = s * 2;
        m.indices.insert(m.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
    for (float sign : {1.0f, -1.0f}) {
        const auto center = static_cast<std::uint32_t>(m.vertices.size());
        Vertex3D c;
        c.position = {0, hh * sign, 0};
        c.normal = {0, sign, 0};
        c.uv = {0.5f, 0.5f};
        m.vertices.push_back(c);
        for (std::uint32_t s = 0; s <= segments; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segments) * kTau;
            Vertex3D v;
            v.position = {std::cos(t) * radius, hh * sign, -std::sin(t) * radius};
            v.normal = {0, sign, 0};
            v.uv = {std::cos(t) * 0.5f + 0.5f, std::sin(t) * 0.5f + 0.5f};
            m.vertices.push_back(v);
        }
        for (std::uint32_t s = 0; s < segments; ++s) {
            std::uint32_t a = center + 1 + s;
            if (sign > 0) m.indices.insert(m.indices.end(), {center, a, a + 1});
            else m.indices.insert(m.indices.end(), {center, a + 1, a});
        }
    }
    compute_tangents(m);
    return m;
}

} // namespace victus::render
