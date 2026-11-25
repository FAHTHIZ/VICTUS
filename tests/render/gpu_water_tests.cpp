// SPDX-License-Identifier: Apache-2.0
// Water surfaces (ADR-0095): Gerstner grid, refraction of the opaque scene with Beer-Lambert
// absorption, depth occlusion, frustum culling, the underwater view and the Water component
// through runtime extraction.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/water.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <memory>
#include <optional>

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
    [[nodiscard]] std::uint32_t center() const { return at(W / 2, H / 2); }
};

RenderSettings flat_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::Medium);
    s.bloom = false;
    s.fxaa = false;
    s.taa = false;
    s.ssao = false;
    return s;
}

Environment flat_env() {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {0.3f, 0.3f, 0.3f, 1};
    env.ambient_intensity = 1.0f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    return env;
}

const Camera3D kAbove = Camera3D::look_at({0, 3, 4}, {0, -0.5f, 0});

template <typename F>
Shot shoot(SceneRenderer& r, F&& content, const char* save = nullptr, Camera3D cam = kAbove) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    r.set_settings(flat_settings());
    DirectionalLight sun;
    sun.direction = {0.3f, -1, -0.2f};
    sun.intensity = 1.5f;
    sun.shadows = false;
    r.begin(cam, sun, flat_env());
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

// A red floor whose top face lies at y = -0.5.
struct Floor {
    GpuMesh cube;
    Material red;
    math::Mat4 model = math::Mat4::trs({0, -1.0f, 0}, {}, {30, 1, 30});
    explicit Floor(GpuContext& ctx) : cube(ctx.create_mesh(make_cube(), "water_floor").value()) {
        red.base_color = {0.9f, 0.1f, 0.1f, 1};
    }
    void submit(SceneRenderer& r) const { r.submit(cube, red, model); }
};

WaterSurface calm(float clarity) {
    WaterSurface w;
    w.center = {0, 0, 0};
    w.half_extent = {20, 20};
    w.clarity = clarity;
    w.foam = 0;
    w.resolution = 64;
    gameplay::WaterComponent c;
    c.wave_height = 0.05f;
    const auto waves = gameplay::water_waves(c, 1.0);
    for (std::size_t i = 0; i < 4; ++i)
        w.waves[i] = {waves[i].direction, waves[i].k, waves[i].amplitude, waves[i].q, waves[i].phase};
    return w;
}

int red_minus_blue(std::uint32_t c) { return static_cast<int>(channel(c, 0)) - static_cast<int>(channel(c, 2)); }

} // namespace

TEST_CASE("gpu water: refracts and tints the floor below, clarity controls absorption") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Floor floor(ctx);
    const Shot dry = shoot(r, [&](SceneRenderer& rr) { floor.submit(rr); });
    CHECK(dry.stats.water_surfaces == 0);
    REQUIRE(red_minus_blue(dry.center()) > 60);

    const Shot clear = shoot(r, [&](SceneRenderer& rr) {
        floor.submit(rr);
        rr.add_water(calm(50.0f));
    }, "water_clear");
    CHECK(clear.stats.water_surfaces == 1);
    const Shot murky = shoot(r, [&](SceneRenderer& rr) {
        floor.submit(rr);
        rr.add_water(calm(0.05f));
    }, "water_murky");
    CHECK(murky.stats.water_surfaces == 1);
    // Clear water still shows the red floor; murky water hides it behind the deep colour.
    CHECK(red_minus_blue(clear.center()) > 20);
    CHECK(red_minus_blue(murky.center()) < red_minus_blue(clear.center()) - 20);
    CHECK(clear.center() != dry.center());
}

