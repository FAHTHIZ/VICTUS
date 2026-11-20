// SPDX-License-Identifier: Apache-2.0
// ORM occlusion (ADR-0139): the red channel of the metallic-roughness texture scales ambient
// light by material.occlusion_strength (glTF's occlusionTexture packed as in "ORM" textures),
// and leaves direct light alone.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

TEST_CASE("gpu orm: metallic-roughness red channel occludes ambient light only (ADR-0139)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = s.fxaa = s.ssao = s.bloom = false;
    s.auto_exposure = false;
    (*sr)->set_settings(s);
    Image orm;
    orm.width = orm.height = 4;
    for (int i = 0; i < 16; ++i) orm.pixels.insert(orm.pixels.end(), {64, 255, 0, 255}); // AO 0.25, roughness 1, dielectric
    auto tex = ctx.create_texture(orm, {false, false, "orm"});
    REQUIRE(tex.ok());
    auto plane = ctx.create_mesh(make_plane({4, 4}), "orm_plane").value();
    constexpr std::uint32_t W = 32, H = 32;
    const auto shoot = [&](float strength, float sun_intensity) {
        DirectionalLight sun;
        sun.direction = {0, -1, 0};
        sun.intensity = sun_intensity;
        sun.shadows = false;
        (*sr)->begin(Camera3D::look_at({0, 3, 0.01f}, {0, 0, 0}), sun, {});
        Material m;
        m.base_color = {0.8f, 0.8f, 0.8f, 1};
        m.metallic_roughness_texture = *tex;
        m.occlusion_strength = strength;
        (*sr)->submit(plane, m, math::Mat4::identity());
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return int(px[(H / 2 * W + W / 2) * 4 + 1]);
    };
    const int open = shoot(0.0f, 0.0f), occluded = shoot(1.0f, 0.0f), half = shoot(0.5f, 0.0f);
    const int lit_open = shoot(0.0f, 2.0f), lit_occluded = shoot(1.0f, 2.0f);
    MESSAGE("ambient only: strength 0 " << open << ", 0.5 " << half << ", 1 " << occluded << "; with sun " << lit_open << " / "
                                        << lit_occluded);
    CHECK(open > 40);
    CHECK(occluded < open * 0.7);  // 0.25 occlusion, through the sRGB curve
    CHECK(half > occluded);
    CHECK(half < open);
    // Direct sunlight is not occluded, so a sunlit occluded plane keeps far more of its brightness.
    CHECK(lit_occluded > occluded + 20);
    CHECK(double(lit_occluded) / lit_open > double(occluded) / open + 0.2);
    ctx.destroy(plane);
    dev.destroy(*tex);
}
