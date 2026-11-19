// SPDX-License-Identifier: Apache-2.0
// Acoustic maths shared by the audio engine and its tests (ADR-0096). Pure functions, no miniaudio.
#pragma once
#include "victus/audio/audio.hpp"

namespace victus::audio::detail {

inline constexpr float kBandLowHz = 400.0f;   // Steam Audio band centres
inline constexpr float kBandMidHz = 2500.0f;
inline constexpr float kNoFilterHz = 24000.0f;
inline constexpr float kMinOcclusionCutoffHz = 150.0f;

// Cutoff of a first-order low-pass whose magnitude at kBandMidHz is `ratio` times its magnitude at
// kBandLowHz, clamped to [kMinOcclusionCutoffHz, 20 kHz]. Returns 0 (no filter) for ratio >= 0.98.
[[nodiscard]] float tilt_cutoff(float ratio) noexcept;
// |H(f)| of a first-order low-pass at `cutoff`.
[[nodiscard]] float lowpass1_gain(float f, float cutoff) noexcept;

// Listener weight of a reverb zone: 1 inside the (oriented, scaled) box, falling linearly to 0 over
// `fade` metres outside it. `volume` receives the box volume in world units (tie-break).
[[nodiscard]] float zone_weight(const math::Mat4& world, const ReverbZoneComponent& z, math::Vec3 p, float* volume = nullptr) noexcept;

// Exponential smoothing factor for time constant `tau` seconds over `dt`.
[[nodiscard]] float smoothing(float dt, float tau) noexcept;

} // namespace victus::audio::detail
