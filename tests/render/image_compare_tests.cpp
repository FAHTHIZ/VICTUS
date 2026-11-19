// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/image_compare.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace victus;
using namespace victus::render;

namespace {
// Smooth gradient with some structure (a soft ring), so blur and noise change it measurably.
Image scene(std::uint32_t w, std::uint32_t h, float phase = 0) {
    Image img{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx = float(x) / float(w), fy = float(y) / float(h);
            const float ring = 0.5f + 0.5f * std::sin(40.0f * std::hypot(fx - 0.5f, fy - 0.5f) + phase);
            const std::size_t i = (std::size_t(y) * w + x) * 4;
            img.pixels[i] = static_cast<std::uint8_t>(40 + 160 * fx);
            img.pixels[i + 1] = static_cast<std::uint8_t>(30 + 180 * ring);
            img.pixels[i + 2] = static_cast<std::uint8_t>(60 + 120 * fy);
            img.pixels[i + 3] = 255;
        }
    return img;
}

Image noisy(const Image& src, int amplitude, std::uint32_t seed = 1) {
    Image out = src;
    std::uint32_t s = seed;
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        if (i % 4 == 3) continue;
        s = s * 1664525u + 1013904223u;
        const int n = int((s >> 16) % std::uint32_t(2 * amplitude + 1)) - amplitude;
        out.pixels[i] = static_cast<std::uint8_t>(std::clamp(int(out.pixels[i]) + n, 0, 255));
    }
    return out;
}

Image box_blur(const Image& src) {
    Image out = src;
    const int w = int(src.width), h = int(src.height);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                int sum = 0, n = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                        sum += src.pixels[(std::size_t(yy) * w + xx) * 4 + c];
                        ++n;
                    }
                out.pixels[(std::size_t(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(sum / n);
            }
    return out;
}
} // namespace

TEST_CASE("identical images compare as identical") {
    const Image a = scene(96, 64);
    auto r = compare_images(a, a, {.keep_maps = true});
    REQUIRE(r.ok());
    CHECK(r->flip_mean == doctest::Approx(0.0));
    CHECK(r->flip_max == doctest::Approx(0.0));
    CHECK(r->ssim == doctest::Approx(1.0));
    CHECK(r->ssim_worst_tile == doctest::Approx(1.0));
    CHECK(std::isinf(r->psnr));
    CHECK(r->visible_fraction == 0.0);
    const Image heat = flip_heatmap(*r);
    CHECK(heat.width == 96);
    CHECK(heat.height == 64);
    CHECK(heat.pixels.size() == std::size_t(96) * 64 * 4);
}

TEST_CASE("noise and blur lower SSIM and raise FLIP, more distortion scores worse") {
    const Image a = scene(128, 96);
    auto light = compare_images(a, noisy(a, 4));
    auto heavy = compare_images(a, noisy(a, 40));
    auto blur = compare_images(a, box_blur(a));
    REQUIRE(light.ok());
    REQUIRE(heavy.ok());
    REQUIRE(blur.ok());
    CHECK(light->ssim < 1.0);
    CHECK(heavy->ssim < light->ssim);
    CHECK(heavy->flip_mean > light->flip_mean);
    CHECK(heavy->psnr < light->psnr);
    CHECK(blur->ssim < 0.99);
    CHECK(blur->flip_mean > 0.0);
}

TEST_CASE("a small local defect shows in the worst tile while the mean stays low") {
    const Image a = scene(256, 256);
    Image b = a;
    // A 16x16 magenta patch at (200, 40): one broken "face" in an otherwise correct frame.
    for (std::uint32_t y = 40; y < 56; ++y)
        for (std::uint32_t x = 200; x < 216; ++x) {
            const std::size_t i = (std::size_t(y) * 256 + x) * 4;
            b.pixels[i] = 255;
            b.pixels[i + 1] = 0;
            b.pixels[i + 2] = 255;
        }
    auto r = compare_images(a, b, {.tile = 64});
    REQUIRE(r.ok());
    CHECK(r->flip_mean < 0.01);           // the average hides it...
    CHECK(r->flip_worst_tile > 0.03);     // ...the worst tile does not
    CHECK(r->flip_worst_tile > 5 * r->flip_mean);
    CHECK(r->ssim_worst_tile < r->ssim);
    CHECK(r->worst_tile_x <= 200);
    CHECK(r->worst_tile_x + r->worst_tile_w >= 216);
    CHECK(r->worst_tile_y <= 40);
    CHECK(r->worst_tile_y + r->worst_tile_h >= 56);
    CHECK(r->visible_fraction > 0.0);
    CHECK(r->visible_fraction < 0.02);

    CompareLimits limits;
    limits.max_flip_mean = 0.05;
    SequenceComparison seq;
    seq.frames.push_back(*r);
    seq.flip_worst_frame = r->flip_mean;
    seq.flip_worst_tile = r->flip_worst_tile;
    seq.ssim_worst_tile = r->ssim_worst_tile;
    CHECK(check_limits(seq, limits).empty());
    limits.max_flip_tile = 0.02;
    CHECK(check_limits(seq, limits).size() == 1);
}

TEST_CASE("sequence comparison catches flicker that per-frame means miss") {
    std::vector<Image> ref, steady, flicker;
    for (int f = 0; f < 6; ++f) {
        ref.push_back(scene(64, 48, 0.2f * float(f)));
        steady.push_back(ref.back());
        // Alternate small brightness offsets: each frame alone is only slightly off.
        Image b = ref.back();
        const int d = (f % 2) ? 6 : -6;
        for (std::size_t i = 0; i < b.pixels.size(); ++i)
            if (i % 4 != 3) b.pixels[i] = static_cast<std::uint8_t>(std::clamp(int(b.pixels[i]) + d, 0, 255));
        flicker.push_back(std::move(b));
    }
    auto same = compare_sequences(ref, steady);
    auto bad = compare_sequences(ref, flicker);
    REQUIRE(same.ok());
    REQUIRE(bad.ok());
    CHECK(same->temporal_error == doctest::Approx(0.0));
    CHECK(same->flip_mean == doctest::Approx(0.0));
    CHECK(bad->temporal_error > 0.005);
    CHECK(bad->temporal_worst_frame >= 1);
    CHECK(bad->frames.size() == 6);
    CompareLimits limits;
    limits.max_temporal = 0.002;
    CHECK(check_limits(*same, limits).empty());
    CHECK_FALSE(check_limits(*bad, limits).empty());
}

TEST_CASE("comparison rejects mismatched inputs") {
    CHECK_FALSE(compare_images(scene(32, 32), scene(33, 32)).ok());
    CHECK_FALSE(compare_images(scene(8, 8), scene(8, 8)).ok());
    std::vector<Image> one{scene(32, 32)}, two{scene(32, 32), scene(32, 32)};
    CHECK_FALSE(compare_sequences(one, two).ok());
    CHECK(pixels_per_degree(0.7f, 3840.0f, 0.7f) == doctest::Approx(67.02).epsilon(0.01));
}
