// SPDX-License-Identifier: Apache-2.0
// Water volumes (ADR-0095): an axis-aligned body of water whose surface is a sum of Gerstner
// waves. The same wave set drives rendering (render::WaterSurface), Jolt buoyancy and script
// queries, so floating objects ride the waves the player sees.
#pragma once

#include "victus/core/math.hpp"
#include "victus/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace victus::gameplay {

using scene::Entity;

// Authored on an entity: the surface rests at the entity's world Y, centred on its world XZ
// (rotation and scale are ignored; waves are defined in world space so adjacent volumes match).
struct WaterComponent {
    math::Vec2 size{50, 50};                          // metres along X and Z
    math::Color shallow_color{0.6f, 0.9f, 0.85f, 1};  // tint of light passing through clear water
    math::Color deep_color{0.01f, 0.09f, 0.13f, 1};   // colour of the water body itself
    float clarity{4.0f};          // metres of water before the bottom fades into deep_color (Beer-Lambert)
    float wave_height{0.5f};      // crest-to-trough height of the primary wave, metres
    float wavelength{12.0f};      // primary wavelength, metres
    float wave_direction{30.0f};  // degrees from +X towards +Z
    float steepness{0.45f};       // 0 = sine waves .. 1 = sharp crests (Gerstner Q)
    float wave_speed{1.0f};       // multiplier on deep-water dispersion speed
    float foam{0.6f};             // shoreline and crest foam amount
    float refraction{0.035f};     // screen-space distortion of what lies below
    float roughness{0.06f};       // surface micro roughness (sun highlight width)
    std::int32_t resolution{128}; // grid cells per side of the rendered surface (16..512)
    float density{1000.0f};       // kg/m^3 for buoyancy (1000 fresh water, 1025 sea water)
    float linear_drag{0.5f};      // Jolt fluid drag factors
    float angular_drag{0.05f};
    float damping{1.5f};          // 1/s linear damping while submerged (wave radiation; settles bobbing)
    math::Vec2 current{0, 0};     // flow velocity on XZ, m/s (pushes floating bodies)
    bool visible{true};
};

inline constexpr std::size_t kWaterWaves = 4;

// One Gerstner wave, ready for evaluation: theta = k * dot(direction, xz) - phase.
struct WaterWave {
    math::Vec2 direction{1, 0}; // unit
    float k{0};                 // 2 pi / wavelength
    float omega{0};             // angular frequency, rad/s
    float amplitude{0};         // metres
    float q{0};                 // horizontal displacement factor (steepness / (k A N))
    float phase{0};             // omega * time, wrapped to [0, 2 pi)
};
using WaterWaves = std::array<WaterWave, kWaterWaves>;

// The component's wave set at `time` seconds (phases wrapped in double precision).
[[nodiscard]] WaterWaves water_waves(const WaterComponent& water, double time);

struct WaveSample {
    float height{0};       // above the rest level
    math::Vec3 normal{0, 1, 0};
    float jacobian{1};     // < 1 where the surface compresses (crests; foam)
};
// Surface at world `xz` (accounts for the horizontal Gerstner displacement by fixed-point
// inversion, so the result is the height of the displaced surface above that point).
[[nodiscard]] WaveSample sample_waves(const WaterWaves& waves, math::Vec2 xz);

struct WaterQuery {
    Entity water;
    float surface{0};      // world height of the surface above the point
    math::Vec3 normal{0, 1, 0};
    float depth{0};        // surface - point.y (> 0 below the surface)
    math::Vec3 current{0, 0, 0};
};
// The water volume containing the point's XZ whose surface is nearest, or nothing.
[[nodiscard]] std::optional<WaterQuery> query_water(scene::Scene& scene, math::Vec3 point);

// Seconds of simulated water time (advanced by the gameplay.water system in FixedUpdate).
[[nodiscard]] double water_time(scene::Scene& scene);
void advance_water_time(scene::Scene& scene, float dt);
void set_water_time(scene::Scene& scene, double time);

struct WaterVolume {
    Entity entity;
    math::Vec3 center;      // rest surface centre
    math::Vec2 half_extent;
    float max_height{0};    // largest crest above rest
    const WaterComponent* water{nullptr};
    WaterWaves waves;
};
// All visible-or-not water volumes with their waves at the current water time.
[[nodiscard]] std::vector<WaterVolume> water_volumes(scene::Scene& scene);

void register_water_components();

} // namespace victus::gameplay