TEST_CASE("gpu water: occluded by geometry in front, culled outside the frustum, visible from below") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Floor floor(ctx);
    // Behind the camera: culled.
    WaterSurface behind = calm(0.05f);
    behind.center = {0, 0, 60};
    behind.half_extent = {5, 5};
    const Shot culled = shoot(r, [&](SceneRenderer& rr) {
        floor.submit(rr);
        rr.add_water(behind);
    });
    CHECK(culled.stats.water_surfaces == 0);

    // A white block standing in front of the water hides it.
    auto cube = ctx.create_mesh(make_cube(), "water_block").value();
    Material white;
    white.base_color = {0.9f, 0.9f, 0.9f, 1};
    const math::Mat4 block = math::Mat4::trs({0, 1.5f, 2.5f}, {}, {0.6f, 0.6f, 0.6f});
    auto with_block = [&](bool water) {
        return shoot(r, [&](SceneRenderer& rr) {
            floor.submit(rr);
            rr.submit(cube, white, block);
            if (water) rr.add_water(calm(0.05f));
        });
    };
    const Shot a = with_block(false), b = with_block(true);
    // Find the block's projected centre: the brightest pixel row in the column.
    std::uint32_t by = 0;
    int best = -1;
    for (std::uint32_t y = 0; y < H; ++y) {
        const int g = channel(a.at(W / 2, y), 1);
        if (g > best) {
            best = g;
            by = y;
        }
    }
    REQUIRE(best > 120);
    CHECK(a.at(W / 2, by) == b.at(W / 2, by));
    CHECK(a.at(W / 2, H - 6) != b.at(W / 2, H - 6)); // open water near the camera changes

    // Camera under the surface looking up at it: the surface is drawn (two-sided).
    const Shot under = shoot(r, [&](SceneRenderer& rr) {
        floor.submit(rr);
        rr.add_water(calm(5.0f));
    }, "water_under", Camera3D::look_at({0, -0.25f, 2}, {0, 0.5f, -2}));
    CHECK(under.stats.water_surfaces == 1);
}

TEST_CASE("gpu water: Water components render through runtime extraction") {
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
    s.world().get<scene::Transform>(cam)->position = {0, 3, 4};
    s.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, -0.6f);
    const auto ground = s.create("Ground");
    s.world().get<scene::Transform>(ground)->position = {0, -1, 0};
    s.world().get<scene::Transform>(ground)->scale = {30, 1, 30};
    s.world().add<scene::MeshRendererComponent>(ground).primitive = "cube";
    const auto light = s.create("Sun");
    s.world().add<scene::LightComponent>(light).shadows = false;
    const auto sea = s.create("Sea");
    s.world().add<gameplay::WaterComponent>(sea).clarity = 0.1f;
    gameplay::set_water_time(s, 2.0);

    auto frame = [&]() {
        s.update_transforms();
        runtime::SceneViewSettings settings;
        settings.environment = flat_env();
        const runtime::ExtractStats st = runtime::extract_scene(s, cache, r, settings);
        r.set_settings(flat_settings());
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
    CHECK(st.water_surfaces == 1);
    CHECK(shot.stats.water_surfaces == 1);
    s.world().get<gameplay::WaterComponent>(sea)->visible = false;
    const auto [st2, hidden] = frame();
    CHECK(st2.water_surfaces == 0);
    CHECK(hidden.stats.water_surfaces == 0);
    CHECK(hidden.center() != shot.center());
}

