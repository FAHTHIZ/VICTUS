// SPDX-License-Identifier: Apache-2.0
// Volumetric fog (ADR-0098): froxel injection, front-to-back integration and the composite against
// the analytic solution for a uniform medium, sun shadowing (light shafts), local light glow, fog
// volumes, temporal reprojection, quality gating and the Environment / FogVolume components.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/scene/scene.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return test::pixel(px, W, x, y); }
    [[nodiscard]] int red(std::uint32_t x, std::uint32_t y) const { return channel(at(x, y), 0); }
    // Mean red over a rectangle.
    [[nodiscard]] double mean_red(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) const {
        double sum = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) sum += red(x, y);
        return sum / static_cast<double>((x1 - x0) * (y1 - y0));
    }
};

int srgb8(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(s * 255.0f));
}

RenderSettings fog_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::Medium);
    s.bloom = false;
    s.fxaa = false;
    s.taa = false;
    s.ssao = false;
    return s;
}

// Uniform grey sky of radiance `ambient`, no tonemapping curve, no background.
Environment fog_env(float ambient, float density) {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {1, 1, 1, 1};
    env.ambient_intensity = ambient;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.volumetric_fog.enabled = true;
    env.volumetric_fog.density = density;
    env.volumetric_fog.height_falloff = 0.0f;
    env.volumetric_fog.temporal = 0.0f;
    env.volumetric_fog.sun_intensity = 0.0f;
    return env;
}

const Camera3D kFront = Camera3D::look_at({0, 0, 10}, {0, 0, 0});

struct Scene3 {
    GpuMesh cube;
    Material wall;
    explicit Scene3(GpuContext& ctx, float grey) : cube(ctx.create_mesh(make_cube(), "fog_cube").value()) {
        wall.base_color = {grey, grey, grey, 1};
        wall.unlit = true;
    }
    // Wall whose front face is the plane z = `z`.
    void submit_wall(SceneRenderer& r, float z = 0.0f) const { r.submit(cube, wall, math::Mat4::trs({0, 0, z - 0.5f}, {}, {80, 80, 1})); }
};

template <typename F>
Shot shoot(SceneRenderer& r, const Environment& env, F&& content, const char* save = nullptr, const RenderSettings& settings = fog_settings(),
           DirectionalLight sun = {}, Camera3D cam = kFront) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    r.set_settings(settings);
    r.begin(cam, sun, env);
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

DirectionalLight no_sun() {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    return sun;
}

} // namespace

TEST_CASE("gpu volumetric fog: a uniform medium matches the analytic in-scattering and transmittance") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.8f);
    // Wall 10 m away, fog extinction 0.1 / m, white albedo, sky radiance 0.2 and no sun:
    // L = 0.8 * T + 0.2 * (1 - T), T = exp(-1).
    const float T = std::exp(-1.0f);
    const Shot fogged = shoot(r, fog_env(0.2f, 0.1f), [&](SceneRenderer& rr) { sc.submit_wall(rr); }, "fog_uniform", fog_settings(), no_sun());
    CHECK(fogged.stats.volumetric_fog);
    CHECK(fogged.stats.fog_froxels_x == 10); // 160 / 16
    CHECK(fogged.stats.fog_froxels_y == 6);  // 96 / 16
    CHECK(fogged.stats.fog_slices == 48);    // Medium
    const int expected = srgb8(0.8f * T + 0.2f * (1.0f - T));
    CHECK(std::abs(fogged.red(W / 2, H / 2) - expected) <= 3);

    // Twice the extinction: T = exp(-2).
    const float T2 = std::exp(-2.0f);
    const Shot dense = shoot(r, fog_env(0.2f, 0.2f), [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    CHECK(std::abs(dense.red(W / 2, H / 2) - srgb8(0.8f * T2 + 0.2f * (1.0f - T2))) <= 3);

    // Distance limits the grid: with 8 m of fog only 8 m of the path is fogged.
    Environment near_env = fog_env(0.2f, 0.1f);
    near_env.volumetric_fog.distance = 8.0f;
    const float T8 = std::exp(-0.8f);
    const Shot limited = shoot(r, near_env, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    CHECK(std::abs(limited.red(W / 2, H / 2) - srgb8(0.8f * T8 + 0.2f * (1.0f - T8))) <= 3);

    // Off: the unfogged wall.
    Environment off = fog_env(0.2f, 0.1f);
    off.volumetric_fog.enabled = false;
    const Shot clear = shoot(r, off, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    CHECK_FALSE(clear.stats.volumetric_fog);
    CHECK(std::abs(clear.red(W / 2, H / 2) - srgb8(0.8f)) <= 1);
    // Disabled by the quality settings (and on the Low tier).
    RenderSettings no_fog = fog_settings();
    no_fog.volumetric_fog = false;
    const Shot gated = shoot(r, fog_env(0.2f, 0.1f), [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, no_fog, no_sun());
    CHECK_FALSE(gated.stats.volumetric_fog);
    CHECK(gated.red(W / 2, H / 2) == clear.red(W / 2, H / 2));
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).volumetric_fog);
}

TEST_CASE("gpu volumetric fog: height falloff thins the fog above its base height") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.0f); // black wall: the image is the in-scattered light alone
    Environment env = fog_env(0.5f, 0.2f);
    env.volumetric_fog.height = 0.0f;
    env.volumetric_fog.height_falloff = 1.0f;
    const Shot s = shoot(r, env, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, "fog_height", fog_settings(), no_sun());
    // Upper rows look through fog well above y = 0 (thin), lower rows through the dense layer.
    const double top = s.mean_red(40, 4, 120, 14), bottom = s.mean_red(40, H - 14, 120, H - 4);
    CHECK(bottom > top + 30);
}

