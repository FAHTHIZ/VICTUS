// SPDX-License-Identifier: Apache-2.0
// Backend-independent cloth geometry (ADR-0205): the particle grid and the render mesh written
// from simulated particles. The Jolt soft body lives in jolt_world.cpp.
#include "internal.hpp"

#include <algorithm>
#include <cmath>

namespace victus::physics {

namespace detail {

bool cloth_grid(const ClothComponent& c, ClothGrid& out) {
    out = {};
    if (!(c.size.x > 1e-4f) || !(c.size.y > 1e-4f) || !std::isfinite(c.size.x) || !std::isfinite(c.size.y)) return false;
    const std::uint32_t sx = std::clamp<std::uint32_t>(c.segments_x, 1u, 128u);
    const std::uint32_t sy = std::clamp<std::uint32_t>(c.segments_y, 1u, 128u);
    out.columns = sx + 1;
    out.rows = sy + 1;
    const std::size_t n = static_cast<std::size_t>(out.columns) * out.rows;
    out.rest.resize(n);
    out.uvs.resize(n);
    out.pinned.assign(n, 0);
    const float hw = c.size.x * 0.5f, hh = c.size.y * 0.5f;
    for (std::uint32_t j = 0; j < out.rows; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(sy);
        for (std::uint32_t i = 0; i < out.columns; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(sx);
            const std::size_t k = static_cast<std::size_t>(j) * out.columns + i;
            out.rest[k] = {-hw + u * c.size.x, hh - v * c.size.y, 0.0f};
            out.uvs[k] = {u, v};
            bool pin = false;
            const bool top = j == 0, bottom = j == sy, left = i == 0, right = i == sx;
            switch (c.pin) {
            case ClothComponent::Pin::None: break;
            case ClothComponent::Pin::TopEdge: pin = top; break;
            case ClothComponent::Pin::TopCorners: pin = top && (left || right); break;
            case ClothComponent::Pin::LeftEdge: pin = left; break;
            case ClothComponent::Pin::Corners: pin = (top || bottom) && (left || right); break;
            }
            out.pinned[k] = pin ? 1 : 0;
            out.pinned_count += pin ? 1u : 0u;
        }
    }
    out.indices.reserve(static_cast<std::size_t>(sx) * sy * 6);
    for (std::uint32_t j = 0; j < sy; ++j)
        for (std::uint32_t i = 0; i < sx; ++i) {
            const std::uint32_t a = j * out.columns + i, b = a + 1, d = a + out.columns, e = d + 1;
            if (((i + j) & 1u) == 0) {
                out.indices.insert(out.indices.end(), {a, d, e, a, e, b});
            } else {
                out.indices.insert(out.indices.end(), {a, d, b, b, d, e});
            }
        }
    return true;
}

std::shared_ptr<const scene::ProceduralMesh> cloth_mesh(const ClothGrid& grid, const std::vector<math::Vec3>& positions,
                                                        bool double_sided, std::vector<math::Vec3>& normals) {
    const std::size_t n = grid.rest.size();
    if (n == 0 || positions.size() != n) return nullptr;
    normals.assign(n, math::Vec3{});
    for (std::size_t t = 0; t + 2 < grid.indices.size(); t += 3) {
        const std::uint32_t a = grid.indices[t], b = grid.indices[t + 1], c = grid.indices[t + 2];
        const math::Vec3 fn = math::cross(positions[b] - positions[a], positions[c] - positions[a]); // area weighted
        normals[a] += fn;
        normals[b] += fn;
        normals[c] += fn;
    }
    for (math::Vec3& v : normals) {
        const float l = math::length(v);
        v = l > 1e-12f ? v / l : math::Vec3{0, 0, 1};
    }
    scene::ProceduralMesh m;
    const std::size_t total = double_sided ? n * 2 : n;
    m.positions.reserve(total);
    m.normals.reserve(total);
    m.uvs.reserve(total);
    m.positions.insert(m.positions.end(), positions.begin(), positions.end());
    m.normals.insert(m.normals.end(), normals.begin(), normals.end());
    m.uvs.insert(m.uvs.end(), grid.uvs.begin(), grid.uvs.end());
    m.indices.reserve(double_sided ? grid.indices.size() * 2 : grid.indices.size());
    m.indices.insert(m.indices.end(), grid.indices.begin(), grid.indices.end());
    if (double_sided) {
        m.positions.insert(m.positions.end(), positions.begin(), positions.end());
        for (const math::Vec3& v : normals) m.normals.push_back(-v);
        m.uvs.insert(m.uvs.end(), grid.uvs.begin(), grid.uvs.end());
        const auto off = static_cast<std::uint32_t>(n);
        for (std::size_t t = 0; t + 2 < grid.indices.size(); t += 3) {
            m.indices.push_back(grid.indices[t] + off);
            m.indices.push_back(grid.indices[t + 2] + off);
            m.indices.push_back(grid.indices[t + 1] + off);
        }
    }
    auto published = scene::publish_mesh(std::move(m));
    return published ? *published : nullptr;
}

bool cloth_same_shape(const ClothComponent& a, const ClothComponent& b) {
    return a.size == b.size && a.segments_x == b.segments_x && a.segments_y == b.segments_y && a.pin == b.pin &&
           a.mass == b.mass && a.stretch_compliance == b.stretch_compliance && a.shear_compliance == b.shear_compliance &&
           a.bend_compliance == b.bend_compliance && a.thickness == b.thickness && a.layer == b.layer && a.mask == b.mask;
}

} // namespace detail

std::shared_ptr<const scene::ProceduralMesh> cloth_rest_mesh(const ClothComponent& cloth) {
    detail::ClothGrid grid;
    if (!detail::cloth_grid(cloth, grid)) return nullptr;
    std::vector<math::Vec3> normals;
    return detail::cloth_mesh(grid, grid.rest, cloth.double_sided, normals);
}

} // namespace victus::physics
