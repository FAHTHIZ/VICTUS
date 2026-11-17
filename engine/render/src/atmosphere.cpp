// SPDX-License-Identifier: Apache-2.0
#include "victus/render/atmosphere.hpp"

#include "victus/render/environment_map.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kPlanet = 6360e3f;     // metres
constexpr float kTop = 6460e3f;        // atmosphere top
constexpr float kRayleighH = 8000.0f;  // scale heights
constexpr float kMieH = 1200.0f;
const math::Vec3 kRayleighBeta{5.802e-6f, 13.558e-6f, 33.1e-6f}; // per metre (Hillaire 2020)
constexpr float kMieScatter = 3.996e-6f;
constexpr float kMieExtinct = 4.40e-6f;
constexpr int kViewSamples = 16;
constexpr int kLightSamples = 6;

// Distance along `d` from `o` to the sphere of radius r (far hit), or -1.
float sphere_exit(math::Vec3 o, math::Vec3 d, float r) {
    const float b = math::dot(o, d);
    const float c = math::dot(o, o) - r * r;
    const float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + std::sqrt(disc);
}
// Nearest positive hit with the planet, or -1.
float ground_hit(math::Vec3 o, math::Vec3 d) {
    const float b = math::dot(o, d);
    const float c = math::dot(o, o) - kPlanet * kPlanet;
    const float disc = b * b - c;
    if (disc < 0.0f || b > 0.0f) return -1.0f;
    const float t = -b - std::sqrt(disc);
    return t > 0.0f ? t : -1.0f;
}

struct Media {
    math::Vec3 rayleigh;
    float mie_s, mie_e;
};
Media media(const AtmosphereSettings& s) {
    const float r = std::isfinite(s.rayleigh_scale) ? std::clamp(s.rayleigh_scale, 0.0f, 20.0f) : 1.0f;
    const float m = std::isfinite(s.mie_scale) ? std::clamp(s.mie_scale, 0.0f, 50.0f) : 1.0f;
    return {kRayleighBeta * r, kMieScatter * m, kMieExtinct * m};
}
math::Vec3 extinction(const Media& md, float od_r, float od_m) {
    const math::Vec3 tau = md.rayleigh * od_r + math::Vec3{1, 1, 1} * (md.mie_e * od_m);
    return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
}
// Optical depth (rayleigh, mie) from p towards the sun; false if the planet blocks it.
bool light_depth(math::Vec3 p, math::Vec3 sun, float& od_r, float& od_m) {
    if (ground_hit(p, sun) > 0.0f) return false;
    const float len = sphere_exit(p, sun, kTop);
    const float ds = len / kLightSamples;
    od_r = od_m = 0.0f;
    for (int j = 0; j < kLightSamples; ++j) {
        const math::Vec3 q = p + sun * ((static_cast<float>(j) + 0.5f) * ds);
        const float h = std::max(math::length(q) - kPlanet, 0.0f);
        od_r += std::exp(-h / kRayleighH) * ds;
        od_m += std::exp(-h / kMieH) * ds;
    }
    return true;
}
math::Vec3 sun_dir_of(const AtmosphereSettings& s) {
    const math::Vec3 d = s.sun_direction;
    const float l = math::length(d);
    return l > 1e-6f && std::isfinite(l) ? d / l : math::Vec3{0, 1, 0};
}
float origin_height(const AtmosphereSettings& s) {
    return kPlanet + (std::isfinite(s.altitude) ? std::clamp(s.altitude, 1.0f, 50000.0f) : 200.0f);
}
} // namespace

bool AtmosphereSettings::same_as(const AtmosphereSettings& o, float max_sun_angle_radians) const noexcept {
    const auto close = [](float a, float b) { return std::abs(a - b) <= 1e-4f * std::max(1.0f, std::abs(a)); };
    const math::Vec3 a = math::normalize(sun_direction), b = math::normalize(o.sun_direction);
    return math::dot(a, b) >= std::cos(max_sun_angle_radians) && close(sun_illuminance, o.sun_illuminance) && close(sky_gain, o.sky_gain) &&
           close(rayleigh_scale, o.rayleigh_scale) && close(mie_scale, o.mie_scale) && close(mie_g, o.mie_g) &&
           close(ground_albedo.x, o.ground_albedo.x) && close(ground_albedo.y, o.ground_albedo.y) &&
           close(ground_albedo.z, o.ground_albedo.z) && close(altitude, o.altitude) && width == o.width;
}