TEST_CASE("gpu volumetric fog: sun light scatters only where the shadow map sees the sun") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.0f);
    GpuMesh slab = ctx.create_mesh(make_cube(), "fog_slab").value();
    Material grey;
    grey.base_color = {0.2f, 0.2f, 0.2f, 1};
    DirectionalLight sun;
    sun.direction = {0, -1, 0};
    sun.intensity = 6.0f;
    sun.shadows = true;
    sun.shadow_distance = 30.0f;
    Environment env = fog_env(0.0f, 0.15f);
    env.volumetric_fog.sun_intensity = 1.0f;
    env.volumetric_fog.anisotropy = 0.0f;
    // A roof over the left half of the view (x < 0) shadows the fog below it.
    const Shot s = shoot(
        r, env,
        [&](SceneRenderer& rr) {
            sc.submit_wall(rr, -10.0f);
            rr.submit(slab, grey, math::Mat4::trs({-20.5f, 8.0f, 0}, {}, {40, 0.5f, 60}));
        },
        "fog_shafts", fog_settings(), sun);
    CHECK(s.stats.volumetric_fog);
    const double left = s.mean_red(10, 60, 60, 90), right = s.mean_red(100, 60, 150, 90);
    CHECK(right > left + 25);
    CHECK(right > 40);

    // Without a sun the fog stays dark (ambient 0).
    DirectionalLight off = sun;
    off.intensity = 0.0f;
    const Shot dark = shoot(r, env, [&](SceneRenderer& rr) { sc.submit_wall(rr, -10.0f); }, nullptr, fog_settings(), off);
    CHECK(dark.mean_red(100, 60, 150, 90) < 3);
}

TEST_CASE("gpu volumetric fog: local lights glow through the fog") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.0f);
    Environment env = fog_env(0.0f, 0.1f);
    env.volumetric_fog.anisotropy = 0.3f;
    PointLight p;
    p.position = {-3, 0, 4};
    p.color = {1, 1, 1, 1};
    p.intensity = 40.0f;
    p.range = 8.0f;
    const Shot lit = shoot(
        r, env,
        [&](SceneRenderer& rr) {
            sc.submit_wall(rr);
            rr.add_point_light(p);
        },
        "fog_point_light", fog_settings(), no_sun());
    // Project the light: it sits left of centre; the right edge is out of its range.
    const double near_light = lit.mean_red(20, 38, 60, 58), far_side = lit.mean_red(140, 38, 158, 58);
    CHECK(near_light > far_side + 30);
    env.volumetric_fog.local_light_intensity = 0.0f;
    const Shot muted = shoot(
        r, env,
        [&](SceneRenderer& rr) {
            sc.submit_wall(rr);
            rr.add_point_light(p);
        },
        nullptr, fog_settings(), no_sun());
    CHECK(muted.mean_red(20, 38, 60, 58) < near_light - 30);
}

