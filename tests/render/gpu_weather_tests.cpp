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

TEST_CASE("gpu weather: a roof stops drops and keeps the ground beneath it dry") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "weather_roof").value();
    Material black;
    black.base_color = {0, 0, 0, 1};
    black.roughness = 1.0f;
    // A thin black roof 6 m up over the left half of the world (x < 0), out of view above the
    // camera, which looks level at 2 m: drops under it (left half of the image) must vanish.
    auto roof = [&](SceneRenderer& rr) { rr.submit(cube, black, math::Mat4::trs({-30, 6, 0}, {}, {60, 0.2f, 60})); };
    const Shot open = shoot(r, weather_env(), nothing);
    const Shot covered = shoot(r, weather_env(), roof, "weather_roof");
    CHECK(covered.stats.precipitation_occlusion == 1024);
    CHECK(covered.stats.precipitation_occluders >= 1);
    CHECK(open.stats.precipitation_occluders == 0);
    // Pixels a drop brightened over the same scene without rain (the roof's underside is visible).
    const Shot roof_only = shoot(r, weather_env(PrecipitationType::None), roof);
    const auto drops = [&](const Shot& s, std::uint32_t x0, std::uint32_t x1) {
        int n = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = x0; x < x1; ++x) n += channel(s.at(x, y), 1) > channel(roof_only.at(x, y), 1) + 2 ? 1 : 0;
        return n;
    };
    const int left_open = open.lit(0, 0, W / 2 - 8, H), left_covered = drops(covered, 0, W / 2 - 8);
    const int right_open = open.lit(W / 2 + 8, 0, W, H), right_covered = drops(covered, W / 2 + 8, W);
    CHECK(left_open > 200);
    CHECK(left_covered * 10 < left_open);
    CHECK(right_covered * 10 > right_open * 6); // the open side still rains
    // Turned off, the roof no longer shelters anything.
    RenderSettings no_occ = weather_settings();
    no_occ.precipitation_occlusion = 0;
    const Shot leaky = shoot(r, weather_env(), roof, nullptr, no_occ);
    CHECK(leaky.stats.precipitation_occlusion == 0);
    CHECK(drops(leaky, 0, W / 2 - 8) * 10 > left_open * 6);

    // Wet ground: rough grey floor, half of it under the roof. Looking down from below the roof.
    Material grey;
    grey.base_color = {0.6f, 0.6f, 0.6f, 1};
    grey.roughness = 0.9f;
    auto scene = [&](SceneRenderer& rr) {
        rr.submit(cube, grey, math::Mat4::trs({0, -1, 0}, {}, {100, 1, 100}));
        roof(rr);
    };
    Camera3D down = Camera3D::look_at({0, 4, 3}, {0, 0, -1});
    down.z_far = 200.0f;
    Environment wet = weather_env();
    wet.precipitation.opacity = 1e-4f; // drops are discarded: only the surfaces show
    wet.ambient_intensity = 0.3f;
    wet.wetness = 1.0f;
    Environment dry = wet;
    dry.wetness = 0.0f;
    const Shot d = shoot(r, dry, scene, nullptr, weather_settings(), down);
    const Shot w = shoot(r, wet, scene, "weather_roof_wet", weather_settings(), down);
    CHECK(w.stats.surface_wetness);
    // Exposed ground (right) darkens; sheltered ground (left) stays as it was.
    CHECK(w.mean(W * 3 / 4, H / 2, W, H) + 15.0 < d.mean(W * 3 / 4, H / 2, W, H));
    CHECK(std::abs(w.mean(0, H / 2, W / 4, H) - d.mean(0, H / 2, W / 4, H)) < 2.0);
    // Without the occlusion map both halves get wet.
    const Shot w_leaky = shoot(r, wet, scene, nullptr, no_occ, down);
    CHECK(w_leaky.mean(0, H / 2, W / 4, H) + 15.0 < d.mean(0, H / 2, W / 4, H));
}

namespace {
// Rough ground lit by a sun and a sky that is bright above and dark at the horizon, so a smoother
// or tilted surface reflects visibly different light. A wall stands at the back.
Environment puddle_env(PrecipitationType type = PrecipitationType::None) {
    Environment env = weather_env(type);
    env.sky_zenith = {1, 1, 1, 1};
    env.sky_horizon = {0.08f, 0.08f, 0.08f, 1};
    env.ground = {0.02f, 0.02f, 0.02f, 1};
    env.ambient_intensity = 0.6f;
    env.precipitation.opacity = 1e-4f; // drops are discarded: only the surfaces show
    return env;
}
} // namespace

