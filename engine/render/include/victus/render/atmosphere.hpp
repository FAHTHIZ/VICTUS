// SPDX-License-Identifier: Apache-2.0
// Physically based sky: single-scattering Rayleigh + Mie atmosphere (Nishita et al. 1993 with
// the Earth coefficients of Bruneton & Neyret 2008 / Hillaire 2020) evaluated on the CPU into a
// small equirect, which EnvironmentMap bakes into the IBL cubemap. A 128x64 image costs a few
// milliseconds on one core, so time of day can re-bake whenever the sun moves noticeably.
#pragma once
#include "victus/render/image.hpp"

namespace victus::render {

struct AtmosphereSettings {
    math::Vec3 sun_direction{0.3f, 0.8f, -0.5f}; // towards the sun (normalised internally)
    float sun_illuminance{3.0f};                 // sun irradiance in scene units (the directional light intensity)
    // Extra gain on scattered light only: single scattering misses multiple bounces and engine
    // suns are far dimmer than 100 klx, so ~8 matches real sky-to-sun ratios (sky ~ 1/20 of sun).
    float sky_gain{8.0f};
    float rayleigh_scale{1.0f};                  // air density (1 = Earth); higher = deeper blue / redder sunsets
    float mie_scale{1.0f};                       // aerosol density / haze (1 = clear day)
    float mie_g{0.76f};                          // Mie phase anisotropy (forward scattering halo)
    math::Vec3 ground_albedo{0.3f, 0.3f, 0.3f};  // lower hemisphere
    float altitude{200.0f};                      // viewer height above ground, metres
    std::uint32_t width{128};                    // equirect width (height = width / 2), 16..1024
    [[nodiscard]] bool same_as(const AtmosphereSettings& o, float max_sun_angle_radians = 0.0087f) const noexcept;
};

// Linear HDR equirect (u = 0.5 looks down -Z, v = 0 is +Y, like every VICTUS environment).
[[nodiscard]] ImageF render_atmosphere(const AtmosphereSettings& s);
// Radiance of one view direction (direction need not be normalised).
[[nodiscard]] math::Vec3 atmosphere_radiance(const AtmosphereSettings& s, math::Vec3 direction);
// Fraction of sunlight reaching the viewer through the atmosphere (sunset reddening); zero
// when the sun is below the horizon.
[[nodiscard]] math::Vec3 atmosphere_sun_transmittance(const AtmosphereSettings& s);

} // namespace victus::render
