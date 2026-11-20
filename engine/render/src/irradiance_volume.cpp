// SPDX-License-Identifier: Apache-2.0
#include "victus/render/irradiance_volume.hpp"

#include "victus/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace victus::render {
using math::Vec3;

IrradianceGrid IrradianceGrid::sanitized() const noexcept {
    IrradianceGrid g = *this;
    const auto fin = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!fin(g.bounds.min) || !fin(g.bounds.max)) g.bounds = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const Vec3 lo{std::min(g.bounds.min.x, g.bounds.max.x), std::min(g.bounds.min.y, g.bounds.max.y),
                  std::min(g.bounds.min.z, g.bounds.max.z)};
    const Vec3 hi{std::max(g.bounds.min.x, g.bounds.max.x), std::max(g.bounds.min.y, g.bounds.max.y),
                  std::max(g.bounds.min.z, g.bounds.max.z)};
    g.bounds = {lo, hi};
    for (int a = 0; a < 3; ++a) {
        if (g.bounds.max[a] - g.bounds.min[a] < 1e-3f) {
            g.bounds.min[a] -= 0.5f;
            g.bounds.max[a] += 0.5f;
        }
    }
    g.counts[0] = std::clamp(g.counts[0], 2u, kMaxIrradianceProbesPerAxis);
    g.counts[1] = std::clamp(g.counts[1], 2u, kMaxIrradianceProbesY);
    g.counts[2] = std::clamp(g.counts[2], 2u, kMaxIrradianceProbesPerAxis);
    return g;
}

Vec3 IrradianceGrid::spacing() const noexcept {
    const Vec3 size = bounds.max - bounds.min;
    return {size.x / static_cast<float>(std::max(counts[0], 2u) - 1), size.y / static_cast<float>(std::max(counts[1], 2u) - 1),
            size.z / static_cast<float>(std::max(counts[2], 2u) - 1)};
}

Vec3 IrradianceGrid::probe_position(std::uint32_t i) const noexcept {
    const std::uint32_t x = i % counts[0];
    const std::uint32_t z = (i / counts[0]) % counts[2];
    const std::uint32_t y = i / (counts[0] * counts[2]);
    const Vec3 s = spacing();
    return bounds.min + Vec3{s.x * static_cast<float>(x), s.y * static_cast<float>(y), s.z * static_cast<float>(z)};
}

std::array<std::uint32_t, 3> irradiance_counts_for(Vec3 size, float spacing) noexcept {
    if (!std::isfinite(spacing) || spacing <= 0.01f) spacing = 2.0f;
    const auto n = [&](float extent, std::uint32_t max) {
        const float e = std::isfinite(extent) ? std::abs(extent) : 0.0f;
        return std::clamp(static_cast<std::uint32_t>(std::lround(e / spacing)) + 1u, 2u, max);
    };
    return {n(size.x, kMaxIrradianceProbesPerAxis), n(size.y, kMaxIrradianceProbesY), n(size.z, kMaxIrradianceProbesPerAxis)};
}

Result<std::unique_ptr<IrradianceVolume>> IrradianceVolume::create(GpuContext& ctx, const IrradianceGrid& grid) {
    std::unique_ptr<IrradianceVolume> v(new IrradianceVolume(ctx));
    v->grid_ = grid.sanitized();
    v->probes_.resize(v->grid_.probe_count());
    v->valid_.assign(v->grid_.probe_count(), 0);
    v->sun_.assign(v->grid_.probe_count(), 1.0f);
    rhi::TextureDesc d;
    d.name = "irradiance_volume";
    d.format = rhi::Format::RGBA16_FLOAT;
    d.width = v->texture_width();
    d.height = v->texture_height();
    d.usage = rhi::TextureUsageSampled;
    auto t = ctx.device().create_texture(d);
    if (!t) return std::move(t).error();
    v->texture_ = *t;
    VICTUS_TRY_STATUS(v->upload());
    return v;
}

IrradianceVolume::~IrradianceVolume() {
    if (texture_) ctx_.device().destroy(texture_);
}

void IrradianceVolume::set_probe(std::uint32_t index, const SphericalHarmonics9& sh) {
    if (index >= probes_.size()) return;
    SphericalHarmonics9 clean = sh;
    for (auto& c : clean.coefficients)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(c[k])) c[k] = 0.0f;
    probes_[index] = clean;
    valid_[index] = 1;
    dirty_ = true;
}

void IrradianceVolume::set_probe_sun(std::uint32_t index, float visibility) {
    if (index >= sun_.size()) return;
    sun_[index] = std::isfinite(visibility) ? std::clamp(visibility, 0.0f, 1.0f) : 1.0f;
    dirty_ = true;
}

