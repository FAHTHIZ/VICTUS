// SPDX-License-Identifier: Apache-2.0
// Screen-space reflections on opaque glossy surfaces (ADR-0199): traced in the forward shader
// against last frame's TAA-resolved colour and linear depth history.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <optional>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

Environment dark_env() {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {0.05f, 0.05f, 0.05f, 1};
    env.ambient_intensity = 1.0f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    return env;
}

std::optional<PathState> feature_state(const SceneRenderer& r, std::string_view name) {
    for (const auto& f : r.path_report().features)
        if (f.name == name) return f.state;
    return std::nullopt;
}

} // namespace

TEST_CASE("gpu opaque ssr: a glossy floor mirrors a glowing pillar from the TAA history") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "ssr_cube").value();
    Material floor;
    floor.base_color = {0.9f, 0.9f, 0.9f, 1};
    floor.metallic = 1.0f;
    floor.roughness = 0.05f;
    Material green;
    green.base_color = {0, 0, 0, 1};
    green.emissive = {0.1f, 1.0f, 0.1f, 1};
    green.emissive_strength = 3.0f;
    const math::Mat4 floor_m = math::Mat4::trs({0, -0.5f, 0}, {}, {30, 1, 30});
    const math::Mat4 pillar = math::Mat4::trs({0, 1.5f, -6}, {}, {1.5f, 3.0f, 0.6f});
    const Camera3D cam = Camera3D::look_at({0, 1.0f, 4}, {0, 0.5f, -6});

    const auto render = [&](RenderSettings s, int frames, const char* save) {
        r.set_settings(s);
        r.reset_history();
        std::vector<std::uint8_t> px;
        auto target = test::make_target(W, H);
        for (int i = 0; i < frames; ++i) {
            DirectionalLight sun;
            sun.direction = {0.3f, -1, -0.2f};
            sun.intensity = 0.2f;
            sun.shadows = false;
            r.begin(cam, sun, dark_env());
            r.submit(cube, floor, floor_m);
            r.submit(cube, green, pillar);
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
        }
        px = dev.read_texture(target).value();
        test::save_output(save, W, H, px);
        dev.destroy(target);
        return px;
    };
    RenderSettings on = RenderSettings::preset(QualityTier::High);
    on.bloom = false;
    on.ssao = false;
    on.taa = true;
    on.screen_space_reflections = true;
    RenderSettings off = on;
    off.ssr_max_roughness = 0.0f;

    if (dev.capabilities().backend == rhi::Backend::GLES) {
        (void)render(on, 3, "opaque_ssr_gles");
        CHECK_FALSE(r.stats().opaque_ssr);
        CHECK(feature_state(r, "ssr_opaque") == PathState::Off);
        ctx.destroy(cube);
        return;
    }
    // Frame one has no history: the floor reflects the environment only.
    (void)render(on, 1, "opaque_ssr_first");
    CHECK(feature_state(r, "ssr_opaque") == PathState::Off);
    const auto with = render(on, 12, "opaque_ssr");
    CHECK(r.stats().opaque_ssr);
    CHECK(feature_state(r, "ssr_opaque") == PathState::Active);
    const auto without = render(off, 12, "opaque_ssr_off");
    CHECK_FALSE(r.stats().opaque_ssr);
    CHECK(feature_state(r, "ssr_opaque") == PathState::Off);

    // The pillar's lowest row in the centre column (green without SSR).
    std::uint32_t foot = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        if (channel(test::pixel(without, W, W / 2, y), 1) > 100) foot = y;
    REQUIRE(foot > 0);
    REQUIRE(foot + 4 < H);
    const auto green_floor = [&](const std::vector<std::uint8_t>& px) {
        int n = 0;
        for (std::uint32_t y = foot + 2; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint32_t c = test::pixel(px, W, x, y);
                if (channel(c, 1) > 60 && channel(c, 1) > channel(c, 0) + 30) ++n;
            }
        return n;
    };
    const int lit_on = green_floor(with), lit_off = green_floor(without);
    const std::uint32_t below = test::pixel(with, W, W / 2, foot + 4), below_off = test::pixel(without, W, W / 2, foot + 4);
    MESSAGE("pillar foot row " << foot << ": green floor pixels with SSR " << lit_on << ", without " << lit_off << "; below foot g "
                               << channel(below, 1) << " vs " << channel(below_off, 1));
    CHECK(lit_on > lit_off + 100);
    CHECK(channel(below, 1) > channel(below_off, 1) + 40);
    ctx.destroy(cube);
}

TEST_CASE("gpu opaque ssr: the Medium tier (no TAA) does not trace") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "ssr_cube").value();
    Material m;
    auto target = test::make_target(W, H);
    const auto frame = [&](RenderSettings s) {
        r.set_settings(s);
        r.begin(Camera3D::look_at({0, 2, 4}, {0, 0, 0}), DirectionalLight{}, dark_env());
        r.submit(cube, m, math::Mat4::identity());
        auto cmd = ctx.device().begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    };
    frame(RenderSettings::preset(QualityTier::Medium)); // no TAA
    CHECK_FALSE(r.stats().opaque_ssr);
    const auto* f = [&]() -> const PathFeature* {
        for (const auto& x : r.path_report().features)
            if (x.name == "ssr_opaque") return &x;
        return nullptr;
    }();
    REQUIRE(f != nullptr);
    CHECK(f->state == PathState::Off);
    CHECK(f->reason == PathReason::Requires);
    ctx.device().destroy(target);
    ctx.destroy(cube);
}
