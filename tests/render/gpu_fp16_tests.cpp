// SPDX-License-Identifier: Apache-2.0
// FP16 shading (ADR-0137): the half-precision PBR variant must be used where the device enabled
// shaderFloat16, look the same as FP32 within an 8-bit step on average, and keep the cases half
// precision is known to break under control: a mirror-smooth highlight (GGX overflow at
// roughness 0), very dark and very bright materials, and grazing angles.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

TEST_CASE("gpu fp16: half-precision BRDF matches FP32 without blown highlights") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    SceneRendererOptions half_opts;
    half_opts.half_precision = true;
    auto half = SceneRenderer::create(ctx, half_opts);
    REQUIRE(half.ok());
    if (!dev.capabilities().shader_float16) {
        CHECK_FALSE(half.value()->half_precision()); // falls back to FP32 cleanly
        MESSAGE("device has no shaderFloat16; skipping the comparison");
        return;
    }
    REQUIRE(half.value()->half_precision());
    auto full = SceneRenderer::create(ctx);
    REQUIRE(full.ok());
    CHECK_FALSE(full.value()->half_precision());

    auto ground = ctx.create_mesh(make_plane({60, 60}), "fp16_ground").value();
    auto sphere = ctx.create_mesh(make_uv_sphere(0.6f, 48, 24), "fp16_sphere").value();
    constexpr std::uint32_t W = 256, H = 144;
    const auto shoot = [&](SceneRenderer& r) {
        RenderSettings s = RenderSettings::preset(QualityTier::Medium);
        s.auto_exposure = false;
        s.taa = false; // one frame, so only the shading differs
        r.set_settings(s);
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = {0.3f, -0.6f, -0.5f};
        sun.intensity = 6.0f; // bright enough that an overflowing GGX lobe would saturate
        sun.shadow_distance = 40;
        r.begin(Camera3D::look_at({0, 1.6f, 5}, {0, 0.6f, 0}), sun, {});
        Material g;
        g.base_color = {0.5f, 0.5f, 0.5f, 1};
        g.roughness = 0.3f; // grazing-angle specular on the far floor
        r.submit(ground, g, math::Mat4::identity());
        // Mirror metal, rough metal, black dielectric, white dielectric, smooth dielectric.
        const struct { math::Color c; float rough, metal; } mats[] = {
            {{0.95f, 0.93f, 0.88f, 1}, 0.0f, 1.0f}, {{0.9f, 0.6f, 0.3f, 1}, 0.7f, 1.0f},
            {{0.01f, 0.01f, 0.01f, 1}, 0.5f, 0.0f}, {{0.95f, 0.95f, 0.95f, 1}, 0.9f, 0.0f},
            {{0.2f, 0.4f, 0.8f, 1}, 0.05f, 0.0f}};
        for (int i = 0; i < 5; ++i) {
            Material m;
            m.base_color = mats[i].c;
            m.roughness = mats[i].rough;
            m.metallic = mats[i].metal;
            r.submit(sphere, m, math::Mat4::translation({-2.6f + 1.3f * float(i), 0.6f, 0.0f}));
        }
        PointLight pl;
        pl.position = {0.5f, 1.8f, 1.5f};
        pl.color = {1.0f, 0.8f, 0.6f, 1};
        pl.intensity = 30.0f;
        pl.range = 10.0f;
        r.add_point_light(pl);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const auto a = shoot(*full.value());
    const auto b = shoot(*half.value());
    test::save_output("fp16_medium", W, H, b);
    REQUIRE(a.size() == b.size());
    // The mirror sphere (left, screen x < 84) is compared separately: FP16 clamps the perceptual
    // roughness to 0.089 instead of 0.045 (Filament's mobile minimum), so its sun highlight is a
    // deliberately broader, dimmer-peaked lobe rather than a pinpoint.
    double abs_sum = 0, bias[3] = {0, 0, 0};
    int worst = 0;
    std::size_t differ = 0, over4 = 0, rest = 0, sat_a = 0, sat_b = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::uint32_t x = std::uint32_t((i / 4) % W);
        if (i % 4 == 3) {
            if (x < 84) {
                sat_a += a[i - 1] >= 250 && a[i - 2] >= 250 && a[i - 3] >= 250;
                sat_b += b[i - 1] >= 250 && b[i - 2] >= 250 && b[i - 3] >= 250;
            }
            continue;
        }
        const int d = int(b[i]) - int(a[i]);
        if (d != 0) ++differ;
        if (x < 84) continue;
        ++rest;
        abs_sum += std::abs(d);
        bias[i % 4] += d;
        worst = std::max(worst, std::abs(d));
        if (std::abs(d) > 4) ++over4;
    }
    const double n = double(rest) / 3.0;
    MESSAGE("fp16: mean |diff| " << abs_sum / rest << ", worst " << worst << ", differing " << differ << ", >4: "
                                 << over4 << ", bias " << bias[0] / n << " " << bias[1] / n << " " << bias[2] / n
                                 << ", mirror saturated px " << sat_b << " vs " << sat_a);
    CHECK(differ > 0);                         // the half variant really ran
    CHECK(abs_sum / rest < 0.5);               // well below one 8-bit step on average
    CHECK(over4 < std::size_t(0.002 * rest));  // no NaN/Inf patches or banding
    CHECK(worst < 24);
    for (double c : bias) CHECK(std::abs(c / n) < 0.25); // no brightening or hue shift
    CHECK(sat_b <= sat_a + 12);                // the mirror highlight does not blow out
    ctx.destroy(ground);
    ctx.destroy(sphere);
}
