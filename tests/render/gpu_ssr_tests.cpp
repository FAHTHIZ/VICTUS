// SPDX-License-Identifier: Apache-2.0
// Screen-space reflections on opaque glossy surfaces (ADR-0199): traced in the forward shader
// against last frame's TAA-resolved colour and linear depth history, or without TAA against the
// history capture pass (ADR-0250).
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
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

    // ADR-0250: the Medium tier has no TAA; a capture pass keeps last frame's colour and depth.
    RenderSettings med = RenderSettings::preset(QualityTier::Medium);
    med.bloom = false;
    REQUIRE_FALSE(med.taa);
    RenderSettings med_off = med;
    med_off.ssr_max_roughness = 0.0f;
    (void)render(med, 1, "opaque_ssr_medium_first");
    CHECK(feature_state(r, "ssr_opaque") == PathState::Off);
    CHECK(r.stats().ssr_history_capture);
    const auto med_with = render(med, 4, "opaque_ssr_medium");
    CHECK(r.stats().opaque_ssr);
    CHECK(r.stats().ssr_history_capture);
    CHECK(feature_state(r, "ssr_opaque") == PathState::Active);
    // Nothing in the frame reads the capture, so the graph must not cull it: a culled pass leaves
    // the history from earlier frames, which a same-size static scene would hide.
    const auto& passes = r.graph_info().executed_passes;
    CHECK(std::find(passes.begin(), passes.end(), "ssr_history") != passes.end());
    const auto med_without = render(med_off, 4, "opaque_ssr_medium_off");
    CHECK_FALSE(r.stats().opaque_ssr);
    CHECK_FALSE(r.stats().ssr_history_capture);
    const int med_on = green_floor(med_with), med_lit_off = green_floor(med_without);
    const std::uint32_t med_below = test::pixel(med_with, W, W / 2, foot + 4), med_below_off = test::pixel(med_without, W, W / 2, foot + 4);
    MESSAGE("Medium: green floor pixels with SSR " << med_on << ", without " << med_lit_off << "; below foot g "
                                                   << channel(med_below, 1) << " vs " << channel(med_below_off, 1));
    CHECK(med_on > med_lit_off + 100);
    CHECK(channel(med_below, 1) > channel(med_below_off, 1) + 40);
    ctx.destroy(cube);
}

TEST_CASE("gpu opaque ssr: capture mode has no history, so it does not trace (ADR-0250)") {
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
    r.set_capture_mode(true); // deterministic captures keep no frame-to-frame state
    frame(RenderSettings::preset(QualityTier::Medium));
    frame(RenderSettings::preset(QualityTier::Medium));
    CHECK_FALSE(r.stats().opaque_ssr);
    CHECK_FALSE(r.stats().ssr_history_capture);
    const auto* f = [&]() -> const PathFeature* {
        for (const auto& x : r.path_report().features)
            if (x.name == "ssr_opaque") return &x;
        return nullptr;
    }();
    REQUIRE(f != nullptr);
    CHECK(f->state == PathState::Off);
    CHECK(f->reason == PathReason::CaptureMode);
    ctx.device().destroy(target);
    ctx.destroy(cube);
}

TEST_CASE("gpu opaque ssr: glossy walls reflect facades seen at a grazing angle (ADR-0241)") {
    // A street: a glossy black wall on the left mirrors a glowing facade on the right that runs
    // away from the camera. Its depth changes quickly from pixel to pixel, so many reflected rays
    // land far behind the stored surface; they reflect the first such surface instead of nothing.
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    if (dev.capabilities().backend == rhi::Backend::GLES) return; // no opaque SSR on GLES
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "ssr_cube").value();
    Material mirror;
    mirror.base_color = {0.9f, 0.9f, 0.9f, 1};
    mirror.metallic = 1.0f;
    mirror.roughness = 0.05f;
    Material green;
    green.base_color = {0, 0, 0, 1};
    green.emissive = {0.1f, 1.0f, 0.1f, 1};
    green.emissive_strength = 3.0f;
    Material ground;
    ground.base_color = {0.05f, 0.05f, 0.05f, 1};
    ground.roughness = 1.0f;
    const math::Mat4 left = math::Mat4::trs({-4.0f, 4, -40}, {}, {0.4f, 8, 80});
    const math::Mat4 right = math::Mat4::trs({4.0f, 4, -40}, {}, {0.4f, 8, 80});
    const math::Mat4 floor_m = math::Mat4::trs({0, -0.5f, -40}, {}, {20, 1, 80});
    const Camera3D cam = Camera3D::look_at({0, 1.6f, 2}, {0, 1.6f, -20});
    const auto render = [&](RenderSettings s, const char* save) {
        r.set_settings(s);
        r.reset_history();
        auto target = test::make_target(W, H);
        for (int i = 0; i < 16; ++i) {
            DirectionalLight sun;
            sun.direction = {0.3f, -1, -0.2f};
            sun.intensity = 0.2f;
            sun.shadows = false;
            r.begin(cam, sun, dark_env());
            r.submit(cube, mirror, left);
            r.submit(cube, green, right);
            r.submit(cube, ground, floor_m);
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
        }
        auto px = dev.read_texture(target).value();
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
    const auto with = render(on, "opaque_ssr_grazing"), without = render(off, "opaque_ssr_grazing_off");
    // Left third of the image: the mirror wall. Count green pixels and the mean absolute
    // difference between horizontal neighbours there (dither from rays flipping hit / miss).
    int green_on = 0, green_off = 0;
    double grain = 0;
    int pairs = 0;
    for (std::uint32_t y = H / 4; y < H * 3 / 4; ++y)
        for (std::uint32_t x = 2; x < W / 3; ++x) {
            const std::uint32_t c = test::pixel(with, W, x, y), o = test::pixel(without, W, x, y);
            if (channel(c, 1) > 60 && channel(c, 1) > channel(c, 0) + 30) ++green_on;
            if (channel(o, 1) > 60 && channel(o, 1) > channel(o, 0) + 30) ++green_off;
            const std::uint32_t p = test::pixel(with, W, x - 1, y);
            grain += std::abs(static_cast<int>(channel(c, 1)) - static_cast<int>(channel(p, 1)));
            ++pairs;
        }
    grain /= pairs;
    MESSAGE("mirror wall: green pixels with SSR " << green_on << ", without " << green_off << "; neighbour difference " << grain);
    CHECK(green_on > green_off + 200);
    CHECK(grain < 5.0); // 12.3 without the fallback, 0.96 with it (lavapipe)
    ctx.destroy(cube);
}