float IrradianceVolume::sample_sun(Vec3 p) const noexcept {
    const auto& g = grid_;
    const Vec3 s = g.spacing();
    Vec3 f{};
    for (int a = 0; a < 3; ++a) {
        if (!(p[a] >= g.bounds.min[a] && p[a] <= g.bounds.max[a])) return 1.0f;
        f[a] = std::clamp((p[a] - g.bounds.min[a]) / s[a], 0.0f, static_cast<float>(g.counts[static_cast<std::size_t>(a)] - 1));
    }
    const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(f.x), g.counts[0] - 2);
    const std::uint32_t y0 = std::min(static_cast<std::uint32_t>(f.y), g.counts[1] - 2);
    const std::uint32_t z0 = std::min(static_cast<std::uint32_t>(f.z), g.counts[2] - 2);
    const Vec3 t{f.x - static_cast<float>(x0), f.y - static_cast<float>(y0), f.z - static_cast<float>(z0)};
    float v = 0.0f, weight = 0.0f;
    for (std::uint32_t c = 0; c < 8; ++c) {
        const std::uint32_t dx = c & 1u, dy = (c >> 1) & 1u, dz = (c >> 2) & 1u;
        const float w = (dx ? t.x : 1 - t.x) * (dy ? t.y : 1 - t.y) * (dz ? t.z : 1 - t.z);
        const std::uint32_t i = g.index(x0 + dx, y0 + dy, z0 + dz);
        if (!valid_[i] || w <= 0.0f) continue;
        v += sun_[i] * w;
        weight += w;
    }
    if (weight <= 1e-5f) return 1.0f;
    return 1.0f + (v / weight - 1.0f) * std::min(weight, 1.0f);
}

void IrradianceVolume::clear_probe(std::uint32_t index) {
    if (index >= probes_.size()) return;
    probes_[index] = {};
    valid_[index] = 0;
    dirty_ = true;
}

std::uint32_t IrradianceVolume::valid_count() const noexcept {
    return static_cast<std::uint32_t>(std::count(valid_.begin(), valid_.end(), std::uint8_t{1}));
}

Vec3 IrradianceVolume::sample(Vec3 p, Vec3 n, Vec3 fallback) const noexcept {
    const auto& g = grid_;
    const Vec3 s = g.spacing();
    Vec3 f{};
    for (int a = 0; a < 3; ++a) {
        if (p[a] < g.bounds.min[a] || p[a] > g.bounds.max[a]) return fallback;
        f[a] = std::clamp((p[a] - g.bounds.min[a]) / s[a], 0.0f, static_cast<float>(g.counts[static_cast<std::size_t>(a)] - 1));
    }
    const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(f.x), g.counts[0] - 2);
    const std::uint32_t y0 = std::min(static_cast<std::uint32_t>(f.y), g.counts[1] - 2);
    const std::uint32_t z0 = std::min(static_cast<std::uint32_t>(f.z), g.counts[2] - 2);
    const Vec3 t{f.x - static_cast<float>(x0), f.y - static_cast<float>(y0), f.z - static_cast<float>(z0)};
    SphericalHarmonics9 sum{}; // coefficients interpolated first, as in the shader
    float weight = 0.0f;
    for (std::uint32_t c = 0; c < 8; ++c) {
        const std::uint32_t dx = c & 1u, dy = (c >> 1) & 1u, dz = (c >> 2) & 1u;
        const float w = (dx ? t.x : 1 - t.x) * (dy ? t.y : 1 - t.y) * (dz ? t.z : 1 - t.z);
        const std::uint32_t i = g.index(x0 + dx, y0 + dy, z0 + dz);
        if (!valid_[i] || w <= 0.0f) continue;
        for (std::size_t k = 0; k < 9; ++k) sum.coefficients[k] = sum.coefficients[k] + probes_[i].coefficients[k] * w;
        weight += w;
    }
    if (weight <= 1e-5f) return fallback;
    for (auto& c : sum.coefficients) c = c * (1.0f / weight);
    const Vec3 v = sum.diffuse(n);
    return fallback + (v - fallback) * std::min(weight, 1.0f);
}

Status IrradianceVolume::upload() {
    if (!dirty_) return {};
    const std::uint32_t w = texture_width(), h = texture_height();
    const std::uint32_t nx = grid_.counts[0], ny = grid_.counts[1], nz = grid_.counts[2];
    std::vector<std::uint8_t> bytes(std::size_t{w} * h * 8, 0);
    const auto put = [&](std::uint32_t tx, std::uint32_t ty, Vec3 rgb, float a) {
        const std::uint16_t v[4] = {float_to_half(rgb.x), float_to_half(rgb.y), float_to_half(rgb.z), float_to_half(a)};
        std::memcpy(bytes.data() + (std::size_t{ty} * w + tx) * 8, v, 8);
    };
    for (std::uint32_t y = 0; y < ny; ++y)
        for (std::uint32_t z = 0; z < nz; ++z)
            for (std::uint32_t x = 0; x < nx; ++x) {
                const std::uint32_t i = grid_.index(x, y, z);
                const float valid = valid_[i] ? 1.0f : 0.0f;
                // Row 0 alpha: validity (the shader's weight). Row 1 alpha: sun visibility *
                // validity (ADR-0136). The other rows keep validity.
                for (std::uint32_t k = 0; k < 9; ++k)
                    put(y * nx + x, k * nz + z, probes_[i].coefficients[k] * valid, k == 1 ? sun_[i] * valid : valid);
            }
    auto cmd = ctx_.device().begin_commands();
    cmd.upload_texture(texture_, bytes);
    cmd.submit();
    dirty_ = false;
    return {};
}

} // namespace victus::render
