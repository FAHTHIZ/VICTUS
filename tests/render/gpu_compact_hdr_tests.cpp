// SPDX-License-Identifier: Apache-2.0
// Compact HDR (ADR-0129): the scene colour, bloom chain and TAA history stored as R11G11B10_FLOAT
// must look the same as RGBA16_FLOAT at every tier, including after many TAA frames, while
// using less render-target memory.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

TEST_CASE("gpu compact hdr: R11G11B10 scene colour matches RGBA16F at every tier, with less memory") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    if (!dev.supports_format(rhi::Format::R11G11B10_FLOAT,
                             rhi::TextureUsageSampled | rhi::TextureUsageColorTarget)) {
        MESSAGE("R11G11B10_FLOAT render targets unsupported on this device; skipping");
        return;
    }
    auto ground = ctx.create_mesh(make_plane({60, 60}), "chdr_ground").value();
    auto cube = ctx.create_mesh(make_cube(), "chdr_cube").value();
    auto sphere = ctx.create_mesh(make_uv_sphere(0.6f, 32, 16), "chdr_sphere").value();
    constexpr std::uint32_t W = 256, H = 144;

    // Saturated and dark materials, a blue sky, an emissive (bloom) and a coloured point light: the
    // cases where the 5-bit blue mantissa or a hue shift would show.
    const auto shoot = [&](SceneRenderer& r, RenderSettings s, int frames) {
        r.set_settings(s);
        auto target = test::make_target(W, H);
        std::vector<std::uint8_t> px;
        for (int f = 0; f < frames; ++f) {
            DirectionalLight sun;
            sun.direction = {0.4f, -0.7f, -0.3f};
            sun.shadow_distance = 40;
            r.begin(Camera3D::look_at({0, 2.2f, 6}, {0, 0.6f, 0}), sun, {});
            Material g;
            g.base_color = {0.18f, 0.18f, 0.2f, 1};
            g.roughness = 0.8f;
            r.submit(ground, g, math::Mat4::identity());
            const math::Color cols[] = {{0.9f, 0.05f, 0.05f, 1}, {0.05f, 0.7f, 0.1f, 1}, {0.05f, 0.1f, 0.9f, 1},
                                        {0.02f, 0.02f, 0.025f, 1}};
            for (int i = 0; i < 4; ++i) {
                Material m;
                m.base_color = cols[i];
                m.roughness = 0.25f + 0.2f * float(i);
                r.submit(cube, m, math::Mat4::translation({-3.0f + 2.0f * float(i), 0.5f, -1.0f}));
            }
            Material glow;
            glow.base_color = {0.1f, 0.1f, 0.1f, 1};
            glow.emissive = {0.3f, 0.6f, 1.0f, 1};
            glow.emissive_strength = 12.0f;
            r.submit(sphere, glow, math::Mat4::translation({0.0f, 1.2f, 1.0f}));
            PointLight pl;
            pl.position = {2.0f, 1.0f, 1.5f};
            pl.color = {1.0f, 0.55f, 0.2f, 1};
            pl.intensity = 20.0f;
            pl.range = 8.0f;
            r.add_point_light(pl);
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
        }
        px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };

    for (const QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High, QualityTier::Ultra}) {
        RenderSettings s = RenderSettings::preset(tier);
        s.auto_exposure = false; // the same fixed exposure for both, so only storage differs
        const int frames = s.taa ? 24 : 1; // let TAA history accumulate (and any drift build up)

        const auto before_full = dev.stats().texture_bytes;
        auto full = SceneRenderer::create(ctx);
        REQUIRE(full.ok());
        const auto a = shoot(*full.value(), s, frames);
        const auto full_bytes = dev.stats().texture_bytes - before_full;
        CHECK(full.value()->hdr_format() == rhi::Format::RGBA16_FLOAT);
        full.value().reset();

        const auto before_compact = dev.stats().texture_bytes;
        SceneRendererOptions o;
        o.compact_hdr = true;
        auto compact = SceneRenderer::create(ctx, o);
        REQUIRE(compact.ok());
        const auto b = shoot(*compact.value(), s, frames);
        const auto compact_bytes = dev.stats().texture_bytes - before_compact;
        CHECK(compact.value()->hdr_format() == rhi::Format::R11G11B10_FLOAT);
        if (tier == QualityTier::Medium) test::save_output("compact_hdr_medium", W, H, b);
        compact.value().reset();

        REQUIRE(a.size() == b.size());
        double abs_sum = 0, bias[3] = {0, 0, 0};
        int worst = 0;
        std::size_t over4 = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i % 4 == 3) continue;
            const int d = int(b[i]) - int(a[i]);
            abs_sum += std::abs(d);
            bias[i % 4] += d;
            worst = std::max(worst, std::abs(d));
            if (std::abs(d) > 4) ++over4;
        }
        const double n = double(a.size() / 4);
        const double mean = abs_sum / (3.0 * n);
        MESSAGE("compact hdr tier " << int(tier) << ": mean |diff| " << mean << ", worst " << worst << ", >4: "
                                    << over4 << ", bias rgb " << bias[0] / n << " " << bias[1] / n << " "
                                    << bias[2] / n << ", render-target bytes " << compact_bytes << " vs "
                                    << full_bytes);
        CHECK(mean < 0.75); // below one 8-bit step on average
        // Without TAA under 0.1 % of channel samples differ by more than 4/255. With TAA the rest sits
        // on the one-pixel horizon edge, where jittered history converges a few steps differently.
        CHECK(over4 < std::size_t((s.taa ? 0.02 : 0.001) * 3.0 * n));
        for (double c : bias) CHECK(std::abs(c / n) < 0.5); // no hue shift
        CHECK(compact_bytes < full_bytes);
    }
    ctx.destroy(ground);
    ctx.destroy(cube);
    ctx.destroy(sphere);
}
