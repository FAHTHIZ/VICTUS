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
