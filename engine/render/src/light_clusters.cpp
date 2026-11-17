// SPDX-License-Identifier: Apache-2.0
#include "victus/render/light_clusters.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {
using math::Vec2;
using math::Vec3;
using math::Vec4;

Vec4 light_bounding_sphere(const GpuLight& l) noexcept {
    const Vec3 p{l.position_range.x, l.position_range.y, l.position_range.z};
    const float range = l.position_range.w;
    if (l.spot.y < 0.5f) return {p.x, p.y, p.z, range};
    const Vec3 d = math::normalize(Vec3{l.direction_cos_outer.x, l.direction_cos_outer.y, l.direction_cos_outer.z});
    const float cos_a = std::clamp(l.direction_cos_outer.w, -1.0f, 1.0f);
    if (cos_a <= 0.70710678f) { // wide cone: the sphere through the cap's rim
        const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
        const Vec3 c = p + d * (range * std::max(cos_a, 0.0f));
        return {c.x, c.y, c.z, cos_a < 0.0f ? range : range * sin_a};
    }
    const float r = range / (2.0f * cos_a); // narrow cone: sphere through apex and rim
    const Vec3 c = p + d * r;
    return {c.x, c.y, c.z, r};
}

std::uint32_t LightClusters::cluster_of(Vec2 uv, float depth) const noexcept {
    const auto clampi = [](float v, std::uint32_t n) {
        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::floor(v)), 0, static_cast<int>(n) - 1));
    };
    const std::uint32_t tx = clampi(uv.x * static_cast<float>(grid.tiles_x), grid.tiles_x);
    const std::uint32_t ty = clampi(uv.y * static_cast<float>(grid.tiles_y), grid.tiles_y);
    const float s = std::log(std::max(depth, z_near) / z_near) / std::log(z_far / z_near) * static_cast<float>(grid.slices);
    const std::uint32_t tz = clampi(s, grid.slices);
    return (tz * grid.tiles_y + ty) * grid.tiles_x + tx;
}

std::span<const std::uint32_t> LightClusters::lights_in(std::uint32_t c) const noexcept {
    if (2 * c + 1 >= data.size()) return {};
    return {data.data() + data[2 * c], data[2 * c + 1]};
}

