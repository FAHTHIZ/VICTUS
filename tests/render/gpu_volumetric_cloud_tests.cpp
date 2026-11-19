// SPDX-License-Identifier: Apache-2.0
// Volumetric clouds (ADR-0111): the reduced-resolution march and its composite. Checks that the
// layer appears only where view rays cross it, that geometry in front of the layer is untouched at
// full resolution while ground seen through the layer from above is covered, that coverage,
// density and sun light behave monotonically, that wind moves the shapes deterministically with
// the renderer time, quality gating, and the Environment component mapping.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cmath>
#include <format>

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
    [[nodiscard]] double mean(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, int c = 0) const {
        double sum = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) sum += channel(at(x, y), c);
        return sum / static_cast<double>((x1 - x0) * (y1 - y0));
    }
    [[nodiscard]] int diff(const Shot& o, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) const {
        int worst = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = x0; x < x1; ++x)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(channel(at(x, y), c) - channel(o.at(x, y), c)));
        return worst;
    }
};

RenderSettings cloud_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.bloom = false;
    s.fxaa = false;
    s.taa = false;
    s.ssao = false;
    s.auto_exposure = false;
    return s;
}

// Black background, grey sky ambient for the clouds, overcast-ish layer 1.5..3.5 km.
Environment cloud_env(float coverage = 0.8f) {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {1, 1, 1, 1};
    env.ambient_intensity = 0.3f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.clouds.enabled = true;
    env.clouds.coverage = coverage;
    env.clouds.density = 0.03f;
    env.clouds.bottom = 1500.0f;
    env.clouds.top = 3500.0f;
    env.clouds.wind = {0, 0, 0};
    return env;
}

DirectionalLight sun_light(float intensity = 3.0f) {
    DirectionalLight sun;
    sun.direction = math::normalize(math::Vec3{0.3f, -0.8f, -0.4f});
    sun.intensity = intensity;
    sun.shadows = false;
    return sun;
}

// Looking up at the sky from the ground (the upper part of the image sees the layer).
const Camera3D kUp = [] {
    Camera3D c = Camera3D::look_at({0, 2, 0}, {0, 300, -1000});
    c.z_far = 5000.0f;
    return c;
}();

template <typename F>
Shot shoot(SceneRenderer& r, const Environment& env, F&& content, const char* save = nullptr, const RenderSettings& settings = cloud_settings(),
           DirectionalLight sun = sun_light(), Camera3D cam = kUp, double time = 0.0) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    r.set_settings(settings);
    r.set_time(time);
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

void nothing(SceneRenderer&) {}

} // namespace

TEST_CASE("gpu volumetric clouds: the layer appears where view rays cross it and only there") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Shot sky = shoot(r, cloud_env(), nothing, "clouds_up");
    CHECK(sky.stats.volumetric_clouds);
    CHECK(sky.stats.cloud_width == 80); // High: half resolution
    CHECK(sky.stats.cloud_height == 48);
    CHECK(sky.stats.cloud_steps == 48);
    // The upper image is lit cloud; the background is black where no cloud is.
    CHECK(sky.mean(0, 0, W, H / 3) > 20.0);
    // Below the horizon rays never reach the layer (camera under it).
    const Camera3D down = Camera3D::look_at({0, 2, 0}, {0, -300, -1000});
    const Shot ground = shoot(r, cloud_env(), nothing, nullptr, cloud_settings(), sun_light(), down);
    CHECK(ground.mean(0, H / 2, W, H) == 0.0);

    Environment off = cloud_env();
    off.clouds.enabled = false;
    const Shot clear = shoot(r, off, nothing);
    CHECK_FALSE(clear.stats.volumetric_clouds);
    CHECK(clear.stats.cloud_width == 0);
    CHECK(clear.mean(0, 0, W, H) == 0.0);
    // Degenerate layers are ignored.
    Environment flat = cloud_env();
    flat.clouds.top = flat.clouds.bottom;
    CHECK_FALSE(shoot(r, flat, nothing).stats.volumetric_clouds);
    Environment empty = cloud_env(0.0f);
    CHECK_FALSE(shoot(r, empty, nothing).stats.volumetric_clouds);
}

TEST_CASE("gpu volumetric clouds: coverage, density and the sun change the clouds monotonically") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    // Clear background in a saturated blue so cloud opacity shows as lost blue.
    Environment base = cloud_env(0.3f);
    base.clear_color = {0, 0, 1, 1};
    Environment thick = base;
    thick.clouds.coverage = 0.9f;
    const Shot sparse = shoot(r, base, nothing, "clouds_sparse");
    const Shot overcast = shoot(r, thick, nothing, "clouds_overcast");
    // More coverage: less of the blue background shows through and more cloud light.
    CHECK(overcast.mean(0, 0, W, H / 2, 2) < sparse.mean(0, 0, W, H / 2, 2) - 5.0);
    CHECK(overcast.mean(0, 0, W, H / 2, 0) > sparse.mean(0, 0, W, H / 2, 0) + 5.0);
    // Without sun or ambient light the clouds only absorb: darker than the lit ones.
    Environment dark = thick;
    dark.clouds.ambient_intensity = 0.0f;
    const Shot unlit = shoot(r, dark, nothing, nullptr, cloud_settings(), sun_light(0.0f));
    CHECK(unlit.mean(0, 0, W, H / 2, 0) < 1.0);
    CHECK(unlit.mean(0, 0, W, H / 2, 2) < overcast.mean(0, 0, W, H / 2, 2));
    // Seen from above, the sun lights the cloud tops well beyond the ambient-only clouds.
    Camera3D above = Camera3D::look_at({0, 6000, 0}, {0, 0, -3000});
    above.z_far = 20000.0f;
    const Shot lit_tops = shoot(r, thick, nothing, "clouds_tops", cloud_settings(), sun_light(), above);
    const Shot ambient_tops = shoot(r, thick, nothing, nullptr, cloud_settings(), sun_light(0.0f), above);
    CHECK(lit_tops.mean(0, 0, W, H, 0) > ambient_tops.mean(0, 0, W, H, 0) + 20.0);
    // Thinner clouds let more background through.
    Environment thin = thick;
    thin.clouds.density = 0.0005f;
    const Shot faint = shoot(r, thin, nothing);
    CHECK(faint.mean(0, 0, W, H / 2, 2) > overcast.mean(0, 0, W, H / 2, 2) + 5.0);
}

