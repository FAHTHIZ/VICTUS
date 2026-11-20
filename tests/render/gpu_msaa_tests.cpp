// SPDX-License-Identifier: Apache-2.0
// MSAA and alpha to coverage (ADR-0152). Geometry edges and alpha-tested cut-outs rendered at
// 1x and with 4x MSAA are compared against a 4x4 supersampled reference (rendered at four times
// the resolution and box-filtered down): MSAA must cut the edge error, and alpha to coverage
// must cut the cut-out error further than MSAA alone (MSAA does not touch a discard edge).
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 128, H = 80, SS = 4;

// Discs ("leaves") of smooth alpha: bilinear filtering gives the cut-out a soft alpha ramp.
Image discs(std::uint32_t size) {
    Image img;
    img.width = img.height = size;
    img.pixels.resize(std::size_t{size} * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x % 32) - 15.5f, fy = static_cast<float>(y % 32) - 15.5f;
            const float r = std::sqrt(fx * fx + fy * fy);
            const std::size_t i = (std::size_t{y} * size + x) * 4;
            img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = 255;
            img.pixels[i + 3] = static_cast<std::uint8_t>(std::clamp((13.0f - r) * 40.0f + 128.0f, 0.0f, 255.0f));
        }
    return img;
}

float to_linear(std::uint8_t v) {
    const float c = v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Box filter in linear light (MSAA resolves linear HDR), in linear units x 255.
std::vector<float> downsample(const std::vector<std::uint8_t>& px, std::uint32_t w, std::uint32_t h, std::uint32_t f) {
    std::vector<float> out(std::size_t{w / f} * (h / f) * 3, 0.0f);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                out[(std::size_t{y / f} * (w / f) + x / f) * 3 + c] +=
                    255.0f * to_linear(px[(std::size_t{y} * w + x) * 4 + c]) / float(f * f);
    return out;
}

double error(const std::vector<float>& ref, const std::vector<float>& img) {
    double e = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) e += std::abs(ref[i] - img[i]);
    return e / static_cast<double>(ref.size());
}

} // namespace

TEST_CASE("gpu msaa: 4x MSAA and alpha to coverage approach a supersampled reference") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    if (!dev.supports_samples(rhi::Format::RGBA16_FLOAT, 4) && !dev.supports_samples(rhi::Format::R11G11B10_FLOAT, 4)) {
        MESSAGE("4x MSAA unsupported on this device; skipped");
        return;
    }
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto tex = ctx.create_texture(discs(256), TextureOptions{.name = "msaa_discs"});
    REQUIRE(tex.ok());
    auto plane = ctx.create_mesh(make_plane({6, 6}), "msaa_plane").value();
    auto cube = ctx.create_mesh(make_cube({0.6f, 0.6f, 0.6f}), "msaa_cube").value();
    Material leaf;
    leaf.unlit = true;
    leaf.double_sided = true;
    leaf.alpha_cutoff = 0.5f;
    leaf.base_color_texture = tex.value();
    leaf.uv_scale = {2, 2};
    Material solid;
    solid.unlit = true;

    auto shot = [&](bool cutout, std::uint32_t w, std::uint32_t h, std::uint32_t msaa, bool a2c, std::uint32_t* used = nullptr) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = false;
        s.fxaa = false;
        s.bloom = false;
        s.ssao = false;
        s.auto_exposure = false;
        s.msaa_samples = msaa;
        s.alpha_to_coverage = a2c;
        sr.value()->set_settings(s);
        auto target = test::make_target(w, h);
        Camera3D cam = Camera3D::look_at({0.4f, 3.2f, 2.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(50.0f);
        // No tonemap curve or vignette: the output is linear light (sRGB-encoded), so a linear box
        // filter of the supersampled frame is the exact reference for a resolve.
        Environment env;
        env.sky_zenith = env.sky_horizon = env.ground = {0.05f, 0.05f, 0.05f, 1};
        env.vignette = 0;
        env.tonemap = false;
        sr.value()->begin(cam, DirectionalLight{}, env);
        if (cutout) sr.value()->submit(plane, leaf, math::Mat4::rotation(math::Quat::axis_angle({0, 1, 0}, math::radians(17.0f))));
        else sr.value()->submit(cube, solid, math::Mat4::rotation(math::Quat::axis_angle({0, 1, 0}, math::radians(23.0f))));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, w, h, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        if (used) *used = sr.value()->stats().msaa_samples;
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    for (const bool cutout : {false, true}) {
        const auto ref = downsample(shot(cutout, W * SS, H * SS, 1, false), W * SS, H * SS, SS);
        std::uint32_t used1 = 0, used4 = 0;
        const auto one = shot(cutout, W, H, 1, false, &used1);
        const auto ms = shot(cutout, W, H, 4, false, &used4);
        const auto a2c = shot(cutout, W, H, 4, true);
        test::save_output(cutout ? "msaa_leaves_1x" : "msaa_edges_1x", W, H, one);
        test::save_output(cutout ? "msaa_leaves_4x" : "msaa_edges_4x", W, H, ms);
        test::save_output(cutout ? "msaa_leaves_a2c" : "msaa_edges_a2c", W, H, a2c);
        CHECK(used1 == 1u);
        CHECK(used4 == 4u);
        const double e1 = error(ref, downsample(one, W, H, 1));
        const double e4 = error(ref, downsample(ms, W, H, 1));
        const double ea = error(ref, downsample(a2c, W, H, 1));
        INFO((cutout ? "cut-out" : "edges") << ": 1x " << e1 << ", 4x MSAA " << e4 << ", 4x + A2C " << ea);
        CHECK(e1 > 0.5); // the scene has aliasing to remove
        if (!cutout) {
            CHECK(e4 < e1 * 0.6);
            CHECK(std::abs(ea - e4) < 0.05); // no cut-outs: alpha to coverage changes nothing
        } else {
            CHECK(ea < e1 * 0.7);
            CHECK(ea < e4 * 0.8);
        }
    }
    // TAA owns anti-aliasing when on: MSAA is not used with it.
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = true;
    s.msaa_samples = 4;
    sr.value()->set_settings(s);
    auto target = test::make_target(W, H);
    sr.value()->begin(Camera3D::look_at({0, 2, 3}, {0, 0, 0}, {0, 1, 0}), DirectionalLight{}, {});
    sr.value()->submit(cube, solid, math::Mat4::identity());
    auto cmd = dev.begin_commands();
    REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    CHECK(sr.value()->stats().msaa_samples == 1u);
    dev.destroy(target);
    dev.destroy(tex.value());
    ctx.destroy(plane);
    ctx.destroy(cube);
}

TEST_CASE("gpu msaa: the Low preset asks for 4x MSAA with alpha to coverage") {
    const RenderSettings low = RenderSettings::preset(QualityTier::Low);
    CHECK(low.msaa_samples == 4u);
    CHECK(low.alpha_to_coverage);
    CHECK_FALSE(low.taa);
    RenderSettings odd;
    odd.msaa_samples = 3;
    CHECK(odd.sanitized().msaa_samples == 2u);
    odd.msaa_samples = 64;
    CHECK(odd.sanitized().msaa_samples == 8u);
    odd.msaa_samples = 0;
    CHECK(odd.sanitized().msaa_samples == 1u);
}
