// SPDX-License-Identifier: Apache-2.0
// Terrain editing (ADR-0063): sculpt and paint brushes, region snapshots and ray queries.
#include "victus/terrain/terrain.hpp"

#include <algorithm>
#include <cmath>

namespace victus::terrain {

namespace {

float cell_x(const Heightfield& hf, math::Vec3 size) noexcept { return size.x / static_cast<float>(hf.resolution - 1); }
float cell_z(const Heightfield& hf, math::Vec3 size) noexcept { return size.z / static_cast<float>(hf.resolution - 1); }

SampleRect clamp_rect(const Heightfield& hf, SampleRect r) noexcept {
    const auto last = static_cast<std::int32_t>(hf.resolution) - 1;
    r.x0 = std::max(r.x0, 0);
    r.z0 = std::max(r.z0, 0);
    r.x1 = std::min(r.x1, last);
    r.z1 = std::min(r.z1, last);
    return r;
}

} // namespace

math::Mat4 rigid_matrix(const math::Mat4& world) noexcept {
    math::Mat4 m = world;
    for (int c = 0; c < 3; ++c) {
        const math::Vec3 axis = m.c[c].xyz();
        const float len = math::length(axis);
        math::Vec3 unit{c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f};
        if (len > 1e-8f) unit = axis * (1.0f / len);
        m.c[c] = math::Vec4(unit, 0.0f);
    }
    return m;
}

void SampleRect::merge(const SampleRect& o) noexcept {
    if (o.empty()) return;
    if (empty()) {
        *this = o;
        return;
    }
    x0 = std::min(x0, o.x0);
    z0 = std::min(z0, o.z0);
    x1 = std::max(x1, o.x1);
    z1 = std::max(z1, o.z1);
}

std::string_view brush_mode_name(BrushMode mode) noexcept {
    switch (mode) {
    case BrushMode::Raise: return "Raise";
    case BrushMode::Lower: return "Lower";
    case BrushMode::Smooth: return "Smooth";
    case BrushMode::Flatten: return "Flatten";
    case BrushMode::Paint: return "Paint";
    }
    return "Raise";
}

float brush_weight(const Brush& brush, float distance) noexcept {
    const float r = std::max(brush.radius, 1e-4f);
    if (!(distance < r)) return 0.0f;
    const float falloff = std::clamp(brush.falloff, 0.0f, 1.0f);
    const float inner = 1.0f - falloff;
    const float d = std::max(distance, 0.0f) / r;
    if (d <= inner || falloff <= 0.0f) return 1.0f;
    const float t = std::clamp((d - inner) / falloff, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

SampleRect apply_brush(Heightfield& hf, math::Vec3 size, const Brush& brush, float x, float z, float amount) {
    if (hf.empty() || !std::isfinite(x) || !std::isfinite(z) || !(amount > 0.0f) || !std::isfinite(amount)) return {};
    const float strength = std::clamp(brush.strength, 0.0f, 1.0f);
    if (strength <= 0.0f) return {};
    const float cx = cell_x(hf, size), cz = cell_z(hf, size);
    const float radius = std::clamp(brush.radius, 1e-3f, std::max(size.x, size.z) * 2.0f);
    // Sample coordinates of the centre (sample 0 at -size/2).
    const float sx = (x + size.x * 0.5f) / cx, sz = (z + size.z * 0.5f) / cz;
    const SampleRect area = clamp_rect(hf, {static_cast<std::int32_t>(std::floor(sx - radius / cx)),
                                            static_cast<std::int32_t>(std::floor(sz - radius / cz)),
                                            static_cast<std::int32_t>(std::ceil(sx + radius / cx)),
                                            static_cast<std::int32_t>(std::ceil(sz + radius / cz))});
    if (area.empty()) return {};
    Brush b = brush;
    b.radius = radius;
    // Smoothing reads the unmodified neighbourhood.
    HeightfieldRegion source;
    if (brush.mode == BrushMode::Smooth)
        source = copy_region(hf, {area.x0 - 1, area.z0 - 1, area.x1 + 1, area.z1 + 1});
    const auto original = [&](std::int32_t ix, std::int32_t iz) {
        ix = std::clamp(ix, source.rect.x0, source.rect.x1);
        iz = std::clamp(iz, source.rect.z0, source.rect.z1);
        return source.heights[static_cast<std::size_t>(iz - source.rect.z0) * static_cast<std::size_t>(source.rect.x1 - source.rect.x0 + 1) +
                              static_cast<std::size_t>(ix - source.rect.x0)];
    };
    const float height_scale = size.y > 1e-6f ? 1.0f / size.y : 0.0f;
    const math::Vec4 onehot{brush.layer == 0 ? 1.0f : 0.0f, brush.layer == 1 ? 1.0f : 0.0f, brush.layer == 2 ? 1.0f : 0.0f,
                            brush.layer == 3 ? 1.0f : 0.0f};
    SampleRect changed;
    for (std::int32_t iz = area.z0; iz <= area.z1; ++iz)
        for (std::int32_t ix = area.x0; ix <= area.x1; ++ix) {
            const float dx = (static_cast<float>(ix) - sx) * cx, dz = (static_cast<float>(iz) - sz) * cz;
            const float w = brush_weight(b, std::sqrt(dx * dx + dz * dz));
            if (w <= 0.0f) continue;
            const std::size_t i = hf.index(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iz));
            bool did = false;
            switch (brush.mode) {
            case BrushMode::Raise:
            case BrushMode::Lower: {
                const float sign = brush.mode == BrushMode::Raise ? 1.0f : -1.0f;
                const float h = std::clamp(hf.heights[i] + sign * strength * amount * w * radius * 0.5f * height_scale, 0.0f, 1.0f);
                did = h != hf.heights[i];
                hf.heights[i] = h;
                break;
            }
            case BrushMode::Smooth: {
                float sum = 0.0f;
                for (std::int32_t oz = -1; oz <= 1; ++oz)
                    for (std::int32_t ox = -1; ox <= 1; ++ox) sum += original(ix + ox, iz + oz);
                const float k = std::clamp(strength * amount * w * 8.0f, 0.0f, 1.0f);
                const float h = hf.heights[i] + (sum / 9.0f - hf.heights[i]) * k;
                did = h != hf.heights[i];
                hf.heights[i] = h;
                break;
            }
            case BrushMode::Flatten: {
                const float k = std::clamp(strength * amount * w * 4.0f, 0.0f, 1.0f);
                const float h = hf.heights[i] + (std::clamp(brush.target, 0.0f, 1.0f) - hf.heights[i]) * k;
                did = h != hf.heights[i];
                hf.heights[i] = h;
                break;
            }
            case BrushMode::Paint: {
                if (brush.layer >= kLayerCount) break;
                const float k = std::clamp(strength * amount * w * 4.0f, 0.0f, 1.0f);
                const math::Vec4 cur = unpack_weights(hf.splat[i]);
                math::Vec4 next;
                for (int l = 0; l < 4; ++l) next[l] = cur[l] + (onehot[l] - cur[l]) * k;
                std::uint32_t packed = pack_weights(next);
                // Quantization can stall a slow stroke: always move at least one step.
                if (packed == hf.splat[i] && k > 0.0f && cur[static_cast<int>(brush.layer)] < 1.0f) {
                    for (int l = 0; l < 4; ++l) next[l] = cur[l] + (onehot[l] - cur[l]) * std::max(k, 2.0f / 255.0f);
                    packed = pack_weights(next);
                }
                did = packed != hf.splat[i];
                hf.splat[i] = packed;
                break;
            }
            }
            if (did) changed.merge({ix, iz, ix, iz});
        }
    if (!changed.empty()) hf.touch(changed.x0, changed.z0, changed.x1, changed.z1);
    return changed;
}

HeightfieldRegion copy_region(const Heightfield& hf, SampleRect rect) {
    HeightfieldRegion out;
    if (hf.empty()) return out;
    out.rect = clamp_rect(hf, rect);
    if (out.rect.empty()) return out;
    out.heights.reserve(out.rect.count());
    out.splat.reserve(out.rect.count());
    for (std::int32_t z = out.rect.z0; z <= out.rect.z1; ++z)
        for (std::int32_t x = out.rect.x0; x <= out.rect.x1; ++x) {
            const std::size_t i = hf.index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
            out.heights.push_back(hf.heights[i]);
            out.splat.push_back(hf.splat[i]);
        }
    return out;
}

void paste_region(Heightfield& hf, const HeightfieldRegion& region) {
    const SampleRect r = region.rect;
    if (hf.empty() || r.empty() || r.x0 < 0 || r.z0 < 0 || r.x1 >= static_cast<std::int32_t>(hf.resolution) ||
        r.z1 >= static_cast<std::int32_t>(hf.resolution) || region.heights.size() != r.count() || region.splat.size() != r.count())
        return;
    std::size_t k = 0;
    for (std::int32_t z = r.z0; z <= r.z1; ++z)
        for (std::int32_t x = r.x0; x <= r.x1; ++x, ++k) {
            const std::size_t i = hf.index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
            hf.heights[i] = std::clamp(region.heights[k], 0.0f, 1.0f);
            hf.splat[i] = region.splat[k];
        }
    hf.touch(r.x0, r.z0, r.x1, r.z1);
}

std::optional<math::Vec3> raycast_local(const Heightfield& hf, math::Vec3 size, math::Vec3 origin, math::Vec3 direction,
                                        float max_distance) noexcept {
    if (hf.empty() || !(max_distance > 0.0f)) return std::nullopt;
    const float len = math::length(direction);
    if (!(len > 1e-8f)) return std::nullopt;
    const math::Vec3 d = direction * (1.0f / len);
    // Clip to the terrain's bounding box.
    const math::Vec3 lo{-size.x * 0.5f, -1e-3f, -size.z * 0.5f}, hi{size.x * 0.5f, size.y + 1e-3f, size.z * 0.5f};
    float t0 = 0.0f, t1 = max_distance;
    const float o[3] = {origin.x, origin.y, origin.z}, dv[3] = {d.x, d.y, d.z};
    const float mn[3] = {lo.x, lo.y, lo.z}, mx[3] = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; ++a) {
        if (std::abs(dv[a]) < 1e-9f) {
            if (o[a] < mn[a] || o[a] > mx[a]) return std::nullopt;
            continue;
        }
        float ta = (mn[a] - o[a]) / dv[a], tb = (mx[a] - o[a]) / dv[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return std::nullopt;
    }
    const auto above = [&](float t) -> std::optional<float> {
        const math::Vec3 p = origin + d * t;
        const auto h = local_height(hf, size, std::clamp(p.x, lo.x, hi.x), std::clamp(p.z, lo.z, hi.z));
        if (!h) return std::nullopt;
        return p.y - *h;
    };
    // March at half a cell, then bisect the bracketing step.
    const float step = std::max(0.5f * std::min(cell_x(hf, size), cell_z(hf, size)), 1e-4f);
    auto prev = above(t0);
    if (!prev) return std::nullopt;
    if (*prev <= 0.0f) return origin + d * t0; // starts below the surface (or on it)
    float tp = t0;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const float t = std::min(tp + step, t1);
        const auto cur = above(t);
        if (!cur) return std::nullopt;
        if (*cur <= 0.0f) {
            float a = tp, b = t;
            for (int i = 0; i < 24; ++i) {
                const float m = 0.5f * (a + b);
                const auto v = above(m);
                if (v && *v > 0.0f) a = m;
                else b = m;
            }
            return origin + d * b;
        }
        if (t >= t1) break;
        tp = t;
    }
    return std::nullopt;
}

std::optional<TerrainHit> raycast(scene::Scene& scene, scene::Entity entity, math::Vec3 origin, math::Vec3 direction,
                                  float max_distance) {
    auto& world = scene.world();
    const auto* rt = world.get<TerrainRuntime>(entity);
    const auto* wt = world.get<scene::WorldTransform>(entity);
    if (!rt || !rt->data || !wt) return std::nullopt;
    const float len = math::length(direction);
    if (!(len > 1e-8f)) return std::nullopt;
    const math::Mat4 m = rigid_matrix(wt->matrix);
    const math::Mat4 inv = math::inverse(m);
    const math::Vec3 lo = inv.transform_point(origin);
    const math::Vec3 ld = (inv * math::Vec4(direction * (1.0f / len), 0.0f)).xyz();
    const auto p = raycast_local(*rt->data, rt->built_size, lo, ld, max_distance);
    if (!p) return std::nullopt;
    TerrainHit hit;
    hit.entity = entity;
    hit.local = *p;
    hit.point = m.transform_point(*p);
    hit.distance = math::length(hit.point - origin);
    // Normal from the nearest sample's central differences.
    const Heightfield& hf = *rt->data;
    const auto last = static_cast<float>(hf.resolution - 1);
    const auto sx = static_cast<std::int32_t>(std::lround((p->x / rt->built_size.x + 0.5f) * last));
    const auto sz = static_cast<std::int32_t>(std::lround((p->z / rt->built_size.z + 0.5f) * last));
    hit.normal = math::normalize((m * math::Vec4(sample_normal(hf, rt->built_size, sx, sz), 0.0f)).xyz());
    return hit;
}

std::optional<TerrainHit> raycast(scene::Scene& scene, math::Vec3 origin, math::Vec3 direction, float max_distance) {
    std::vector<scene::Entity> terrains;
    scene.world().each<TerrainComponent>([&](scene::Entity e, TerrainComponent&) { terrains.push_back(e); });
    std::optional<TerrainHit> best;
    for (const scene::Entity e : terrains)
        if (auto h = raycast(scene, e, origin, direction, max_distance); h && (!best || h->distance < best->distance)) best = h;
    return best;
}

std::optional<TerrainSample> sample_at(scene::Scene& scene, scene::Entity entity, float x, float z) {
    const auto h = height_at(scene, entity, x, z);
    if (!h) return std::nullopt;
    auto& world = scene.world();
    const auto* rt = world.get<TerrainRuntime>(entity);
    const auto* wt = world.get<scene::WorldTransform>(entity);
    const Heightfield& hf = *rt->data;
    const math::Vec3 size = rt->built_size;
    const math::Mat4 m = rigid_matrix(wt->matrix);
    const math::Vec3 local = math::inverse(m).transform_point({x, *h, z});
    TerrainSample out;
    out.entity = entity;
    out.height = *h;
    const float u = local.x / size.x + 0.5f, v = local.z / size.z + 0.5f;
    out.weights = hf.weights(u, v);
    const math::Vec3 n = local_normal(hf, size, local.x, local.z);
    out.normal = math::normalize((m * math::Vec4(n, 0.0f)).xyz());
    return out;
}

std::optional<TerrainSample> sample_at(scene::Scene& scene, float x, float z) {
    std::vector<scene::Entity> terrains;
    scene.world().each<TerrainComponent>([&](scene::Entity e, TerrainComponent&) { terrains.push_back(e); });
    for (const scene::Entity e : terrains)
        if (auto s = sample_at(scene, e, x, z)) return s;
    return std::nullopt;
}

} // namespace victus::terrain