TEST_CASE("gpu water: screen-space reflections mirror objects above the surface (ADR-0196)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Floor floor(ctx);
    auto cube = ctx.create_mesh(make_cube(), "ssr_pillar").value();
    Material green;
    green.base_color = {0.0f, 0.0f, 0.0f, 1};
    green.emissive = {0.1f, 1.0f, 0.1f, 1};
    green.emissive_strength = 3.0f;
    // A glowing pillar standing in murky, mirror-calm water, seen at a grazing angle.
    const math::Mat4 pillar = math::Mat4::trs({0, 1.0f, -6}, {}, {1.5f, 3.0f, 0.6f});
    WaterSurface w = calm(0.05f);
    w.roughness = 0.02f;
    w.refraction = 0.0f;
    for (auto& wave : w.waves) wave.amplitude = 0.0f;
    const Camera3D low = Camera3D::look_at({0, 0.7f, 4}, {0, 0.2f, -6});
    const auto render = [&](RenderSettings s, const char* save) {
        r.set_settings(s);
        auto& dev = gpu().ctx->device();
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = {0.3f, -1, -0.2f};
        sun.intensity = 1.0f;
        sun.shadows = false;
        r.begin(low, sun, flat_env());
        floor.submit(r);
        r.submit(cube, green, pillar);
        r.add_water(w);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value(), r.stats()};
        test::save_output(save, W, H, out.px);
        dev.destroy(target);
        return out;
    };
    // Pixels of the water (below the pillar's foot on screen) that look green.
    const auto green_water = [&](const Shot& s, std::uint32_t foot) {
        int n = 0;
        for (std::uint32_t y = foot + 2; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint32_t c = s.at(x, y);
                if (channel(c, 1) > 60 && channel(c, 1) > channel(c, 0) + 30) ++n;
            }
        return n;
    };
    RenderSettings on = flat_settings();
    on.screen_space_reflections = true;
    on.ssr_steps = 32;
    RenderSettings off = on;
    off.screen_space_reflections = false;
    const Shot with = render(on, "water_ssr");
    const auto ssr_state = [&] {
        for (const auto& f : r.path_report().features)
            if (f.name == "ssr") return std::optional<PathState>(f.state);
        return std::optional<PathState>();
    };
    CHECK(ssr_state() == PathState::Reduced); // 32 of Ultra's 48 steps
    const Shot without = render(off, "water_ssr_off");
    // The pillar's lowest visible row: the last row in the centre column that is green without SSR.
    std::uint32_t foot = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        if (channel(without.at(W / 2, y), 1) > 100) foot = y;
    REQUIRE(foot > 0);
    const int lit_on = green_water(with, foot), lit_off = green_water(without, foot);
    const std::uint32_t below = with.at(W / 2, std::min(foot + 6, H - 1)), below_off = without.at(W / 2, std::min(foot + 6, H - 1));
    MESSAGE("pillar foot row " << foot << ": green water pixels with SSR " << lit_on << ", without " << lit_off << "; below foot g "
                               << channel(below, 1) << " vs " << channel(below_off, 1));
    CHECK(lit_off == 0);
    CHECK(lit_on > 100);
    CHECK(channel(below, 1) > channel(below_off, 1) + 40);
    // The pillar itself is unchanged: SSR only touches the water.
    CHECK(with.at(W / 2, foot - 15) == without.at(W / 2, foot - 15));
    CHECK(ssr_state() == PathState::Off); // the last frame had it off
    ctx.destroy(cube);
}

