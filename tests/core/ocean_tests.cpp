// SPDX-License-Identifier: Apache-2.0
// FFT ocean (ADR-0261): the FFT itself, spectrum determinism and scaling, dispersion, choppy
// crests, periodic sampling, the inverse-displacement height query and normals.
#include "victus/core/ocean.hpp"

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

using namespace victus;
using namespace victus::ocean;

namespace {

double rms_height(const Field& f) {
    double sum = 0;
    for (const math::Vec4& d : f.displacement) sum += static_cast<double>(d.y) * d.y;
    return std::sqrt(sum / static_cast<double>(f.displacement.size()));
}

SpectrumDesc sea(float choppiness = 1.0f) {
    SpectrumDesc d;
    d.resolution = 64;
    d.tile_size = 64;
    d.wave_height = 2.0f;
    d.peak_wavelength = 16;
    d.direction = 0.4f;
    d.choppiness = choppiness;
    d.seed = 7;
    return d;
}

} // namespace

TEST_CASE("ocean fft: impulse, single tone and round trip") {
    std::vector<std::complex<float>> v(16);
    v[0] = 1;
    fft(v, false);
    for (const auto& c : v) CHECK(std::abs(c - std::complex<float>(1, 0)) < 1e-6f);
    // A single frequency bin inverse-transforms into e^{i 2 pi 3 n / 16}.
    std::vector<std::complex<float>> tone(16);
    tone[3] = 1;
    fft(tone, true);
    for (std::size_t n = 0; n < 16; ++n) {
        const double a = 2.0 * std::numbers::pi * 3.0 * static_cast<double>(n) / 16.0;
        CHECK(std::abs(tone[n] - std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)))) < 1e-5f);
    }
    std::vector<std::complex<float>> x(64), y;
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = {std::sin(0.3f * static_cast<float>(i)), 0.1f * static_cast<float>(i % 7)};
    y = x;
    fft(y, false);
    fft(y, true);
    for (std::size_t i = 0; i < x.size(); ++i) CHECK(std::abs(y[i] / 64.0f - x[i]) < 1e-5f);
}

TEST_CASE("ocean spectrum: deterministic per seed, scaled to the significant wave height") {
    const Spectrum a(sea()), b(sea());
    SpectrumDesc other = sea();
    other.seed = 8;
    const Spectrum c(other);
    const Field fa = a.evaluate(3.0), fb = b.evaluate(3.0), fc = c.evaluate(3.0);
    REQUIRE(fa.n == 64);
    REQUIRE(fa.displacement.size() == 64u * 64u);
    bool same = true, differ = false;
    for (std::size_t i = 0; i < fa.displacement.size(); ++i) {
        same = same && fa.displacement[i].y == fb.displacement[i].y && fa.normal[i].w == fb.normal[i].w;
        differ = differ || std::abs(fa.displacement[i].y - fc.displacement[i].y) > 1e-3f;
    }
    CHECK(same);
    CHECK(differ);
    CHECK(fa.revision != fb.revision);
    // 4 x RMS height equals the requested height at t = 0 (by construction) and stays close later.
    CHECK(4.0 * rms_height(a.evaluate(0.0)) == doctest::Approx(2.0).epsilon(1e-3));
    CHECK(4.0 * rms_height(fa) == doctest::Approx(2.0).epsilon(0.25));
    CHECK(fa.max_height > 1.0f);
    CHECK(fa.max_height < 4.0f);
    // A sea with no wave height is flat.
    SpectrumDesc calm = sea();
    calm.wave_height = 0;
    const Field flat = Spectrum(calm).evaluate(1.0);
    CHECK(flat.max_height == 0.0f);
    CHECK(flat.max_horizontal == 0.0f);
}

TEST_CASE("ocean spectrum: waves travel, speed 0 freezes them, settings are sanitised") {
    const Spectrum s(sea());
    const Field f0 = s.evaluate(0.0), f1 = s.evaluate(1.0);
    double diff = 0;
    for (std::size_t i = 0; i < f0.displacement.size(); ++i) diff += std::abs(f0.displacement[i].y - f1.displacement[i].y);
    CHECK(diff / static_cast<double>(f0.displacement.size()) > 0.05);
    SpectrumDesc frozen = sea();
    frozen.speed = 0;
    const Spectrum z(frozen);
    const Field z0 = z.evaluate(0.0), z1 = z.evaluate(50.0);
    for (std::size_t i = 0; i < z0.displacement.size(); i += 97) CHECK(z0.displacement[i].y == z1.displacement[i].y);
    // Very long times keep their phase precision (computed in double).
    const Field late = s.evaluate(1.0e6);
    CHECK(std::isfinite(late.max_height));
    CHECK(4.0 * rms_height(late) == doctest::Approx(2.0).epsilon(0.35));

    SpectrumDesc bad;
    bad.resolution = 100;
    bad.tile_size = std::nanf("");
    bad.choppiness = 9;
    const SpectrumDesc ok = sanitize(bad);
    CHECK(ok.resolution == 64);
    CHECK(ok.tile_size == 100.0f);
    CHECK(ok.choppiness == 2.0f);
    CHECK(sanitize(SpectrumDesc{.resolution = 4}).resolution == 16);
    CHECK(sanitize(SpectrumDesc{.resolution = 4096}).resolution == 256);
}