TEST_CASE("gpu weather: puddles collect on flat ground and rain ripples them") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "puddle_ground").value();
    Material rough;
    rough.base_color = {0.5f, 0.45f, 0.4f, 1};
    rough.roughness = 0.9f;
    // Ground below, a wall across the top of the view.
    auto scene = [&](SceneRenderer& rr) {
        rr.submit(cube, rough, math::Mat4::trs({0, -1, 0}, {}, {100, 1, 100}));
        rr.submit(cube, rough, math::Mat4::trs({0, 5, -8}, {}, {100, 10, 1}));
    };
    Camera3D cam = Camera3D::look_at({0, 3, 4}, {0, 0.5f, -4});
    cam.z_far = 200.0f;
    const auto changed = [&](const Shot& a, const Shot& b, std::uint32_t y0, std::uint32_t y1, int by) {
        int n = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = 0; x < W; ++x) n += std::abs(channel(a.at(x, y), 1) - channel(b.at(x, y), 1)) > by ? 1 : 0;
        return n;
    };
    const std::uint32_t ground_y0 = H * 2 / 3, wall_y1 = H / 6;
    const int ground_px = static_cast<int>(W * (H - ground_y0));
    const Shot dry = shoot(r, puddle_env(), scene, "weather_puddles_dry", weather_settings(), cam);
    CHECK_FALSE(dry.stats.surface_puddles);
    Environment some = puddle_env();
    some.puddles = 0.5f;
    const Shot half = shoot(r, some, scene, "weather_puddles", weather_settings(), cam);
    CHECK(half.stats.surface_puddles);
    CHECK_FALSE(half.stats.rain_ripples);
    const int half_changed = changed(dry, half, ground_y0, H, 8);
    CHECK(half_changed > ground_px / 10);
    CHECK(half_changed < ground_px * 9 / 10);
    CHECK(changed(dry, half, 0, wall_y1, 1) == 0); // walls hold no water
    Environment all = puddle_env();
    all.puddles = 1.0f;
    const Shot full = shoot(r, all, scene, nullptr, weather_settings(), cam);
    CHECK(changed(dry, full, ground_y0, H, 8) > half_changed);
    // Puddle size changes where they are.
    Environment small = some;
    small.puddle_size = 1.0f;
    CHECK(changed(half, shoot(r, small, scene, nullptr, weather_settings(), cam), ground_y0, H, 8) > ground_px / 20);

    // Rain animates ripples on the water; frames at the same time match. A low sun ahead puts a
    // glint on the water that the ripples break up.
    Camera3D close = Camera3D::look_at({0, 1.5f, 1}, {0, 0, -1.5f});
    close.z_far = 200.0f;
    DirectionalLight glint = sun_light(6.0f);
    glint.direction = math::normalize(math::Vec3{0, -0.35f, 1});
    Environment rain = puddle_env(PrecipitationType::Rain);
    rain.puddles = 1.0f;
    const Shot r1 = shoot(r, rain, scene, "weather_ripples", weather_settings(), close, 1.0, glint);
    const Shot r1b = shoot(r, rain, scene, nullptr, weather_settings(), close, 1.0, glint);
    const Shot r2 = shoot(r, rain, scene, nullptr, weather_settings(), close, 1.3, glint);
    CHECK(r1.stats.rain_ripples);
    CHECK(r1.diff(r1b) == 0);
    CHECK(changed(r1, r2, 0, H, 4) > static_cast<int>(W * H / 100));
    Environment still = rain;
    still.precipitation.ripples = 0.0f;
    CHECK(changed(r1, shoot(r, still, scene, nullptr, weather_settings(), close, 1.0, glint), 0, H, 4) > static_cast<int>(W * H / 100));
    // Snow, ripples set to 0 and the precipitation setting off all leave the water still.
    Environment snow = puddle_env(PrecipitationType::Snow);
    snow.puddles = 1.0f;
    const Shot s1 = shoot(r, snow, scene, nullptr, weather_settings(), close, 1.0, glint);
    CHECK_FALSE(s1.stats.rain_ripples);
    CHECK(changed(s1, shoot(r, snow, scene, nullptr, weather_settings(), close, 1.3, glint), 0, H, 0) == 0);
    const Environment& calm = still;
    const Shot c1 = shoot(r, calm, scene, nullptr, weather_settings(), close, 1.0, glint);
    CHECK_FALSE(c1.stats.rain_ripples);
    CHECK(changed(c1, shoot(r, calm, scene, nullptr, weather_settings(), close, 1.3, glint), 0, H, 0) == 0);
    RenderSettings no_rain = weather_settings();
    no_rain.precipitation = false;
    CHECK_FALSE(shoot(r, rain, scene, nullptr, no_rain, close, 1.0, glint).stats.rain_ripples);
}

