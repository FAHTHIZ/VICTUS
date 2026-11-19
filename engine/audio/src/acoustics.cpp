// SPDX-License-Identifier: Apache-2.0
#include "acoustics.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace victus::audio {

namespace {
// Energy transmission per band (400 Hz, 2.5 kHz, 15 kHz) from Steam Audio's IPL_MATERIAL presets
// (core/src/core/phonon.h), the same table its occlusion/transmission model uses.
constexpr std::array<math::Vec3, 11> kPresetTransmission{{
    {0.100f, 0.050f, 0.030f}, // generic
    {0.015f, 0.015f, 0.015f}, // brick
    {0.015f, 0.002f, 0.001f}, // concrete
    {0.060f, 0.044f, 0.011f}, // ceramic
    {0.031f, 0.012f, 0.008f}, // gravel
    {0.020f, 0.005f, 0.003f}, // carpet
    {0.060f, 0.044f, 0.011f}, // glass
    {0.056f, 0.056f, 0.004f}, // plaster
    {0.070f, 0.014f, 0.005f}, // wood
    {0.200f, 0.025f, 0.010f}, // metal
    {0.015f, 0.002f, 0.001f}, // rock
}};
} // namespace

math::Vec3 acoustic_transmission(const AcousticMaterialComponent& m) noexcept {
    const auto i = static_cast<std::size_t>(m.preset);
    if (i < kPresetTransmission.size()) return kPresetTransmission[i];
    const auto c = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
    return {c(m.transmission.x), c(m.transmission.y), c(m.transmission.z)};
}

namespace detail {

float tilt_cutoff(float ratio) noexcept {
    if (!std::isfinite(ratio) || ratio >= 0.98f) return 0.0f;
    const float lo2 = kBandLowHz * kBandLowHz, mid2 = kBandMidHz * kBandMidHz;
    const float r2 = ratio * ratio;
    const float den = mid2 * r2 - lo2; // <= 0 when the requested tilt exceeds one pole's slope
    if (den <= 0.0f) return kMinOcclusionCutoffHz;
    const float inv_fc2 = (1.0f - r2) / den;
    return std::clamp(1.0f / std::sqrt(inv_fc2), kMinOcclusionCutoffHz, 20000.0f);
}

float lowpass1_gain(float f, float cutoff) noexcept {
    if (cutoff <= 0.0f) return 1.0f;
    const float x = f / cutoff;
    return 1.0f / std::sqrt(1.0f + x * x);
}

float zone_weight(const math::Mat4& world, const ReverbZoneComponent& z, math::Vec3 p, float* volume) noexcept {
    const math::Vec3 t = world[3].xyz();
    const std::array<math::Vec3, 3> axes{world[0].xyz(), world[1].xyz(), world[2].xyz()};
    const math::Vec3 d = p - t;
    float outside2 = 0.0f, vol = 1.0f;
    const std::array<float, 3> size{std::abs(z.size.x), std::abs(z.size.y), std::abs(z.size.z)};
    for (std::size_t k = 0; k < 3; ++k) {
        const float s = math::length(axes[k]);
        const float half = 0.5f * size[k] * s;
        vol *= 2.0f * half;
        const float along = s > 1e-8f ? math::dot(d, axes[k]) / s : 0.0f;
        const float o = std::max(0.0f, std::abs(along) - half);
        outside2 += o * o;
    }
    if (volume) *volume = vol;
    if (outside2 <= 0.0f) return 1.0f;
    const float fade = std::max(0.0f, z.fade);
    if (fade <= 0.0f) return 0.0f;
    return std::clamp(1.0f - std::sqrt(outside2) / fade, 0.0f, 1.0f);
}

float smoothing(float dt, float tau) noexcept {
    if (!(dt > 0.0f)) return 0.0f;
    if (!(tau > 0.0f)) return 1.0f;
    return 1.0f - std::exp(-dt / tau);
}

} // namespace detail
} // namespace victus::audio
