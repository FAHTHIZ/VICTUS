// SPDX-License-Identifier: Apache-2.0
// Weather (ADR-0113): procedural rain and snow around the camera and wet surfaces. Checks that
// drops appear only when asked for, scale with intensity and the quality tier, are hidden behind
// nearer geometry, are deterministic in time and periodic in world space (so they stay put while
// the camera moves), that wetness darkens rough dielectrics but leaves smooth metal alone, the
// render-path report, and the Environment component mapping.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

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
    [[nodiscard]] double mean(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, int c = 0) const {
        double sum = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) sum += channel(at(x, y), c);
        return sum / static_cast<double>((x1 - x0) * (y1 - y0));
    }
    [[nodiscard]] int lit(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, int above = 2) const {
        int n = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) n += channel(at(x, y), 1) > above ? 1 : 0;
        return n;
    }
    [[nodiscard]] int diff(const Shot& o) const {
        int worst = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(channel(at(x, y), c) - channel(o.at(x, y), c)));
        return worst;
    }
};

RenderSettings weather_settings(QualityTier tier = QualityTier::High) {
    RenderSettings s = RenderSettings::preset(tier);
    s.bloom = false;
    s.fxaa = false;
    s.taa = false;
    s.ssao = false;
    s.auto_exposure = false;
    return s;
}

// Black background, white sky ambient: drops show up as light on black.
Environment weather_env(PrecipitationType type = PrecipitationType::Rain) {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {1, 1, 1, 1};
    env.ambient_intensity = 1.0f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.precipitation.type = type;
    env.precipitation.radius = 10.0f;
    return env;
}

DirectionalLight sun_light(float intensity = 2.0f) {
    DirectionalLight sun;
    sun.direction = math::normalize(math::Vec3{0.3f, -0.8f, -0.4f});
    sun.intensity = intensity;
    sun.shadows = false;
    return sun;
}

const Camera3D kAhead = [] {
    Camera3D c = Camera3D::look_at({0, 2, 0}, {0, 2, -10});
    c.z_far = 200.0f;
    return c;
}();

template <typename F>
Shot shoot(SceneRenderer& r, const Environment& env, F&& content, const char* save = nullptr,
           const RenderSettings& settings = weather_settings(), Camera3D cam = kAhead, double time = 1.0, DirectionalLight sun = sun_light()) {
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

TEST_CASE("gpu weather: rain and snow are drawn around the camera and scale with intensity and tier") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Environment dry = weather_env(PrecipitationType::None);
    const Shot none = shoot(r, dry, nothing);
    CHECK(none.stats.precipitation_drops == 0);
    CHECK(none.mean(0, 0, W, H) == 0.0);
    CHECK(r.path_report().find("precipitation")->reason == PathReason::Scene);

    const Shot rain = shoot(r, weather_env(), nothing, "weather_rain");
    CHECK(rain.stats.precipitation_drops == 16384); // High
    const int rain_lit = rain.lit(0, 0, W, H);
    CHECK(rain_lit > static_cast<int>(W * H / 50));
    CHECK(rain_lit < static_cast<int>(W * H * 3 / 4));
    CHECK(r.path_report().find("precipitation")->state == PathState::Reduced); // below the Ultra budget
    CHECK(r.path_report().find("precipitation")->reason == PathReason::QualityTier);

    const Shot snow = shoot(r, weather_env(PrecipitationType::Snow), nothing, "weather_snow");
    CHECK(snow.stats.precipitation_drops == 16384);
    CHECK(snow.lit(0, 0, W, H) > static_cast<int>(W * H / 50));
    CHECK(snow.mean(0, 0, W, H) > rain.mean(0, 0, W, H)); // opaque flakes outshine faint streaks

    Environment light = weather_env();
    light.precipitation.intensity = 0.25f;
    const Shot drizzle = shoot(r, light, nothing);
    CHECK(drizzle.stats.precipitation_drops == 4096);
    CHECK(drizzle.lit(0, 0, W, H) < rain_lit);

    CHECK(shoot(r, weather_env(), nothing, nullptr, weather_settings(QualityTier::Low)).stats.precipitation_drops == 2048);
    CHECK(shoot(r, weather_env(), nothing, nullptr, weather_settings(QualityTier::Medium)).stats.precipitation_drops == 8192);
    const Shot ultra = shoot(r, weather_env(), nothing, nullptr, weather_settings(QualityTier::Ultra));
    CHECK(ultra.stats.precipitation_drops == 32768);
    CHECK(r.path_report().find("precipitation")->state == PathState::Active);

    RenderSettings off = weather_settings();
    off.precipitation = false;
    const Shot gated = shoot(r, weather_env(), nothing, nullptr, off);
    CHECK(gated.stats.precipitation_drops == 0);
    CHECK(gated.mean(0, 0, W, H) == 0.0);
    CHECK(r.path_report().find("precipitation")->reason == PathReason::Setting);
    r.set_capture_mode(true);
    CHECK(shoot(r, weather_env(), nothing).stats.precipitation_drops == 0);
    CHECK(r.path_report().find("precipitation")->reason == PathReason::CaptureMode);
    r.set_capture_mode(false);
}