LightClusters build_light_clusters(std::span<const GpuLight> lights, const math::Mat4& view, const math::Mat4& proj,
                                   float z_near, float z_far, const ClusterGridSettings& grid_in) {
    LightClusters out;
    out.grid = grid_in;
    out.grid.tiles_x = std::clamp(out.grid.tiles_x, 1u, 64u);
    out.grid.tiles_y = std::clamp(out.grid.tiles_y, 1u, 64u);
    out.grid.slices = std::clamp(out.grid.slices, 1u, 64u);
    out.grid.max_lights_per_cluster = std::clamp(out.grid.max_lights_per_cluster, 1u, 1024u);
    const ClusterGridSettings& g = out.grid;
    out.z_near = std::max(z_near, 1e-4f);
    out.z_far = std::max(z_far, out.z_near * 1.001f);
    const std::uint32_t clusters = out.cluster_count();
    const float px = proj[0].x, py = proj[1].y; // ndc = view.xy * p / depth
    const float log_ratio = std::log(out.z_far / out.z_near);
    const auto slice_depth = [&](std::uint32_t k) {
        return out.z_near * std::exp(log_ratio * static_cast<float>(k) / static_cast<float>(g.slices));
    };

    // Per-light lists built cluster by cluster: first count, then fill (two passes keep the
    // index list contiguous without per-cluster vectors).
    struct Range {
        std::uint32_t x0, x1, y0, y1, z0, z1;
        Vec3 c;
        float r;
    };
    std::vector<Range> ranges;
    ranges.reserve(lights.size());
    const auto tile = [](float ndc, std::uint32_t n, bool flip) {
        const float t = (flip ? (1.0f - ndc) : (ndc + 1.0f)) * 0.5f * static_cast<float>(n);
        return std::clamp(static_cast<int>(std::floor(t)), 0, static_cast<int>(n) - 1);
    };
    for (const GpuLight& l : lights) {
        const Vec4 s = light_bounding_sphere(l);
        const Vec3 c = view.transform_point({s.x, s.y, s.z});
        const float r = s.w, depth = -c.z;
        if (r <= 0.0f || depth + r < out.z_near || depth - r > out.z_far) {
            ranges.push_back({1, 0, 1, 0, 1, 0, c, r});
            continue;
        }
        const float d0 = std::max(depth - r, out.z_near), d1 = std::min(depth + r, out.z_far);
        Range rg{0, g.tiles_x - 1, 0, g.tiles_y - 1, 0, g.slices - 1, c, r};
        // Depth slices.
        rg.z0 = static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::floor(std::log(d0 / out.z_near) / log_ratio * g.slices)), 0, static_cast<int>(g.slices) - 1));
        rg.z1 = static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::floor(std::log(d1 / out.z_near) / log_ratio * g.slices)), 0, static_cast<int>(g.slices) - 1));
        // Screen tiles from the projected view-space box (extremes occur at the nearest or
        // farthest depth); lights straddling the near plane cover the whole screen.
        if (depth - r > out.z_near) {
            float nx0 = 1e9f, nx1 = -1e9f, ny0 = 1e9f, ny1 = -1e9f;
            for (float d : {depth - r, depth + r})
                for (float x : {c.x - r, c.x + r}) {
                    nx0 = std::min(nx0, x * px / d);
                    nx1 = std::max(nx1, x * px / d);
                }
            for (float d : {depth - r, depth + r})
                for (float y : {c.y - r, c.y + r}) {
                    ny0 = std::min(ny0, y * py / d);
                    ny1 = std::max(ny1, y * py / d);
                }
            if (nx1 < -1.0f || nx0 > 1.0f || ny1 < -1.0f || ny0 > 1.0f) {
                ranges.push_back({1, 0, 1, 0, 1, 0, c, r});
                continue;
            }
            rg.x0 = static_cast<std::uint32_t>(tile(nx0, g.tiles_x, false));
            rg.x1 = static_cast<std::uint32_t>(tile(nx1, g.tiles_x, false));
            rg.y0 = static_cast<std::uint32_t>(tile(ny1, g.tiles_y, true)); // tile rows go top-down
            rg.y1 = static_cast<std::uint32_t>(tile(ny0, g.tiles_y, true));
        }
        ranges.push_back(rg);
    }

    // Cluster view-space boxes are evaluated lazily per candidate (sphere vs box refinement).
    const auto cluster_box_hit = [&](std::uint32_t tx, std::uint32_t ty, std::uint32_t tz, Vec3 c, float r) {
        const float d0 = slice_depth(tz), d1 = slice_depth(tz + 1);
        const float x0 = 2.0f * static_cast<float>(tx) / g.tiles_x - 1.0f, x1 = 2.0f * static_cast<float>(tx + 1) / g.tiles_x - 1.0f;
        const float y0 = 1.0f - 2.0f * static_cast<float>(ty + 1) / g.tiles_y, y1 = 1.0f - 2.0f * static_cast<float>(ty) / g.tiles_y;
        Vec3 lo{1e30f, 1e30f, -d1}, hi{-1e30f, -1e30f, -d0};
        for (float d : {d0, d1}) {
            for (float x : {x0, x1}) {
                lo.x = std::min(lo.x, x * d / px);
                hi.x = std::max(hi.x, x * d / px);
            }
            for (float y : {y0, y1}) {
                lo.y = std::min(lo.y, y * d / py);
                hi.y = std::max(hi.y, y * d / py);
            }
        }
        const Vec3 q{std::clamp(c.x, lo.x, hi.x), std::clamp(c.y, lo.y, hi.y), std::clamp(c.z, lo.z, hi.z)};
        const Vec3 d = q - c;
        return math::dot(d, d) <= r * r;
    };

    std::vector<std::uint32_t> counts(clusters, 0);
    std::vector<std::uint32_t> hits; // (cluster, light) pairs
    for (std::uint32_t li = 0; li < ranges.size(); ++li) {
        const Range& rg = ranges[li];
        for (std::uint32_t z = rg.z0; z <= rg.z1 && rg.z0 <= rg.z1; ++z)
            for (std::uint32_t y = rg.y0; y <= rg.y1; ++y)
                for (std::uint32_t x = rg.x0; x <= rg.x1; ++x) {
                    if (!cluster_box_hit(x, y, z, rg.c, rg.r)) continue;
                    const std::uint32_t cl = (z * g.tiles_y + y) * g.tiles_x + x;
                    if (counts[cl] >= g.max_lights_per_cluster) {
                        if (counts[cl] == g.max_lights_per_cluster) {
                            ++out.overflowed_clusters;
                            ++counts[cl]; // mark once; clamped below
                        }
                        continue;
                    }
                    ++counts[cl];
                    hits.push_back(cl);
                    hits.push_back(li);
                }
    }
    out.data.assign(2 * clusters, 0);
    std::uint32_t offset = 2 * clusters;
    for (std::uint32_t c = 0; c < clusters; ++c) {
        const std::uint32_t n = std::min(counts[c], g.max_lights_per_cluster);
        out.data[2 * c] = offset;
        out.data[2 * c + 1] = 0;
        offset += n;
        out.max_per_cluster = std::max(out.max_per_cluster, n);
    }
    out.data.resize(offset, 0);
    for (std::size_t i = 0; i < hits.size(); i += 2) {
        const std::uint32_t c = hits[i];
        out.data[out.data[2 * c] + out.data[2 * c + 1]++] = hits[i + 1];
    }
    out.index_count = offset - 2 * clusters;
    return out;
}

} // namespace victus::render
