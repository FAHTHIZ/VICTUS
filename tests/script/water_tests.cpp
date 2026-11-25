// SPDX-License-Identifier: Apache-2.0
// Gerstner water volumes (ADR-0095): wave set construction, the inverse-displacement height
// query, normals, volume bounds, the water clock and reflection.
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/water.hpp"
#include "victus/scene/reflect.hpp"

#include "doctest.h"

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::gameplay;
using scene::Entity;
using scene::Scene;

namespace {

// Forward Gerstner displacement of grid point p0 (what water.vert computes).
math::Vec3 displaced(const WaterWaves& waves, math::Vec2 p0) {
    math::Vec3 p{p0.x, 0, p0.y};
    for (const WaterWave& w : waves) {
        const float t = w.k * (w.direction.x * p0.x + w.direction.y * p0.y) - w.phase;
        p.x += w.q * w.amplitude * w.direction.x * std::cos(t);
        p.z += w.q * w.amplitude * w.direction.y * std::cos(t);
        p.y += w.amplitude * std::sin(t);
    }
    return p;
}

} // namespace

TEST_CASE("water waves: dispersion, amplitude split and wrapped phase") {
    WaterComponent c;
    c.wave_height = 1.0f;
    c.wavelength = 10.0f;
    c.wave_direction = 90.0f;
    const WaterWaves w = water_waves(c, 0.0);
    CHECK(w[0].direction.x == doctest::Approx(0.0f).epsilon(1e-6).scale(1));
    CHECK(w[0].direction.y == doctest::Approx(1.0f));
    CHECK(w[0].k == doctest::Approx(2.0f * 3.14159265f / 10.0f));
    CHECK(w[0].omega == doctest::Approx(std::sqrt(9.81f * w[0].k)));
    CHECK(w[0].amplitude == doctest::Approx(0.5f));
    float steep = 0;
    for (const WaterWave& x : w) {
        steep += x.q * x.k * x.amplitude;
        CHECK(x.phase == doctest::Approx(0.0f));
    }
    CHECK(steep == doctest::Approx(c.steepness)); // sum Q k A = steepness: no loops
    // A day later the phase is still well conditioned.
    const WaterWaves late = water_waves(c, 86400.0 * 3);
    for (const WaterWave& x : late) {
        CHECK(x.phase >= 0.0f);
        CHECK(x.phase < 6.2832f);
    }
    WaterComponent bad;
    bad.wavelength = NAN;
    bad.wave_height = -3;
    for (const WaterWave& x : water_waves(bad, 1.0)) {
        CHECK(std::isfinite(x.k));
        CHECK(x.amplitude == 0.0f);
        CHECK(x.q == 0.0f);
    }
}

TEST_CASE("water waves: sampled height matches the displaced grid, normals are unit") {
    WaterComponent c;
    c.wave_height = 0.8f;
    c.wavelength = 8.0f;
    c.steepness = 0.7f;
    const WaterWaves w = water_waves(c, 3.7);
    for (int i = 0; i < 40; ++i) {
        const math::Vec2 p0{static_cast<float>(i) * 0.37f - 7.0f, static_cast<float>(i % 7) * 1.3f - 4.0f};
        const math::Vec3 p = displaced(w, p0);
        const WaveSample s = sample_waves(w, {p.x, p.z});
        CHECK(s.height == doctest::Approx(p.y).epsilon(0.02).scale(1)); // within 2 cm
        CHECK(math::length(s.normal) == doctest::Approx(1.0f));
        CHECK(s.normal.y > 0.3f);
        CHECK(s.jacobian <= 1.0f + c.steepness);
    }
    WaterComponent flat;
    flat.wave_height = 0;
    const WaveSample f = sample_waves(water_waves(flat, 2.0), {3, 4});
    CHECK(f.height == 0.0f);
    CHECK(f.normal.y == doctest::Approx(1.0f));
}