TEST_CASE("gpu weather: drops behind nearer geometry are hidden") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "weather_wall").value();
    Material black;
    black.base_color = {0, 0, 0, 1};
    black.roughness = 1.0f;
    // A black wall 1.2 m ahead covering the left half of the view.
    auto wall = [&](SceneRenderer& rr) { rr.submit(cube, black, math::Mat4::trs({-5.0f, 2, -1.7f}, {}, {10, 10, 1})); };
    const Shot open = shoot(r, weather_env(), nothing);
    const Shot blocked = shoot(r, weather_env(), wall, "weather_wall");
    const Shot wall_only = shoot(r, weather_env(PrecipitationType::None), wall);
    CHECK(blocked.stats.precipitation_drops == open.stats.precipitation_drops);
    // Pixels a drop brightened: rain against the bare sky versus rain against the wall.
    const auto streaks = [&](const Shot& s, const Shot& base, std::uint32_t x0, std::uint32_t x1) {
        int n = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) n += channel(s.at(x, y), 1) > channel(base.at(x, y), 1) + 2 ? 1 : 0;
        return n;
    };
    Shot black_sky{std::vector<std::uint8_t>(open.px.size(), 0), {}};
    for (std::size_t i = 3; i < black_sky.px.size(); i += 4) black_sky.px[i] = 255;
    const int left_open = streaks(open, black_sky, 0, W / 2 - 8), left_blocked = streaks(blocked, wall_only, 0, W / 2 - 8);
    CHECK(left_open > 200);
    CHECK(left_blocked * 4 < left_open);
    // The open half is untouched by the wall.
    int right = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = W / 2 + 8; x < W; ++x) right = std::max(right, std::abs(channel(open.at(x, y), 1) - channel(blocked.at(x, y), 1)));
    CHECK(right == 0);
}

TEST_CASE("gpu weather: drops are deterministic in time and fixed in the world") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Environment env = weather_env(PrecipitationType::Snow);
    const Shot a = shoot(r, env, nothing, nullptr, weather_settings(), kAhead, 3.0);
    const Shot b = shoot(r, env, nothing, nullptr, weather_settings(), kAhead, 3.0);
    CHECK(a.diff(b) == 0);
    const Shot later = shoot(r, env, nothing, nullptr, weather_settings(), kAhead, 3.5);
    CHECK(a.diff(later) > 20);
    // The drop field repeats every box size (2 x radius) in world space, so a camera moved by
    // exactly one box sees the same drops: they do not follow the camera.
    Camera3D moved = Camera3D::look_at({20, 2, 0}, {20, 2, -10});
    moved.z_far = 200.0f;
    const Shot shifted = shoot(r, env, nothing, nullptr, weather_settings(), moved, 3.0);
    int changed = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) changed += std::abs(channel(a.at(x, y), 1) - channel(shifted.at(x, y), 1)) > 8 ? 1 : 0;
    CHECK(changed < static_cast<int>(W * H / 200));
    // Half a metre sideways the near flakes shift (parallax).
    Camera3D side = Camera3D::look_at({0.5f, 2, 0}, {0.5f, 2, -10});
    side.z_far = 200.0f;
    CHECK(a.diff(shoot(r, env, nothing, nullptr, weather_settings(), side, 3.0)) > 20);
    // Wind changes where the rain is.
    Environment windy = weather_env();
    Environment calm = windy;
    windy.precipitation.wind = {6, 0, 0};
    CHECK(shoot(r, calm, nothing).diff(shoot(r, windy, nothing, "weather_rain_wind")) > 20);
}

