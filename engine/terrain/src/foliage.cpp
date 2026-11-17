// SPDX-License-Identifier: Apache-2.0
// Foliage scatter over terrains (ADR-0063): deterministic jittered-grid placement driven by the
// splat map, slope and height, kept per terrain chunk and rebuilt only where the terrain changed.
#include "victus/terrain/terrain.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace victus::terrain {

using scene::Entity;
using scene::Scene;

namespace {

std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Four uniform numbers in [0, 1) for grid cell (i, j).
struct CellRandom {
    std::uint64_t state;
    float next() noexcept {
        state = mix(state);
        return static_cast<float>(state >> 40) / static_cast<float>(1u << 24);
    }
};

CellRandom cell_random(std::uint32_t seed, std::int64_t i, std::int64_t j) noexcept {
    return {mix(mix(static_cast<std::uint64_t>(seed) ^ 0xF011A6Eull) ^ mix(static_cast<std::uint64_t>(i) * 0x100000001B3ull) ^
                static_cast<std::uint64_t>(j))};
}

std::uint64_t settings_hash(const FoliageComponent& f) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    // Field by field: padding bytes are not guaranteed zero.
    auto add = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t k = 0; k < n; ++k) h = (h ^ b[k]) * 1099511628211ull;
    };
    add(&f.layer, sizeof f.layer), add(&f.min_weight, sizeof f.min_weight), add(&f.density, sizeof f.density);
    add(&f.min_scale, sizeof f.min_scale), add(&f.max_scale, sizeof f.max_scale);
    add(&f.align_to_normal, sizeof f.align_to_normal), add(&f.random_yaw, sizeof f.random_yaw);
    add(&f.max_slope, sizeof f.max_slope), add(&f.min_height, sizeof f.min_height), add(&f.max_height, sizeof f.max_height);
    add(&f.offset, sizeof f.offset), add(&f.seed, sizeof f.seed), add(&f.max_instances, sizeof f.max_instances);
    return h;
}

float finite_or(float v, float fallback) noexcept { return std::isfinite(v) ? v : fallback; }

} // namespace

math::Vec3 local_normal(const Heightfield& hf, math::Vec3 size, float x, float z) noexcept {
    if (hf.resolution < 2) return {0, 1, 0};
    const float dx = size.x / static_cast<float>(hf.resolution - 1), dz = size.z / static_cast<float>(hf.resolution - 1);
    const float hx = size.x * 0.5f, hz = size.z * 0.5f;
    auto at = [&](float lx, float lz) { return local_height(hf, size, lx, lz).value_or(0.0f); };
    // Central differences, one-sided at the borders (divided by the actual distance).
    const float xa = std::clamp(x - dx, -hx, hx), xb = std::clamp(x + dx, -hx, hx);
    const float za = std::clamp(z - dz, -hz, hz), zb = std::clamp(z + dz, -hz, hz);
    const float cx = std::clamp(x, -hx, hx), cz = std::clamp(z, -hz, hz);
    const float gx = xb > xa ? (at(xb, cz) - at(xa, cz)) / (xb - xa) : 0.0f;
    const float gz = zb > za ? (at(cx, zb) - at(cx, za)) / (zb - za) : 0.0f;
    return math::normalize(math::Vec3{-gx, 1.0f, -gz});
}

float foliage_spacing(math::Vec3 size, const FoliageComponent& f) noexcept {
    const float density = std::clamp(finite_or(f.density, 0.0f), 0.0f, 1e4f);
    if (density <= 0.0f || size.x <= 0.0f || size.z <= 0.0f) return 0.0f;
    float spacing = 1.0f / std::sqrt(density);
    const double cap = std::clamp<double>(f.max_instances, 1.0, kMaxFoliageInstances);
    const double cells = static_cast<double>(size.x) * size.z / (static_cast<double>(spacing) * spacing);
    if (cells > cap) spacing = static_cast<float>(std::sqrt(static_cast<double>(size.x) * size.z / cap));
    return spacing;
}

