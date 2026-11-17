// SPDX-License-Identifier: Apache-2.0
// Clustered forward lighting (Olsson et al. 2012): the view frustum is split into a grid of
// screen tiles x logarithmic depth slices, and every local light is assigned on the CPU to the
// clusters its bounding sphere touches. Fragment shaders look up their cluster and loop over
// only those lights, so hundreds of lights cost about as much as the few that reach a pixel.
#pragma once
#include "victus/core/math.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace victus::render {

// One local light as uploaded to the GPU (std430, 64 bytes).
struct GpuLight {
    math::Vec4 position_range;      // xyz world position, w range
    math::Vec4 color_intensity;     // rgb linear colour, w intensity
    math::Vec4 direction_cos_outer; // xyz spot direction (light travels along it), w cos(outer angle)
    math::Vec4 spot;                // x cos(inner angle), y 1 = spot / 0 = point
};
static_assert(sizeof(GpuLight) == 64);

struct ClusterGridSettings {
    std::uint32_t tiles_x{16}, tiles_y{9}, slices{24};
    std::uint32_t max_lights_per_cluster{128};
};

// Bounding sphere (centre, radius) of a light's area of influence; spot cones get a tight
// sphere (Wronski 2017) instead of a sphere of radius `range` around the apex.
[[nodiscard]] math::Vec4 light_bounding_sphere(const GpuLight& light) noexcept;

struct LightClusters {
    ClusterGridSettings grid;
    float z_near{0.1f}, z_far{100.0f};
    // data[2 * c] = first index into the list, data[2 * c + 1] = count, for cluster
    // c = (slice * tiles_y + tile_y) * tiles_x + tile_x; then the flattened light index list.
    std::vector<std::uint32_t> data;
    std::uint32_t index_count{0}, max_per_cluster{0}, overflowed_clusters{0};

    [[nodiscard]] std::uint32_t cluster_count() const noexcept { return grid.tiles_x * grid.tiles_y * grid.slices; }
    // Cluster of a view-space position (view depth > 0 in front of the camera) whose projection
    // lands at normalized screen position `uv` (0..1, y down).
    [[nodiscard]] std::uint32_t cluster_of(math::Vec2 uv, float view_depth) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> lights_in(std::uint32_t cluster) const noexcept;
};

// Assigns `lights` to clusters for a perspective camera (`view`, symmetric `proj`).
[[nodiscard]] LightClusters build_light_clusters(std::span<const GpuLight> lights, const math::Mat4& view,
                                                 const math::Mat4& proj, float z_near, float z_far,
                                                 const ClusterGridSettings& grid = {});

} // namespace victus::render