TEST_CASE("ocean spectrum: choppiness sharpens crests (Jacobian below 1 where the sea is high)") {
    const Field smooth = Spectrum(sea(0.0f)).evaluate(2.0);
    for (std::size_t i = 0; i < smooth.normal.size(); i += 61) {
        CHECK(smooth.normal[i].w == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(smooth.displacement[i].x == 0.0f);
    }
    CHECK(smooth.max_horizontal == 0.0f);
    const Field choppy = Spectrum(sea(1.5f)).evaluate(2.0);
    CHECK(choppy.max_horizontal > 0.2f);
    // Crests (high points) compress, troughs stretch: the mean Jacobian of the highest tenth of
    // texels is below that of the lowest tenth. With the sign of the displacement flipped the
    // crests would stretch instead.
    std::vector<std::pair<float, float>> hj;
    for (std::size_t i = 0; i < choppy.displacement.size(); ++i) hj.emplace_back(choppy.displacement[i].y, choppy.normal[i].w);
    std::sort(hj.begin(), hj.end());
    const std::size_t tenth = hj.size() / 10;
    double low = 0, high = 0;
    for (std::size_t i = 0; i < tenth; ++i) {
        low += hj[i].second;
        high += hj[hj.size() - 1 - i].second;
    }
    MESSAGE("mean Jacobian: crests " << high / tenth << ", troughs " << low / tenth);
    CHECK(high / tenth < 0.95);
    CHECK(low / tenth > 1.05);
}

TEST_CASE("ocean field: periodic lookups, height above a point and normals") {
    const Field f = Spectrum(sea(1.0f)).evaluate(4.0);
    const math::Vec2 p{3.3f, -7.9f};
    const math::Vec4 a = displacement_at(f, p), b = displacement_at(f, {p.x + 64.0f, p.y - 128.0f});
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-3));
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-3));
    // Texel centres return the stored values exactly.
    CHECK(displacement_at(f, {1.0f * 5, 1.0f * 9}).y == doctest::Approx(f.displacement[9 * 64 + 5].y));
    // The surface sampled above the displaced position of a rest point has that point's height.
    double worst = 0;
    for (int i = 0; i < 40; ++i) {
        const math::Vec2 p0{static_cast<float>(i) * 1.37f - 20.0f, static_cast<float>(i * 7 % 40) * 0.91f - 15.0f};
        const math::Vec4 d = displacement_at(f, p0);
        const Sample s = sample(f, {p0.x + d.x, p0.y + d.z});
        worst = std::max(worst, static_cast<double>(std::abs(s.height - d.y)));
        CHECK(math::length(s.normal) == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(s.normal.y > 0.0f);
    }
    MESSAGE("worst height error after inversion: " << worst << " m");
    CHECK(worst < 0.05);
    // Without choppiness the normal is the height gradient's: compare with central differences.
    const Field g = Spectrum(sea(0.0f)).evaluate(4.0);
    const float texel = 1.0f;
    double worst_n = 0, mean_n = 0;
    int count = 0;
    for (std::uint32_t j = 3; j < 64; j += 9)
        for (std::uint32_t i = 2; i < 64; i += 11) {
            auto h = [&](std::uint32_t x, std::uint32_t z) { return g.displacement[(z % 64) * 64 + (x % 64)].y; };
            const float dx = (h(i + 1, j) - h(i + 63, j)) / (2 * texel), dz = (h(i, j + 1) - h(i, j + 63)) / (2 * texel);
            const math::Vec3 fd = math::normalize(math::Vec3{-dx, 1, -dz});
            const math::Vec4 nw = g.normal[j * 64 + i];
            const double dev = 1.0 - static_cast<double>(math::dot(fd, {nw.x, nw.y, nw.z}));
            worst_n = std::max(worst_n, dev);
            mean_n += dev;
            ++count;
        }
    mean_n /= count;
    // Central differences over one texel miss part of the shortest waves, so they agree with the
    // spectral derivative only approximately.
    MESSAGE("normal deviation from finite differences (1 - cos): mean " << mean_n << ", worst " << worst_n);
    CHECK(mean_n < 0.01);
    CHECK(worst_n < 0.05);
    // An empty field is flat.
    const Sample none = sample(Field{}, {1, 2});
    CHECK(none.height == 0.0f);
    CHECK(none.normal.y == 1.0f);
}