std::vector<math::Mat4> scatter_chunk(const Heightfield& hf, math::Vec3 size, const FoliageComponent& f, float spacing,
                                      std::uint32_t cx, std::uint32_t cz) {
    std::vector<math::Mat4> out;
    const std::uint32_t cps = hf.chunks_per_side();
    if (!(spacing > 0.0f) || cx >= cps || cz >= cps || hf.resolution < 2) return out;
    // Chunk rectangle in local space; points are assigned to the chunk containing them
    // (half-open, the last chunk includes the far edge).
    const auto last = static_cast<float>(hf.resolution - 1);
    auto edge_x = [&](std::uint32_t c) { return (static_cast<float>(c * kChunkQuads) / last - 0.5f) * size.x; };
    auto edge_z = [&](std::uint32_t c) { return (static_cast<float>(c * kChunkQuads) / last - 0.5f) * size.z; };
    const float x0 = edge_x(cx), x1 = edge_x(cx + 1), z0 = edge_z(cz), z1 = edge_z(cz + 1);
    const bool last_x = cx + 1 == cps, last_z = cz + 1 == cps;
    const float ox = -size.x * 0.5f, oz = -size.z * 0.5f;
    const auto i0 = static_cast<std::int64_t>(std::floor((x0 - ox) / spacing)) - 1;
    const auto i1 = static_cast<std::int64_t>(std::ceil((x1 - ox) / spacing)) + 1;
    const auto j0 = static_cast<std::int64_t>(std::floor((z0 - oz) / spacing)) - 1;
    const auto j1 = static_cast<std::int64_t>(std::ceil((z1 - oz) / spacing)) + 1;
    const float min_weight = std::max(finite_or(f.min_weight, 0.0f), 0.0f); // above 1: nothing
    const float cos_slope = std::cos(math::radians(std::clamp(finite_or(f.max_slope, 90.0f), 0.0f, 90.0f)));
    const float min_scale = std::max(finite_or(f.min_scale, 1.0f), 0.0f);
    const float max_scale = std::max(finite_or(f.max_scale, min_scale), min_scale);
    const float align = std::clamp(finite_or(f.align_to_normal, 0.0f), 0.0f, 1.0f);
    const float offset = finite_or(f.offset, 0.0f);
    const std::int32_t layer = std::clamp(f.layer, -1, static_cast<std::int32_t>(kLayerCount) - 1);
    for (std::int64_t j = std::max<std::int64_t>(j0, 0); j <= j1; ++j) {
        for (std::int64_t i = std::max<std::int64_t>(i0, 0); i <= i1; ++i) {
            CellRandom r = cell_random(f.seed, i, j);
            const float x = ox + (static_cast<float>(i) + r.next()) * spacing;
            const float z = oz + (static_cast<float>(j) + r.next()) * spacing;
            if (x < x0 || z < z0 || (last_x ? x > x1 : x >= x1) || (last_z ? z > z1 : z >= z1)) continue;
            const float keep = r.next(), scale_t = r.next(), yaw_t = r.next();
            const float u = x / size.x + 0.5f, v = z / size.z + 0.5f;
            if (layer >= 0) {
                const float w = hf.weights(u, v)[layer];
                if (w < min_weight || keep >= w) continue;
            }
            // Height on the drawn triangles (not the bilinear sample) so instances sit on the mesh.
            const auto y = local_height(hf, size, x, z);
            if (!y) continue;
            const float hn = size.y > 0.0f ? *y / size.y : 0.0f;
            if (hn < f.min_height || hn > f.max_height) continue;
            const math::Vec3 n = local_normal(hf, size, x, z);
            if (n.y < cos_slope - 1e-6f) continue;
            // Up axis between world up and the normal, then a random turn about it.
            const math::Vec3 up = math::normalize(math::Vec3{n.x * align, 1.0f - align + n.y * align, n.z * align});
            math::Quat q;
            const math::Vec3 axis = math::cross({0, 1, 0}, up);
            const float s = math::length(axis);
            if (s > 1e-6f) q = math::Quat::axis_angle(axis * (1.0f / s), std::atan2(s, up.y));
            if (f.random_yaw) q = q * math::Quat::axis_angle({0, 1, 0}, yaw_t * 2.0f * std::numbers::pi_v<float>);
            const float scale = min_scale + (max_scale - min_scale) * scale_t;
            const math::Vec3 p = math::Vec3{x, *y, z} + up * offset;
            out.push_back(math::Mat4::trs(p, q, {scale, scale, scale}));
        }
    }
    return out;
}

void TerrainSystem::update_foliage(Scene& scene) {
    VICTUS_PROFILE_SCOPE("terrain.foliage");
    stats_.foliage = 0;
    stats_.foliage_instances = 0;
    auto& world = scene.world();
    std::vector<Entity> list;
    world.each<FoliageComponent>([&](Entity e, FoliageComponent&) { list.push_back(e); });
    for (const Entity e : list) {
        ++stats_.foliage;
        if (!world.get<FoliageRuntime>(e)) world.add<FoliageRuntime>(e);
        const FoliageComponent f = *world.get<FoliageComponent>(e);
        Entity t = world.has<TerrainComponent>(e) ? e : scene.parent(e);
        const TerrainRuntime* trt = t.is_null() ? nullptr : world.get<TerrainRuntime>(t);
        auto* rt = world.get<FoliageRuntime>(e);
        if (!trt || !trt->data || trt->chunks_per_side == 0) {
            *rt = {};
            continue;
        }
        const Heightfield& hf = *trt->data;
        const std::uint32_t cps = trt->chunks_per_side;
        const std::size_t count = static_cast<std::size_t>(cps) * cps;
        const std::uint64_t settings = settings_hash(f);
        const float spacing = foliage_spacing(trt->built_size, f);
        if (rt->terrain != t || rt->built_data != &hf || !(rt->built_size == trt->built_size) || rt->settings != settings ||
            rt->chunks.size() != count || rt->spacing != spacing) {
            rt->terrain = t;
            rt->built_data = &hf;
            rt->built_size = trt->built_size;
            rt->settings = settings;
            rt->spacing = spacing;
            rt->chunks.assign(count, {});
            rt->chunk_bounds.assign(count, {});
            rt->chunk_built.assign(count, ~0ull);
        }
        std::uint32_t total = 0;
        const std::uint32_t cap = std::min(f.max_instances, kMaxFoliageInstances);
        for (std::size_t c = 0; c < count; ++c) {
            if (rt->chunk_built[c] != hf.chunk_revisions[c]) {
                rt->chunks[c] = scatter_chunk(hf, trt->built_size, f, spacing, static_cast<std::uint32_t>(c % cps),
                                              static_cast<std::uint32_t>(c / cps));
                math::Aabb b;
                for (const math::Mat4& m : rt->chunks[c]) b.expand(m.transform_point({}));
                rt->chunk_bounds[c] = b;
                rt->chunk_built[c] = hf.chunk_revisions[c];
                ++stats_.foliage_rebuilds;
            }
            // Density can exceed the cap locally (splat weights): trim what goes over.
            if (total + rt->chunks[c].size() > cap) rt->chunks[c].resize(cap - std::min(cap, total));
            total += static_cast<std::uint32_t>(rt->chunks[c].size());
        }
        rt->instances = total;
        stats_.foliage_instances += total;
    }
}

} // namespace victus::terrain