TEST_CASE("gpu volumetric clouds: geometry in front of the layer is kept, ground behind it is covered") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "cloud_cube").value();
    Material grey;
    grey.base_color = {0.5f, 0.5f, 0.5f, 1};
    grey.unlit = true;
    // A tall wall 50 m away covering the left half of the view, in front of the clouds.
    auto wall = [&](SceneRenderer& rr) { rr.submit(cube, grey, math::Mat4::trs({-30, 100, -50}, {}, {60, 400, 1})); };
    const Shot with = shoot(r, cloud_env(), wall, "clouds_wall");
    Environment off = cloud_env();
    off.clouds.enabled = false;
    const Shot without = shoot(r, off, wall);
    // Wall pixels (left, away from the silhouette) are identical; sky pixels on the right differ.
    CHECK(with.diff(without, 2, 2, W / 2 - 8, H / 3) == 0);
    CHECK(with.mean(W / 2 + 8, 0, W, H / 3) > without.mean(W / 2 + 8, 0, W, H / 3) + 10.0);

    // From above the layer, the ground below is covered by the clouds in between.
    Camera3D above = Camera3D::look_at({0, 6000, 0}, {0, 0, -1500});
    above.z_far = 20000.0f;
    auto ground = [&](SceneRenderer& rr) { rr.submit(cube, grey, math::Mat4::trs({0, -1, 0}, {}, {40000, 1, 40000})); };
    Environment overcast = cloud_env(1.0f);
    const Shot covered = shoot(r, overcast, ground, "clouds_from_above", cloud_settings(), sun_light(), above);
    overcast.clouds.enabled = false;
    const Shot bare = shoot(r, overcast, ground, nullptr, cloud_settings(), sun_light(), above);
    CHECK(covered.diff(bare, 0, 0, W, H) > 20);
}

TEST_CASE("gpu volumetric clouds: wind drifts the shapes with the renderer time, deterministically") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Environment env = cloud_env(0.6f);
    env.clouds.wind = {50, 0, 0};
    const Shot a = shoot(r, env, nothing, nullptr, cloud_settings(), sun_light(), kUp, 10.0);
    const Shot b = shoot(r, env, nothing, nullptr, cloud_settings(), sun_light(), kUp, 10.0);
    CHECK(a.diff(b, 0, 0, W, H) == 0);
    const Shot later = shoot(r, env, nothing, nullptr, cloud_settings(), sun_light(), kUp, 70.0); // 3 km further
    CHECK(later.diff(a, 0, 0, W, H / 2) > 10);
}

TEST_CASE("gpu volumetric clouds: quality gating and capture mode") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).volumetric_clouds);
    RenderSettings gated = cloud_settings();
    gated.volumetric_clouds = false;
    const Shot off = shoot(r, cloud_env(), nothing, nullptr, gated);
    CHECK_FALSE(off.stats.volumetric_clouds);
    CHECK(off.mean(0, 0, W, H) == 0.0);
    RenderSettings medium = RenderSettings::preset(QualityTier::Medium);
    medium.bloom = medium.fxaa = medium.taa = medium.ssao = false;
    const Shot med = shoot(r, cloud_env(), nothing, "clouds_medium", medium);
    CHECK(med.stats.cloud_width == 40); // quarter resolution
    CHECK(med.stats.cloud_steps == 32);
    CHECK(med.mean(0, 0, W, H / 3) > 20.0);
    r.set_capture_mode(true);
    const Shot capture = shoot(r, cloud_env(), nothing);
    CHECK_FALSE(capture.stats.volumetric_clouds);
    CHECK(r.path_report().find("volumetric_clouds")->reason == PathReason::CaptureMode);
    r.set_capture_mode(false);
}

TEST_CASE("gpu volumetric clouds: Environment component mapping") {
    gameplay::EnvironmentComponent ec;
    ec.clouds = true;
    ec.cloud_coverage = 3.0f;    // clamped
    ec.cloud_bottom = 800.0f;
    ec.cloud_thickness = -5.0f;  // at least 10 m
    ec.cloud_anisotropy = std::nanf("");
    ec.cloud_wind = {1, 2, 3};
    render::Environment env;
    render::DirectionalLight sun;
    runtime::apply_environment(ec, env, sun);
    CHECK(env.clouds.enabled);
    CHECK(env.clouds.coverage == 1.0f);
    CHECK(env.clouds.bottom == 800.0f);
    CHECK(env.clouds.top == doctest::Approx(810.0f));
    CHECK(env.clouds.anisotropy == doctest::Approx(0.6f));
    CHECK(env.clouds.wind.z == 3.0f);
    CHECK(env.clouds.detail_size == doctest::Approx(env.clouds.shape_size * 0.15f));
}