TEST_CASE("gpu weather: rain splashes where drops land") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh cube = ctx.create_mesh(make_cube(), "splash_ground").value();
    Material black;
    black.base_color = {0, 0, 0, 1};
    black.roughness = 1.0f;
    black.metallic = 1.0f; // no specular from the white sky either: only splashes and drops show
    auto ground = [&](SceneRenderer& rr) { rr.submit(cube, black, math::Mat4::trs({0, -0.5f, 0}, {}, {100, 1, 100})); };
    Environment rain = weather_env();
    Environment dry_ground = rain;
    dry_ground.precipitation.splashes = 0.0f;
    const auto brighter = [&](const Shot& a, const Shot& b, std::uint32_t y0, std::uint32_t y1) {
        int n = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = 0; x < W; ++x) n += channel(a.at(x, y), 1) > channel(b.at(x, y), 1) + 2 ? 1 : 0;
        return n;
    };
    const Shot with = shoot(r, rain, ground, "weather_splashes", weather_settings(), kAhead);
    const Shot without = shoot(r, dry_ground, ground, nullptr, weather_settings(), kAhead);
    CHECK(with.stats.precipitation_splashes == 4096); // a quarter of the High drop count
    CHECK(without.stats.precipitation_splashes == 0);
    // Splashes sit on the ground (below the horizon at mid height), never in the sky.
    CHECK(brighter(with, without, H / 2 + 2, H) > 30);
    CHECK(brighter(with, without, 0, H / 2 - 4) == 0);
    // With nothing below (no ground in the occlusion map) splashes are dropped.
    const Shot open_with = shoot(r, rain, nothing);
    const Shot open_without = shoot(r, dry_ground, nothing);
    CHECK(open_with.diff(open_without) == 0);
    // Splashes come and go.
    const Shot later = shoot(r, rain, ground, nullptr, weather_settings(), kAhead, 1.15);
    CHECK(brighter(later, with, H / 2 + 2, H) > 10);
    // Snow, no occlusion map, or splashes 0: none.
    CHECK(shoot(r, weather_env(PrecipitationType::Snow), ground).stats.precipitation_splashes == 0);
    RenderSettings no_occ = weather_settings();
    no_occ.precipitation_occlusion = 0;
    CHECK(shoot(r, rain, ground, nullptr, no_occ).stats.precipitation_splashes == 0);
}

TEST_CASE("weather: presets, sanitising and Environment component mapping") {
    CHECK(RenderSettings::preset(QualityTier::Low).max_precipitation == 2048);
    CHECK(RenderSettings::preset(QualityTier::Medium).max_precipitation == 8192);
    CHECK(RenderSettings::preset(QualityTier::High).max_precipitation == 16384);
    CHECK(RenderSettings::preset(QualityTier::Ultra).max_precipitation == 32768);
    RenderSettings big;
    big.max_precipitation = 1u << 30;
    CHECK(big.sanitized().max_precipitation == SceneRenderer::kMaxPrecipitation);
    CHECK(RenderSettings::preset(QualityTier::Low).precipitation_occlusion == 256);
    CHECK(RenderSettings::preset(QualityTier::Medium).precipitation_occlusion == 512);
    CHECK(RenderSettings::preset(QualityTier::High).precipitation_occlusion == 1024);
    CHECK(RenderSettings::preset(QualityTier::Ultra).precipitation_occlusion == 1024);
    big.precipitation_occlusion = 3;
    CHECK(big.sanitized().precipitation_occlusion == 64);
    big.precipitation_occlusion = 1u << 20;
    CHECK(big.sanitized().precipitation_occlusion == 4096);
    big.precipitation_occlusion = 0;
    CHECK(big.sanitized().precipitation_occlusion == 0);

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
    CHECK(env.puddles == 0.0f);
    CHECK(env.puddle_size == 3.0f);
    CHECK(env.precipitation.ripples == 1.0f);
    ec.puddles = 0.3f;
    ec.puddle_size = 0.01f;            // at least 0.25 m
    ec.precipitation_ripples = 2.0f;   // clamped
    ec.precipitation_splashes = -1.0f; // clamped
    runtime::apply_environment(ec, env, sun);
    CHECK(env.precipitation.splashes == 0.0f);
    CHECK(env.puddles == doctest::Approx(0.3f));
    CHECK(env.puddle_size == 0.25f);
    CHECK(env.precipitation.ripples == 1.0f);
    ec.precipitation = gameplay::EnvironmentComponent::Precipitation::Rain;
    ec.wetness = 7.0f;
    runtime::apply_environment(ec, env, sun);
    CHECK(env.precipitation.type == PrecipitationType::Rain);
    CHECK(env.wetness == 1.0f);
}
