// SPDX-License-Identifier: Apache-2.0
// Irradiance volumes (ADR-0056): a regular grid of light probes inside a world-aligned box, each
// storing SH9 diffuse irradiance of the scene around it. Lit surfaces inside the box take their
// ambient (indirect) diffuse light from the trilinearly interpolated probes instead of the
// environment, so light bounces off nearby surfaces and interiors are not lit by the open sky.
//
// GPU layout: one RGBA16F 2D texture of (nx * ny) x (nz * 9) texels. Each Y layer is an nx x nz
// tile, layers sit side by side horizontally and the nine SH coefficients stack vertically, so a
// lookup is 2 layers x 9 hardware-bilinear fetches. RGB holds coefficient * validity and A the
// validity (1 baked, 0 not yet), so partially baked volumes blend smoothly into the fallback.
#pragma once
#include "victus/render/environment_map.hpp"
#include "victus/render/gpu_context.hpp"

#include <array>
#include <memory>
#include <vector>

namespace victus::render {

inline constexpr std::uint32_t kMaxIrradianceProbesPerAxis = 32;
inline constexpr std::uint32_t kMaxIrradianceProbesY = 16;

struct IrradianceGrid {
    math::Aabb bounds{};                   // probes sit on the box corners and evenly in between
    std::array<std::uint32_t, 3> counts{}; // probes per axis, each 2..32 (y 2..16)

    [[nodiscard]] std::uint32_t probe_count() const noexcept { return counts[0] * counts[1] * counts[2]; }
    [[nodiscard]] std::uint32_t index(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept {
        return (y * counts[2] + z) * counts[0] + x;
    }
    [[nodiscard]] math::Vec3 probe_position(std::uint32_t index) const noexcept;
    [[nodiscard]] math::Vec3 spacing() const noexcept;
    // Clamps the counts into range and orders the box; invalid boxes become a unit box.
    [[nodiscard]] IrradianceGrid sanitized() const noexcept;
};

// Probe counts that give roughly `spacing` metres between probes inside `size`, within limits.
[[nodiscard]] std::array<std::uint32_t, 3> irradiance_counts_for(math::Vec3 size, float spacing) noexcept;

class IrradianceVolume {
public:
    [[nodiscard]] static Result<std::unique_ptr<IrradianceVolume>> create(GpuContext& ctx, const IrradianceGrid& grid);
    ~IrradianceVolume();
    IrradianceVolume(const IrradianceVolume&) = delete;
    IrradianceVolume& operator=(const IrradianceVolume&) = delete;

    [[nodiscard]] const IrradianceGrid& grid() const noexcept { return grid_; }
    // Stores a probe's irradiance (CPU side; call upload() to make it visible to the GPU).
    void set_probe(std::uint32_t index, const SphericalHarmonics9& sh);
    void clear_probe(std::uint32_t index);
    [[nodiscard]] bool probe_valid(std::uint32_t index) const noexcept { return index < valid_.size() && valid_[index]; }
    [[nodiscard]] const SphericalHarmonics9& probe(std::uint32_t index) const noexcept { return probes_[index]; }
    [[nodiscard]] std::uint32_t valid_count() const noexcept;
    // CPU reference of the shader's lookup: irradiance / pi for normal n at world position p
    // (trilinear over valid probes; the fallback is returned outside the box or with no data).
    [[nodiscard]] math::Vec3 sample(math::Vec3 p, math::Vec3 n, math::Vec3 fallback) const noexcept;

    // Rewrites the texture from the CPU probes if anything changed since the last upload.
    Status upload();
    [[nodiscard]] rhi::TextureHandle texture() const noexcept { return texture_; }
    [[nodiscard]] std::uint32_t texture_width() const noexcept { return grid_.counts[0] * grid_.counts[1]; }
    [[nodiscard]] std::uint32_t texture_height() const noexcept { return grid_.counts[2] * 9; }

private:
    explicit IrradianceVolume(GpuContext& ctx) : ctx_(ctx) {}
    GpuContext& ctx_;
    IrradianceGrid grid_;
    std::vector<SphericalHarmonics9> probes_;
    std::vector<std::uint8_t> valid_;
    rhi::TextureHandle texture_;
    bool dirty_{true};
};

} // namespace victus::render