TEST_CASE("gpu water: planar reflections mirror objects off screen and clip what is under water (ADR-0260)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Floor floor(ctx);
    auto cube = ctx.create_mesh(make_cube(), "planar_slab").value();
    Material green, blue;
    green.base_color = blue.base_color = {0, 0, 0, 1};
    green.emissive = {0.1f, 1.0f, 0.1f, 1};
    green.emissive_strength = 20.0f;
    blue.emissive = {0.1f, 0.1f, 1.0f, 1};
    blue.emissive_strength = 20.0f;
    // A glowing slab overhead, above the top of the view (so screen-space reflections cannot find
    // it); a glowing block under murky water, which the reflection must not pick up; and a dark post
    // standing in the water whose submerged part lies on the mirrored rays to the slab, so the
    // slab's reflection survives only if the oblique near plane clips everything under the surface.
    Material dark;
    dark.base_color = {0.02f, 0.02f, 0.02f, 1};
    const math::Mat4 slab = math::Mat4::trs({0, 3.25f, -1.25f}, {}, {3.0f, 0.5f, 1.5f});
    const math::Mat4 sunken = math::Mat4::trs({0, -0.3f, 1.0f}, {}, {1.0f, 0.3f, 1.0f});
    const math::Mat4 post = math::Mat4::trs({0, -0.975f, 3.2f}, {}, {6.0f, 2.05f, 0.6f});
    WaterSurface w = calm(0.02f);
    w.roughness = 0.02f;
    w.refraction = 0.0f;
    for (auto& wave : w.waves) wave.amplitude = 0.0f;
    const math::Vec3 eye{0, 1.0f, 4};
    const float pitch = math::radians(15.0f);
    const Camera3D cam = Camera3D::look_at(eye, eye + math::Vec3{0, -std::sin(pitch), -std::cos(pitch)});
    const auto render = [&](RenderSettings s, const char* save) {
        r.set_settings(s);
        auto& dev = gpu().ctx->device();
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = {0.3f, -1, -0.2f};
        sun.intensity = 1.0f;
        sun.shadows = false;
        r.begin(cam, sun, flat_env());
        floor.submit(r);
        r.submit(cube, green, slab);
        r.submit(cube, blue, sunken);
        r.submit(cube, dark, post);
        r.add_water(w);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value(), r.stats()};
        test::save_output(save, W, H, out.px);
        dev.destroy(target);
        return out;
    };
    const auto count = [&](const Shot& s, int ch) {
        int n = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint32_t c = s.at(x, y);
                const int v = static_cast<int>(channel(c, ch));
                const int others = std::max(static_cast<int>(channel(c, (ch + 1) % 3)), static_cast<int>(channel(c, (ch + 2) % 3)));
                if (v > 60 && v > others + 30) ++n;
            }
        return n;
    };
    const auto planar_feature = [&] {
        const PathFeature* f = r.path_report().find("planar_reflections");
        REQUIRE(f != nullptr);
        return *f;
    };
    RenderSettings on = flat_settings();
    on.screen_space_reflections = true;
    on.planar_reflections = true;
    on.planar_reflection_scale = 0.5f;
    RenderSettings off = on;
    off.planar_reflections = false;

    const Shot with = render(on, "water_planar");
    const PathFeature active = planar_feature();
    const Shot without = render(off, "water_planar_off");
    const PathFeature disabled = planar_feature();
    const int green_on = count(with, 1), green_off = count(without, 1);
    const int blue_on = count(with, 2), blue_off = count(without, 2);
    MESSAGE("green pixels with planar " << green_on << ", SSR only " << green_off << "; blue " << blue_on << " vs " << blue_off
                                        << "; " << active.detail << "; draws " << with.stats.planar_reflection_draws);
    CHECK(green_off == 0);   // the slab is off screen: neither it nor an SSR hit shows
    CHECK(green_on > 150);   // the mirror shows it in the water, past the post's submerged part
    CHECK(blue_on == blue_off); // the sunken block is not reflected
    CHECK(blue_on == 0);
    CHECK(active.state == PathState::Reduced); // half of Ultra's resolution
    CHECK(active.detail.find("80x48") != std::string::npos);
    CHECK(with.stats.planar_reflection_draws == 2); // slab and post; the floor and the sunken block are culled below the plane
    CHECK(without.stats.planar_reflection_draws == 0);
    CHECK(disabled.state == PathState::Off);
    CHECK(disabled.reason == PathReason::QualityTier); // Medium has it off

    // From below the surface there is nothing to mirror.
    const Camera3D under = Camera3D::look_at({0, -0.2f, 4}, {0, -0.2f, -4});
    r.set_settings(on);
    {
        auto& dev = gpu().ctx->device();
        auto target = test::make_target(W, H);
        r.begin(under, DirectionalLight{}, flat_env());
        r.submit(cube, green, slab);
        r.add_water(w);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        dev.destroy(target);
    }
    CHECK(r.stats().planar_reflection_draws == 0);
    CHECK(planar_feature().reason == PathReason::NotNeeded);
    ctx.destroy(cube);
}