TEST_CASE("gpu weather: wet surfaces darken rough dielectrics and leave smooth metal") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "weather_ground").value();
    Material rough;
    rough.base_color = {0.6f, 0.6f, 0.6f, 1};
    rough.roughness = 0.9f;
    Material metal;
    metal.base_color = {0.9f, 0.9f, 0.9f, 1};
    metal.metallic = 1.0f;
    metal.roughness = 0.05f;
    Camera3D down = Camera3D::look_at({0, 6, 4}, {0, 0, -2});
    down.z_far = 200.0f;
    // Rough ground on the left, polished metal on the right.
    auto ground = [&](SceneRenderer& rr) {
        rr.submit(cube, rough, math::Mat4::trs({-25, -1, 0}, {}, {50, 1, 100}));
        rr.submit(cube, metal, math::Mat4::trs({25, -1, 0}, {}, {50, 1, 100}));
    };
    Environment dry = weather_env(PrecipitationType::None);
    dry.ambient_intensity = 0.3f;
    Environment wet = dry;
    wet.wetness = 1.0f;
    const Shot d = shoot(r, dry, ground, "weather_dry", weather_settings(), down);
    const Shot w = shoot(r, wet, ground, "weather_wet", weather_settings(), down);
    CHECK_FALSE(d.stats.surface_wetness);
    CHECK(w.stats.surface_wetness);
    // Ground in the left third: much darker when wet.
    CHECK(w.mean(0, H / 2, W / 3, H) + 20.0 < d.mean(0, H / 2, W / 3, H));
    // Polished metal is neither darker nor smoother (it already is).
    int metal_diff = 0;
    for (std::uint32_t y = H / 2; y < H; ++y)
        for (std::uint32_t x = W * 3 / 4; x < W; ++x) metal_diff = std::max(metal_diff, std::abs(channel(w.at(x, y), 0) - channel(d.at(x, y), 0)));
    CHECK(metal_diff <= 1);
    Environment half = dry;
    half.wetness = 0.5f;
    const Shot h = shoot(r, half, ground, nullptr, weather_settings(), down);
    CHECK(h.mean(0, H / 2, W / 3, H) < d.mean(0, H / 2, W / 3, H));
    CHECK(h.mean(0, H / 2, W / 3, H) > w.mean(0, H / 2, W / 3, H));
}

TEST_CASE("weather: presets, sanitising and Environment component mapping") {
    CHECK(RenderSettings::preset(QualityTier::Low).max_precipitation == 2048);
    CHECK(RenderSettings::preset(QualityTier::Medium).max_precipitation == 8192);
    CHECK(RenderSettings::preset(QualityTier::High).max_precipitation == 16384);
    CHECK(RenderSettings::preset(QualityTier::Ultra).max_precipitation == 32768);
    RenderSettings big;
    big.max_precipitation = 1u << 30;
    CHECK(big.sanitized().max_precipitation == SceneRenderer::kMaxPrecipitation);

    gameplay::EnvironmentComponent ec;
    render::Environment env;
    render::DirectionalLight sun;
    runtime::apply_environment(ec, env, sun);
    CHECK(env.precipitation.type == PrecipitationType::None);
    CHECK(env.wetness == 0.0f);
    ec.precipitation = gameplay::EnvironmentComponent::Precipitation::Snow;
    ec.precipitation_intensity = 4.0f;  // clamped
    ec.precipitation_wind = {3, 0, std::nanf("")};
    ec.precipitation_radius = 0.5f;     // at least 2 m
    ec.precipitation_speed = 2.5f;
    ec.precipitation_brightness = -1.0f;
    ec.wetness = 0.4f;
    runtime::apply_environment(ec, env, sun);
    CHECK(env.precipitation.type == PrecipitationType::Snow);
    CHECK(env.precipitation.intensity == 1.0f);
    CHECK(env.precipitation.wind.x == 3.0f);
    CHECK(env.precipitation.wind.z == 0.0f);
    CHECK(env.precipitation.radius == 2.0f);
    CHECK(env.precipitation.fall_speed == 2.5f);
    CHECK(env.precipitation.brightness == 0.0f);
    CHECK(env.wetness == doctest::Approx(0.4f));
    ec.precipitation = gameplay::EnvironmentComponent::Precipitation::Rain;
    ec.wetness = 7.0f;
    runtime::apply_environment(ec, env, sun);
    CHECK(env.precipitation.type == PrecipitationType::Rain);
    CHECK(env.wetness == 1.0f);
}