math::Vec3 atmosphere_sun_transmittance(const AtmosphereSettings& s) {
    const Media md = media(s);
    float od_r = 0, od_m = 0;
    if (!light_depth({0, origin_height(s), 0}, sun_dir_of(s), od_r, od_m)) return {0, 0, 0};
    return extinction(md, od_r, od_m);
}

math::Vec3 atmosphere_radiance(const AtmosphereSettings& s, math::Vec3 direction) {
    const Media md = media(s);
    const math::Vec3 sun = sun_dir_of(s);
    const float len_d = math::length(direction);
    const math::Vec3 d = len_d > 1e-6f ? direction / len_d : math::Vec3{0, 1, 0};
    const math::Vec3 o{0, origin_height(s), 0};
    const float e_sun = std::isfinite(s.sun_illuminance) ? std::max(s.sun_illuminance, 0.0f) : 0.0f;
    const float g = std::isfinite(s.mie_g) ? std::clamp(s.mie_g, -0.95f, 0.95f) : 0.76f;
    const float mu = math::dot(d, sun);
    const float phase_r = 3.0f / (16.0f * kPi) * (1.0f + mu * mu);
    const float phase_m = 3.0f / (8.0f * kPi) * ((1.0f - g * g) * (1.0f + mu * mu)) /
                          ((2.0f + g * g) * std::pow(std::max(1.0f + g * g - 2.0f * g * mu, 1e-4f), 1.5f));
    const float t_ground = ground_hit(o, d);
    const float len = t_ground > 0.0f ? t_ground : sphere_exit(o, d, kTop);
    const float ds = len / kViewSamples;
    float od_r = 0, od_m = 0;
    math::Vec3 sum_r{}, sum_m{};
    for (int i = 0; i < kViewSamples; ++i) {
        const math::Vec3 p = o + d * ((static_cast<float>(i) + 0.5f) * ds);
        const float h = std::max(math::length(p) - kPlanet, 0.0f);
        const float dr = std::exp(-h / kRayleighH) * ds, dm = std::exp(-h / kMieH) * ds;
        od_r += dr;
        od_m += dm;
        float lr = 0, lm = 0;
        if (!light_depth(p, sun, lr, lm)) continue;
        const math::Vec3 t = extinction(md, od_r + lr, od_m + lm);
        sum_r += t * dr;
        sum_m += t * dm;
    }
    math::Vec3 l = (math::Vec3{sum_r.x * md.rayleigh.x, sum_r.y * md.rayleigh.y, sum_r.z * md.rayleigh.z} * phase_r +
                    sum_m * (md.mie_s * phase_m)) * (e_sun * (std::isfinite(s.sky_gain) ? std::max(s.sky_gain, 0.0f) : 1.0f));
    if (t_ground > 0.0f) { // lit Lambertian ground seen through the air in front of it
        const math::Vec3 p = o + d * t_ground;
        const math::Vec3 n = math::normalize(p);
        const float cos_sun = std::max(math::dot(n, sun), 0.0f);
        float lr = 0, lm = 0;
        if (cos_sun > 0.0f && light_depth(p + n * 1.0f, sun, lr, lm)) {
            const math::Vec3 sun_t = extinction(md, lr, lm);
            const math::Vec3 view_t = extinction(md, od_r, od_m);
            const math::Vec3 a = s.ground_albedo;
            l += math::Vec3{a.x * sun_t.x * view_t.x, a.y * sun_t.y * view_t.y, a.z * sun_t.z * view_t.z} *
                 (e_sun * cos_sun / kPi);
        }
    }
    return {std::isfinite(l.x) ? l.x : 0.0f, std::isfinite(l.y) ? l.y : 0.0f, std::isfinite(l.z) ? l.z : 0.0f};
}

ImageF render_atmosphere(const AtmosphereSettings& s) {
    ImageF img;
    img.width = std::clamp(s.width, 16u, 1024u) & ~1u;
    img.height = img.width / 2;
    img.pixels.resize(std::size_t{img.width} * img.height * 4);
    for (std::uint32_t y = 0; y < img.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(img.height);
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(img.width);
            const math::Vec3 c = atmosphere_radiance(s, equirect_direction(u, v));
            float* p = &img.pixels[(std::size_t{y} * img.width + x) * 4];
            p[0] = std::min(c.x, 65000.0f);
            p[1] = std::min(c.y, 65000.0f);
            p[2] = std::min(c.z, 65000.0f);
            p[3] = 1.0f;
        }
    }
    return img;
}

} // namespace victus::render
