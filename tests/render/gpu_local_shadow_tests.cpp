// SPDX-License-Identifier: Apache-2.0
// Local light shadows (Slice 56, ADR-0052): spot and point lights cast perspective shadows from
// one atlas, lit areas are unchanged, and the tile budget favours lights covering more of the view.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <string>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 192, H = 128;

RenderSettings plain_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = s.ssao = s.bloom = s.fxaa = s.auto_exposure = false;
    return s;
}
Environment dark_env() {
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.ambient_intensity = 0.0f;
    env.vignette = 0.0f;
    return env;
}
DirectionalLight no_sun() {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    return sun;
}
std::pair<std::uint32_t, std::uint32_t> project(const Camera3D& cam, math::Vec3 p) {
    const math::Vec4 c = cam.projection(static_cast<float>(W) / H) * cam.view * math::Vec4(p, 1);
    return {static_cast<std::uint32_t>((c.x / c.w * 0.5f + 0.5f) * W), static_cast<std::uint32_t>((0.5f - c.y / c.w * 0.5f) * H)};
}
int lum(std::uint32_t px) { return channel(px, 0) + channel(px, 1) + channel(px, 2); }

struct Scene {
    GpuMesh floor, cube;
    Material grey;
    Camera3D cam = Camera3D::look_at({0, 6, 6}, {0, 0, 0});
};

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename Lights>
Shot shoot(SceneRenderer& r, const Scene& sc, const RenderSettings& s, Lights&& lights, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    r.set_settings(s);
    r.begin(sc.cam, no_sun(), dark_env());
    r.submit(sc.floor, sc.grey, math::Mat4::identity());
    // A floating cube between the light (above the origin) and the floor.
    r.submit(sc.cube, sc.grey, math::Mat4::translation({0, 2, 0}));
    lights(r);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

} // namespace

TEST_CASE("gpu local shadows: spot and point lights shadow the floor under a cube") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene sc{ctx.create_mesh(make_plane({12, 12}), "ls_floor").value(), ctx.create_mesh(make_cube(), "ls_cube").value(), {}};
    sc.grey.base_color = {0.8f, 0.8f, 0.8f, 1};
    sc.grey.roughness = 0.9f;
    const RenderSettings s = plain_settings();
    REQUIRE(s.local_shadows);

    // Under the cube (inside its shadow) and well outside it, both visible to the camera.
    const auto [sx, sy] = project(sc.cam, {0, 0, 0.9f});
    const auto [lx, ly] = project(sc.cam, {0, 0, 2.4f});

    for (bool spot : {true, false}) {
        const std::string kind = spot ? "spot" : "point";
        INFO(kind);
        auto lights = [&](bool shadows) {
            return [=](SceneRenderer& rr) {
                if (spot) {
                    SpotLight l;
                    l.position = {0, 5, 0};
                    l.direction = {0, -1, 0};
                    l.intensity = 120;
                    l.range = 12;
                    l.inner_angle = 0.6f;
                    l.outer_angle = 0.75f;
                    l.cast_shadows = shadows;
                    rr.add_spot_light(l);
                } else {
                    PointLight l;
                    l.position = {0, 5, 0};
                    l.intensity = 120;
                    l.range = 12;
                    l.cast_shadows = shadows;
                    rr.add_point_light(l);
                }
            };
        };
        const Shot lit = shoot(r, sc, s, lights(false));
        const Shot shadowed = shoot(r, sc, s, lights(true), spot ? "local_shadow_spot" : "local_shadow_point");
        CHECK(lit.stats.local_shadow_lights == 0);
        CHECK(shadowed.stats.local_shadow_lights == 1);
        CHECK(shadowed.stats.local_shadow_tiles == (spot ? 1u : 6u));
        CHECK(shadowed.stats.local_shadow_draws >= 1);
        const int under_lit = lum(pixel(lit.px, W, sx, sy)), under_shadow = lum(pixel(shadowed.px, W, sx, sy));
        const int open_lit = lum(pixel(lit.px, W, lx, ly)), open_shadow = lum(pixel(shadowed.px, W, lx, ly));
        MESSAGE(kind << ": under cube " << under_lit << " -> " << under_shadow << ", open floor " << open_lit
                                          << " -> " << open_shadow);
        CHECK(under_lit > 60);
        CHECK(under_shadow * 4 < under_lit);          // the cube blocks the light
        CHECK(std::abs(open_shadow - open_lit) <= 6); // no acne or peter-panning on open floor
        // The top of the cube still sees the light.
        const auto [tx, ty] = project(sc.cam, {0, 2.5f, 0});
        CHECK(std::abs(lum(pixel(shadowed.px, W, tx, ty)) - lum(pixel(lit.px, W, tx, ty))) <= 6);
    }

    // Disabled by the settings (and on the Low tier).
    RenderSettings off = s;
    off.local_shadows = false;
    auto spot_light = [](SceneRenderer& rr) {
        SpotLight l;
        l.position = {0, 5, 0};
        l.intensity = 120;
        l.range = 12;
        l.outer_angle = 0.75f;
        l.cast_shadows = true;
        rr.add_spot_light(l);
    };
    CHECK(shoot(r, sc, off, spot_light).stats.local_shadow_tiles == 0);
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).local_shadows);
    ctx.destroy(sc.floor);
    ctx.destroy(sc.cube);
}

TEST_CASE("gpu local shadows: the tile budget goes to the lights that cover the most of the view") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene sc{ctx.create_mesh(make_plane({12, 12}), "ls_floor").value(), ctx.create_mesh(make_cube(), "ls_cube").value(), {}};
    RenderSettings s = plain_settings();
    s.local_shadow_atlas = 1024;
    s.local_shadow_tile = 512; // 2x2 = 4 tiles
    auto many = [](SceneRenderer& rr) {
        PointLight p; // six tiles: never fits a four-tile atlas
        p.position = {0, 3, 0};
        p.range = 8;
        p.cast_shadows = true;
        rr.add_point_light(p);
        for (int i = 0; i < 5; ++i) {
            SpotLight l;
            l.position = {-4.0f + 2.0f * i, 4, 0};
            l.range = 6.0f + static_cast<float>(i); // later spots reach further: larger on screen
            l.outer_angle = 0.6f;
            l.cast_shadows = true;
            rr.add_spot_light(l);
        }
        SpotLight wide; // beyond 85 degrees: not shadowed
        wide.position = {0, 4, 2};
        wide.outer_angle = 1.5f;
        wide.cast_shadows = true;
        rr.add_spot_light(wide);
    };
    const Shot shot = shoot(r, sc, s, many);
    CHECK(shot.stats.local_shadow_lights == 4);
    CHECK(shot.stats.local_shadow_tiles == 4);
    CHECK(shot.stats.spot_lights == 6);

    s.local_shadow_atlas = 2048; // 4x4 = 16 tiles: the point light fits too
    const Shot roomy = shoot(r, sc, s, many);
    CHECK(roomy.stats.local_shadow_lights == 6);
    CHECK(roomy.stats.local_shadow_tiles == 11);

    // Sanitising clamps the tile to at most 8x8 per atlas.
    RenderSettings odd;
    odd.local_shadow_atlas = 100000;
    odd.local_shadow_tile = 1;
    const RenderSettings clean = odd.sanitized();
    CHECK(clean.local_shadow_atlas == 8192);
    CHECK(clean.local_shadow_tile == 1024);
    ctx.destroy(sc.floor);
    ctx.destroy(sc.cube);
}