TEST_CASE("gpu water: FFT ocean displaces the grid from the uploaded tile and reuses it (ADR-0261)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    ocean::SpectrumDesc desc;
    desc.resolution = 64;
    desc.tile_size = 32;
    desc.wave_height = 2.0f;
    desc.peak_wavelength = 10;
    desc.seed = 5;
    const ocean::Spectrum spectrum(desc);
    auto sea = [&](std::shared_ptr<const ocean::Field> field) {
        WaterSurface w;
        w.center = {0, 0, 0};
        w.half_extent = {20, 20};
        w.clarity = 0.2f;
        w.foam = 0;
        w.resolution = 256;
        w.planar_reflection = false;
        w.ocean = std::move(field);
        return w;
    };
    // Low and level with the surface, outside the volume, looking along it: flat water ends at
    // the horizon row; crests of the spectrum rise above it.
    const Camera3D level = Camera3D::look_at({0, 0.2f, 24}, {0, 0.2f, 0});
    auto above_horizon = [&](const Shot& s) {
        int n = 0;
        for (std::uint32_t y = 0; y + 2 < H / 2; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
                if (channel(s.at(x, y), 0) + channel(s.at(x, y), 1) + channel(s.at(x, y), 2) > 12) ++n;
        return n;
    };
    const Shot flat = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(nullptr)); }, "water_ocean_flat", level);
    CHECK(flat.stats.water_surfaces == 1);
    CHECK(flat.stats.ocean_surfaces == 0);
    CHECK(above_horizon(flat) == 0);

    const auto field = std::make_shared<const ocean::Field>(spectrum.evaluate(3.0));
    REQUIRE(field->max_height > 0.5f);
    const Shot wavy = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(field)); }, "water_ocean_level", level);
    CHECK(wavy.stats.ocean_surfaces == 1);
    CHECK(wavy.stats.ocean_uploads == 1);
    const int crest_pixels = above_horizon(wavy);
    MESSAGE("water pixels above the rest horizon: " << crest_pixels);
    CHECK(crest_pixels > 50);

    // The same evaluation is not uploaded again; the next one is, and the picture moves.
    const Shot again = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(field)); }, nullptr, level);
    CHECK(again.stats.ocean_uploads == 0);
    CHECK(again.px == wavy.px);
    const auto next = std::make_shared<const ocean::Field>(spectrum.evaluate(4.5));
    const Shot moved = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(next)); }, nullptr, level);
    CHECK(moved.stats.ocean_uploads == 1);
    CHECK(moved.px != wavy.px);

    // From above, the tile's normals shade the surface: far more variation than flat water.
    auto spread = [](const Shot& s) {
        int lo = 255, hi = 0;
        for (std::uint32_t y = H / 2; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const int g = channel(s.at(x, y), 1);
                lo = std::min(lo, g);
                hi = std::max(hi, g);
            }
        return hi - lo;
    };
    const Shot top_flat = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(nullptr)); });
    const Shot top_wavy = shoot(r, [&](SceneRenderer& rr) { rr.add_water(sea(field)); }, "water_ocean_above");
    MESSAGE("green spread: flat " << spread(top_flat) << ", ocean " << spread(top_wavy));
    CHECK(spread(top_wavy) > spread(top_flat) + 10);
}

TEST_CASE("gpu water: spectrum Water components render their ocean through runtime extraction (ADR-0261)") {
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
    s.world().get<scene::Transform>(cam)->position = {0, 3, 4};
    s.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, -0.6f);
    const auto light = s.create("Sun");
    s.world().add<scene::LightComponent>(light).shadows = false;
    const auto sea = s.create("Sea");
    auto& w = s.world().add<gameplay::WaterComponent>(sea);
    w.waves = gameplay::WaveModel::Spectrum;
    w.wave_height = 1.0f;
    w.spectrum_tile = 40;
    gameplay::set_water_time(s, 2.0);
    s.update_transforms();
    runtime::SceneViewSettings settings;
    settings.environment = flat_env();
    const runtime::ExtractStats st = runtime::extract_scene(s, cache, r, settings);
    CHECK(st.water_surfaces == 1);
    r.set_settings(flat_settings());
    auto& dev = ctx.device();
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    dev.destroy(target);
    CHECK(r.stats().water_surfaces == 1);
    CHECK(r.stats().ocean_surfaces == 1);
    CHECK(r.stats().ocean_uploads == 1);
}