TEST_CASE("water volumes: query bounds, nearest surface, current and the clock") {
    register_components();
    Scene s;
    const Entity pool = s.create("Pool");
    s.world().get<scene::Transform>(pool)->position = {10, 2, 0};
    auto& w = s.world().add<WaterComponent>(pool);
    w.size = {6, 4};
    w.wave_height = 0;
    w.current = {1, -2};
    CHECK_FALSE(query_water(s, {0, 0, 0}).has_value()); // outside in X
    CHECK_FALSE(query_water(s, {10, 0, 2.5f}).has_value()); // outside in Z
    const auto q = query_water(s, {11, 0.5f, 1});
    REQUIRE(q.has_value());
    CHECK(q->water == pool);
    CHECK(q->surface == doctest::Approx(2.0f));
    CHECK(q->depth == doctest::Approx(1.5f));
    CHECK(q->current.x == doctest::Approx(1.0f));
    CHECK(q->current.z == doctest::Approx(-2.0f));
    // Above the surface: still reported with a negative depth.
    CHECK(query_water(s, {10, 3, 0})->depth == doctest::Approx(-1.0f));

    // A second, lower pool in the same column: the one containing the point wins.
    const Entity low = s.create("Low");
    s.world().get<scene::Transform>(low)->position = {10, -5, 0};
    s.world().add<WaterComponent>(low).wave_height = 0;
    CHECK(query_water(s, {10, 0, 0})->water == pool);
    CHECK(query_water(s, {10, -6, 0})->water == low);
    CHECK(water_volumes(s).size() == 2);

    CHECK(water_time(s) == 0.0);
    advance_water_time(s, 0.5f);
    advance_water_time(s, -1.0f);
    advance_water_time(s, NAN);
    CHECK(water_time(s) == doctest::Approx(0.5));
    set_water_time(s, 100.0);
    CHECK(water_time(s) == doctest::Approx(100.0));

    Simulation sim;
    add_builtin_systems(sim);
    for (int i = 0; i < 60; ++i) sim.tick(s, 1.0f / 60);
    CHECK(water_time(s) == doctest::Approx(101.0).epsilon(1e-3));
}

TEST_CASE("water component: reflected and saved") {
    register_components();
    Scene s;
    const Entity e = s.create("Sea");
    auto& w = s.world().add<WaterComponent>(e);
    w.wave_height = 1.25f;
    w.density = 1025.0f;
    w.shallow_color = {0.1f, 0.5f, 0.4f, 1};
    const auto doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* w2 = t.world().get<WaterComponent>(t.find_by_name("Sea"));
    REQUIRE(w2 != nullptr);
    CHECK(w2->wave_height == doctest::Approx(1.25f));
    CHECK(w2->density == doctest::Approx(1025.0f));
    CHECK(w2->shallow_color.g == doctest::Approx(0.5f));
}

TEST_CASE("water spectrum: volumes carry the ocean tile, queries and saves use it (ADR-0261)") {
    register_components();
    Scene s;
    const Entity e = s.create("Sea");
    auto& w = s.world().add<WaterComponent>(e);
    w.size = {200, 200};
    w.waves = WaveModel::Spectrum;
    w.wave_height = 1.5f;
    w.wavelength = 10.0f;
    w.spectrum_tile = 48.0f;
    w.spectrum_resolution = 32;
    w.spectrum_seed = 3;
    set_water_time(s, 5.0);
    const auto vols = water_volumes(s);
    REQUIRE(vols.size() == 1);
    REQUIRE(vols[0].ocean != nullptr);
    CHECK(vols[0].ocean->n == 32);
    CHECK(vols[0].ocean->tile_size == doctest::Approx(48.0f));
    CHECK(vols[0].max_height == doctest::Approx(vols[0].ocean->max_height));
    for (const WaterWave& wave : vols[0].waves) CHECK(wave.amplitude == 0.0f);
    // The same time reuses the cached tile; a new time or new settings evaluate again.
    CHECK(water_volumes(s)[0].ocean == vols[0].ocean);
    CHECK(water_ocean(s, e, w, 5.0) == vols[0].ocean);
    const auto later = water_ocean(s, e, w, 6.0);
    CHECK(later != vols[0].ocean);
    // A query reports the field's surface and normal.
    const auto q = query_water(s, {4.0f, -20.0f, 7.0f});
    REQUIRE(q.has_value());
    const ocean::Sample expect = ocean::sample(*vols[0].ocean, {4.0f, 7.0f});
    CHECK(q->surface == doctest::Approx(expect.height));
    CHECK(q->normal.y == doctest::Approx(expect.normal.y));
    CHECK(q->depth == doctest::Approx(expect.height + 20.0f));
    // Settings map onto the spectrum: steepness doubles into choppiness, degrees into radians.
    w.steepness = 0.25f;
    w.wave_direction = 90.0f;
    const ocean::SpectrumDesc d = spectrum_desc(w);
    CHECK(d.choppiness == doctest::Approx(0.5f));
    CHECK(d.direction == doctest::Approx(std::numbers::pi_v<float> / 2));
    CHECK(d.peak_wavelength == doctest::Approx(10.0f));
    CHECK(water_ocean(s, e, w, 6.0) != later);
    // Gerstner water has no tile.
    w.waves = WaveModel::Gerstner;
    CHECK(water_ocean(s, e, w, 6.0) == nullptr);
    CHECK(water_volumes(s)[0].ocean == nullptr);
    w.waves = WaveModel::Spectrum;

    const auto doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* w2 = t.world().get<WaterComponent>(t.find_by_name("Sea"));
    REQUIRE(w2 != nullptr);
    CHECK(w2->waves == WaveModel::Spectrum);
    CHECK(w2->spectrum_tile == doctest::Approx(48.0f));
    CHECK(w2->spectrum_resolution == 32);
    CHECK(w2->spectrum_seed == 3);
}