TEST_CASE("gpu volumetric fog: fog volumes add density inside their box") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.0f);
    Environment env = fog_env(0.5f, 0.0f); // no global fog
    FogVolume v;
    v.transform = math::Mat4::trs({-3, 0, 2}, {}, {4, 4, 4});
    v.density = 0.5f;
    v.albedo = {1, 0.2f, 0.2f, 1};
    v.edge_fade = 0.05f;
    const Shot s = shoot(
        r, env,
        [&](SceneRenderer& rr) {
            sc.submit_wall(rr);
            rr.add_fog_volume(v);
        },
        "fog_volume", fog_settings(), no_sun());
    CHECK(s.stats.volumetric_fog);
    CHECK(s.stats.fog_volumes == 1);
    // Box spans x in [-5, -1] at z in [0, 4]: left of centre. Red albedo: red > green inside.
    const std::uint32_t inside = s.at(40, H / 2), outside = s.at(140, H / 2);
    CHECK(channel(inside, 0) > 60);
    CHECK(channel(inside, 0) > channel(inside, 1) + 20);
    CHECK(channel(outside, 0) < 3);
    // Invalid volumes are ignored.
    FogVolume bad = v;
    bad.density = -1.0f;
    const Shot ignored = shoot(
        r, env,
        [&](SceneRenderer& rr) {
            sc.submit_wall(rr);
            rr.add_fog_volume(bad);
        },
        nullptr, fog_settings(), no_sun());
    CHECK_FALSE(ignored.stats.volumetric_fog); // no density anywhere: the passes are skipped
}

TEST_CASE("gpu volumetric fog: temporal reprojection converges to the unfiltered result") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Scene3 sc(ctx, 0.8f);
    const Shot reference = shoot(r, fog_env(0.2f, 0.1f), [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    Environment env = fog_env(0.2f, 0.1f);
    env.volumetric_fog.temporal = 0.9f;
    Shot last;
    for (int i = 0; i < 6; ++i) last = shoot(r, env, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    CHECK(std::abs(last.red(W / 2, H / 2) - reference.red(W / 2, H / 2)) <= 3);
    // A moved camera reprojects: no smearing of the static, uniform fog.
    const Camera3D moved = Camera3D::look_at({0.5f, 0, 10}, {0.5f, 0, 0});
    const Shot after = shoot(r, env, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun(), moved);
    CHECK(std::abs(after.red(W / 2, H / 2) - reference.red(W / 2, H / 2)) <= 3);
    r.reset_history();
    const Shot reset = shoot(r, env, [&](SceneRenderer& rr) { sc.submit_wall(rr); }, nullptr, fog_settings(), no_sun());
    CHECK(std::abs(reset.red(W / 2, H / 2) - reference.red(W / 2, H / 2)) <= 3);
}

TEST_CASE("gpu volumetric fog: Environment and FogVolume components through runtime extraction") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    gameplay::register_components();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const auto cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    s.world().get<scene::Transform>(cam)->position = {0, 0, 10};
    const auto wall = s.create("Wall");
    s.world().get<scene::Transform>(wall)->position = {0, 0, -0.5f};
    s.world().get<scene::Transform>(wall)->scale = {80, 80, 1};
    s.world().add<scene::MeshRendererComponent>(wall).primitive = "cube";
    const auto enve = s.create("Environment");
    auto& ec = s.world().add<gameplay::EnvironmentComponent>(enve);
    ec.volumetric_fog = true;
    ec.volumetric_density = 0.0f;
    ec.volumetric_anisotropy = 5.0f; // clamped
    ec.volumetric_temporal = 0.0f;
    const auto box = s.create("Mist");
    s.world().get<scene::Transform>(box)->position = {0, 0, 3};
    auto& fv = s.world().add<gameplay::FogVolumeComponent>(box);
    fv.density = 0.4f;
    fv.albedo = {1, 0, 0, 1}; // absorbs green and blue: unmistakable over the ambient-lit wall

    render::Environment mapped;
    render::DirectionalLight sun;
    runtime::apply_environment(ec, mapped, sun);
    CHECK(mapped.volumetric_fog.enabled);
    CHECK(mapped.volumetric_fog.anisotropy == doctest::Approx(0.95f));
    CHECK(mapped.volumetric_fog.temporal == 0.0f);

    auto frame = [&]() {
        s.update_transforms();
        runtime::SceneViewSettings settings;
        const runtime::ExtractStats st = runtime::extract_scene(s, cache, r, settings);
        r.set_settings(fog_settings());
        auto& dev = ctx.device();
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value(), r.stats()};
        dev.destroy(target);
        return std::pair{st, out};
    };
    const auto [st, shot] = frame();
    CHECK(st.fog_volumes == 1);
    CHECK(shot.stats.volumetric_fog);
    CHECK(shot.stats.fog_volumes == 1);
    s.world().get<gameplay::EnvironmentComponent>(enve)->volumetric_fog = false;
    const auto [st2, plain] = frame();
    CHECK(st2.fog_volumes == 0); // volumes only count while the environment's fog is on
    CHECK_FALSE(plain.stats.volumetric_fog);
    CHECK(channel(shot.at(W / 2, H / 2), 1) + 40 < channel(plain.at(W / 2, H / 2), 1));
}
