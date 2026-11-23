// SPDX-License-Identifier: Apache-2.0
// CPU side of image-based lighting: HDR image I/O, half floats, equirect mapping and SH9.
#include "doctest.h"
#include "victus/render/environment_map.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace victus;
using namespace victus::render;

namespace {
ImageF make_env(std::uint32_t w, std::uint32_t h, math::Vec3 top, math::Vec3 bottom) {
    ImageF img;
    img.width = w;
    img.height = h;
    img.pixels.resize(std::size_t{w} * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const math::Vec3 c = y < h / 2 ? top : bottom;
            float* p = &img.pixels[(std::size_t{y} * w + x) * 4];
            p[0] = c.x;
            p[1] = c.y;
            p[2] = c.z;
            p[3] = 1;
        }
    return img;
}
} // namespace

TEST_CASE("half floats round trip and clamp") {
    for (float f : {0.0f, 1.0f, -2.5f, 0.333f, 1000.0f, 65504.0f, 6.1e-5f}) {
        const float back = half_to_float(float_to_half(f));
        CHECK(back == doctest::Approx(f).epsilon(0.001));
    }
    CHECK(half_to_float(float_to_half(1e9f)) == doctest::Approx(65504.0f));
    CHECK(half_to_float(float_to_half(-0.0f)) == 0.0f);
}

TEST_CASE("radiance HDR encode/decode round trip") {
    ImageF img = make_env(8, 4, {4.0f, 0.5f, 0.25f}, {0.01f, 0.02f, 12.0f});
    auto bytes = encode_hdr(img);
    REQUIRE(bytes.ok());
    CHECK(is_hdr_image(*bytes));
    const std::uint8_t png_sig[] = {0x89, 'P', 'N', 'G'};
    CHECK_FALSE(is_hdr_image(png_sig));
    auto back = decode_image_hdr(*bytes);
    REQUIRE(back.ok());
    REQUIRE(back->width == 8);
    REQUIRE(back->height == 4);
    // RGBE shares one exponent per pixel: error is relative to the brightest channel.
    for (std::size_t i = 0; i < img.pixels.size(); ++i) {
        if (i % 4 == 3) continue;
        const std::size_t px = i - i % 4;
        const float peak = std::max({img.pixels[px], img.pixels[px + 1], img.pixels[px + 2]});
        CHECK(std::abs(back->pixels[i] - img.pixels[i]) <= peak / 64.0f);
    }
    const ImageF small = downsample_to_fit(img, 4, 2);
    CHECK(small.width == 4);
    CHECK(small.height == 2);
    CHECK(small.pixels[0] == doctest::Approx(4.0f));
}

TEST_CASE("equirect mapping is invertible and oriented") {
    const math::Vec3 fwd = equirect_direction(0.5f, 0.5f);
    CHECK(fwd.z == doctest::Approx(-1.0f).epsilon(0.001));
    const math::Vec3 right = equirect_direction(0.75f, 0.5f);
    CHECK(right.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(equirect_direction(0.3f, 0.0f).y == doctest::Approx(1.0f).epsilon(0.001));
    for (float u : {0.1f, 0.4f, 0.6f, 0.9f})
        for (float v : {0.2f, 0.5f, 0.8f}) {
            const math::Vec2 uv = equirect_uv(equirect_direction(u, v));
            CHECK(uv.x == doctest::Approx(u).epsilon(0.001));
            CHECK(uv.y == doctest::Approx(v).epsilon(0.001));
        }
}

TEST_CASE("SH9 irradiance of constant and split environments") {
    const auto flat = SphericalHarmonics9::project(make_env(64, 32, {0.5f, 1.0f, 2.0f}, {0.5f, 1.0f, 2.0f}));
    for (math::Vec3 n : {math::Vec3{0, 1, 0}, math::Vec3{0, -1, 0}, math::Vec3{1, 0, 0}, math::Vec3{0, 0, -1}}) {
        const math::Vec3 d = flat.diffuse(n);
        CHECK(d.x == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(d.y == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(d.z == doctest::Approx(2.0f).epsilon(0.02));
    }
    CHECK(flat.average().y == doctest::Approx(1.0f).epsilon(0.02));
    const auto split = SphericalHarmonics9::project(make_env(64, 32, {3, 3, 3}, {0, 0, 0}));
    const math::Vec3 up = split.diffuse({0, 1, 0}), down = split.diffuse({0, -1, 0}), side = split.diffuse({1, 0, 0});
    CHECK(up.x > 2.5f);
    CHECK(down.x < 0.5f);
    CHECK(side.x == doctest::Approx(1.5f).epsilon(0.05)); // half the sky visible
}

TEST_CASE("sun extraction clamps a photographed sun and reports its irradiance") {
    // A uniform sky of radiance 1 with a 3x3-texel sun of radiance 20000 at 45 degrees elevation.
    ImageF img = make_env(256, 128, {1, 1, 1}, {0.2f, 0.2f, 0.2f});
    const math::Vec3 sun_dir = math::normalize(math::Vec3{0.0f, 1.0f, -1.0f});
    const math::Vec2 uv = equirect_uv(sun_dir);
    const auto cx = static_cast<std::uint32_t>(uv.x * 256.0f), cy = static_cast<std::uint32_t>(uv.y * 128.0f);
    double expected = 0.0;
    for (std::uint32_t y = cy - 1; y <= cy + 1; ++y)
        for (std::uint32_t x = cx - 1; x <= cx + 1; ++x) {
            float* p = &img.pixels[(std::size_t{y} * 256 + x) * 4];
            p[0] = p[1] = p[2] = 20000.0f;
            const float v = (static_cast<float>(y) + 0.5f) / 128.0f;
            expected += 2.0 * 3.14159265 / 256.0 * 3.14159265 / 128.0 * std::sin(v * 3.14159265);
        }
    const SphericalHarmonics9 before = SphericalHarmonics9::project(img);
    const ExtractedSun sun = extract_sun(img);
    REQUIRE(sun.found);
    CHECK(sun.texels == 9);
    CHECK(math::dot(sun.direction, sun_dir) > 0.999f);
    // Nearly all of the sun's energy is removed: (20000 - threshold) per texel.
    CHECK(sun.irradiance.x == doctest::Approx(expected * (20000.0 - sun.threshold)).epsilon(0.01));
    CHECK(sun.threshold < 50.0f);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4) CHECK(img.pixels[i] <= sun.threshold + 1e-3f);
    // The sky keeps its colour and the irradiance below the sun drops to about the sky's own.
    const SphericalHarmonics9 after = SphericalHarmonics9::project(img);
    CHECK(before.diffuse(sun_dir).x > 2.0f * after.diffuse(sun_dir).x);
    CHECK(after.diffuse({0, 1, 0}).x == doctest::Approx(1.0f).epsilon(0.05));
}

TEST_CASE("sun extraction leaves skies without a sun unchanged") {
    ImageF overcast = make_env(64, 32, {1.2f, 1.2f, 1.25f}, {0.3f, 0.3f, 0.3f});
    overcast.pixels[(5 * 64 + 7) * 4] = 40.0f; // a bright cloud edge is not a sun
    const std::vector<float> original = overcast.pixels;
    const ExtractedSun sun = extract_sun(overcast);
    CHECK_FALSE(sun.found);
    CHECK(sun.texels == 0);
    CHECK(overcast.pixels == original);
    ImageF empty;
    CHECK_FALSE(extract_sun(empty).found);
}
