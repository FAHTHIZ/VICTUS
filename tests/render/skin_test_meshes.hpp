// SPDX-License-Identifier: Apache-2.0
// Test-only skinned tube (ADR-0205): the elbow/forearm stress mesh the CPU and GPU skinning tests
// deform with linear and dual quaternion skinning.
#pragma once
#include "victus/render/mesh.hpp"

#include <cmath>
#include <numbers>
#include <vector>

namespace victus::test {

// A capped tube along +X from 0 to `length`, `rings` rings of `sides` vertices, two joints:
// joint 0 at x = 0, joint 1 at x = length / 2. Weights blend linearly over the middle `blend`
// fraction of the tube (the classic elbow/forearm stress test).
inline render::MeshData make_tube(float length = 2.0f, float radius = 0.2f, int rings = 41, int sides = 24, float blend = 0.4f) {
    render::MeshData m;
    const float half = length * 0.5f;
    for (int r = 0; r < rings; ++r) {
        const float x = length * static_cast<float>(r) / static_cast<float>(rings - 1);
        const float t = math::saturate((x - (half - blend * half)) / (2.0f * blend * half));
        for (int s = 0; s < sides; ++s) {
            const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(s) / static_cast<float>(sides);
            render::Vertex3D v;
            v.normal = {0, std::cos(a), std::sin(a)};
            v.position = {x, radius * v.normal.y, radius * v.normal.z};
            v.tangent = {1, 0, 0, 1};
            m.vertices.push_back(v);
            render::SkinVertex sv;
            sv.joints[0] = 0;
            sv.joints[1] = 1;
            sv.weights[0] = 1.0f - t;
            sv.weights[1] = t;
            m.skin.push_back(sv);
        }
    }
    for (int r = 0; r + 1 < rings; ++r)
        for (int s = 0; s < sides; ++s) {
            const auto i0 = static_cast<std::uint32_t>(r * sides + s);
            const auto i1 = static_cast<std::uint32_t>(r * sides + (s + 1) % sides);
            const auto i2 = i0 + static_cast<std::uint32_t>(sides), i3 = i1 + static_cast<std::uint32_t>(sides);
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2}); // outward-facing
        }
    // Caps (centre vertices bound like their ring) close the tube so volumes are meaningful.
    for (int cap = 0; cap < 2; ++cap) {
        render::Vertex3D c;
        c.position = {cap == 0 ? 0.0f : length, 0, 0};
        c.normal = {cap == 0 ? -1.0f : 1.0f, 0, 0};
        const auto ci = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back(c);
        render::SkinVertex sv;
        sv.joints[0] = cap == 0 ? 0 : 1;
        m.skin.push_back(sv);
        const int r = cap == 0 ? 0 : rings - 1;
        for (int s = 0; s < sides; ++s) {
            const auto a = static_cast<std::uint32_t>(r * sides + s), b = static_cast<std::uint32_t>(r * sides + (s + 1) % sides);
            if (cap == 0) m.indices.insert(m.indices.end(), {ci, b, a});
            else m.indices.insert(m.indices.end(), {ci, a, b});
        }
    }
    return m;
}

// Skin matrix of joint 1 rotated by `q` about its rest position (x = 1).
inline math::Mat4 joint1(math::Quat q, float length = 2.0f) {
    const float h = length * 0.5f;
    return math::Mat4::translation({h, 0, 0}) * math::Mat4::rotation(q) * math::Mat4::translation({-h, 0, 0});
}

// Mean distance of the middle ring from the (rotated half-way) axis: the radius that LBS loses
// under twist.
inline float mid_ring_radius(const std::vector<render::Vertex3D>& v, int rings = 41, int sides = 24) {
    const int r = rings / 2;
    math::Vec3 c{};
    for (int s = 0; s < sides; ++s) c = c + v[static_cast<std::size_t>(r * sides + s)].position;
    c = c / static_cast<float>(sides);
    float sum = 0;
    for (int s = 0; s < sides; ++s) sum += math::length(v[static_cast<std::size_t>(r * sides + s)].position - c);
    return sum / static_cast<float>(sides);
}

} // namespace victus::test
