// SPDX-License-Identifier: Apache-2.0
// Physical sky: blue zenith, brighter horizon, red sunsets, dark nights, and cheap to evaluate.
#include "doctest.h"
#include "victus/render/atmosphere.hpp"
#include "victus/render/environment_map.hpp"

#include <chrono>

using namespace victus;
using namespace victus::render;

TEST_CASE("atmosphere: noon sky is blue overhead and the ground is lit") {
    AtmosphereSettings s;
    s.sun_direction = {0.2f, 1.0f, 0.1f};
    s.sun_illuminance = 3.0f;
    const math::Vec3 zenith = atmosphere_radiance(s, {0, 1, 0});
    const math::Vec3 horizon = atmosphere_radiance(s, {1, 0.02f, 0});
    const math::Vec3 ground = atmosphere_radiance(s, {0, -1, 0});
    MESSAGE("zenith " << zenith.x << "," << zenith.y << "," << zenith.z << " horizon " << horizon.x << ","
                      << horizon.y << "," << horizon.z << " ground " << ground.x);
    CHECK(zenith.z > zenith.y);
    CHECK(zenith.y > zenith.x);
    CHECK(horizon.x + horizon.y + horizon.z > zenith.x + zenith.y + zenith.z); // brighter, whiter horizon
    CHECK(horizon.x / horizon.z > zenith.x / zenith.z);
    CHECK(ground.x > 0.15f); // albedo 0.3 under a high sun: ~0.3 * 3 * cos / pi
    CHECK(ground.x < 0.35f);
    CHECK(zenith.z > 0.3f); // calibrated to read like the analytic default sky
    const math::Vec3 t = atmosphere_sun_transmittance(s);
    CHECK(t.x > 0.8f);
    CHECK(t.z > 0.6f);
    CHECK(t.x > t.z);
}

TEST_CASE("atmosphere: sunsets redden the sun and night is dark") {
    AtmosphereSettings s;
    s.sun_illuminance = 3.0f;
    s.sun_direction = {1.0f, 0.03f, 0.0f};
    const math::Vec3 t = atmosphere_sun_transmittance(s);
    MESSAGE("sunset transmittance " << t.x << "," << t.y << "," << t.z);
    CHECK(t.x > 2.0f * t.z);
    const math::Vec3 near_sun = atmosphere_radiance(s, {1.0f, 0.05f, 0.0f});
    CHECK(near_sun.x > near_sun.z);
    s.sun_direction = {1.0f, -0.4f, 0.0f};
    CHECK(atmosphere_sun_transmittance(s).x == 0.0f);
    const math::Vec3 night = atmosphere_radiance(s, {0, 1, 0});
    CHECK(night.x + night.y + night.z < 1e-3f);
    // Haze whitens the sky; no air means a black sky.
    AtmosphereSettings clear;
    clear.sun_illuminance = 3.0f;
    clear.sun_direction = {0, 1, 0};
    AtmosphereSettings hazy = clear;
    hazy.mie_scale = 20.0f;
    const math::Vec3 c = atmosphere_radiance(clear, {1, 0.3f, 0}), h = atmosphere_radiance(hazy, {1, 0.3f, 0});
    CHECK(h.x / h.z > c.x / c.z);
    AtmosphereSettings vacuum = clear;
    vacuum.rayleigh_scale = 0.0f;
    vacuum.mie_scale = 0.0f;
    const math::Vec3 v = atmosphere_radiance(vacuum, {0, 1, 0});
    CHECK(v.x + v.y + v.z == doctest::Approx(0.0f));
}

TEST_CASE("atmosphere: equirect bake, change detection and cost") {
    AtmosphereSettings s;
    s.sun_illuminance = 3.0f;
    const auto t0 = std::chrono::steady_clock::now();
    const ImageF img = render_atmosphere(s);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("128x64 atmosphere in " << ms << " ms");
    CHECK(img.width == 128);
    CHECK(img.height == 64);
    const auto sh = SphericalHarmonics9::project(img);
    CHECK(sh.diffuse({0, 1, 0}).z > sh.diffuse({0, 1, 0}).x); // blue skylight from above
    AtmosphereSettings moved = s;
    CHECK(moved.same_as(s));
    moved.sun_direction = {0.3f, 0.801f, -0.5f}; // a tiny move is ignored
    CHECK(moved.same_as(s));
    moved.sun_direction = {0.3f, 0.6f, -0.5f};
    CHECK_FALSE(moved.same_as(s));
    moved = s;
    moved.mie_scale = 2.0f;
    CHECK_FALSE(moved.same_as(s));
}
