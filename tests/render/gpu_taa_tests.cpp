// SPDX-License-Identifier: Apache-2.0
// Temporal anti-aliasing: a static scene converges to anti-aliased edges without drifting,
// history follows camera motion without ghosting, and history state is tracked per frame.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/temporal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 96, H = 96;

struct Scene {
    std::unique_ptr<SceneRenderer> renderer;
    GpuMesh cube;
};

Scene make_scene(bool taa, float render_scale = 1.0f, bool temporal_upscale = true) {
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube({1, 1, 1}), "taa_cube");
    REQUIRE(cube.ok());
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = taa;
    s.fxaa = false;
    s.ssao = false;
    s.bloom = false;
    s.render_scale = render_scale;
    s.temporal_upscale = temporal_upscale;
    (*sr)->set_settings(s);
    return {std::move(*sr), *cube};
}

std::vector<std::uint8_t> frame(Scene& sc, float camera_x) {
    auto& dev = gpu().ctx->device();
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    sc.renderer->begin(Camera3D::look_at({camera_x, 0, 6}, {camera_x, 0, 0}, {0, 1, 0}), sun, env);
    Material white;
    white.unlit = true;
    white.base_color = {1, 1, 1, 1};
    const math::Quat spin = math::Quat::axis_angle({0, 0, 1}, 0.45f);
    sc.renderer->submit(sc.cube, white, math::Mat4::trs({0, 0, 0}, spin, {1, 1, 0.01f}));
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sc.renderer->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    return px;
}

int red(const std::vector<std::uint8_t>& img, std::uint32_t x, std::uint32_t y) { return channel(pixel(img, W, x, y), 0); }

int intermediate_pixels(const std::vector<std::uint8_t>& img) {
    int n = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const int v = red(img, x, y);
            if (v > 24 && v < 231) ++n;
        }
    return n;
}

std::size_t distinct_levels(const std::vector<std::uint8_t>& img) {
    std::array<bool, 256> seen{};
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) seen[static_cast<std::size_t>(red(img, x, y))] = true;
    return static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
}

double mean_abs_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) sum += std::abs(red(a, x, y) - red(b, x, y));
    return sum / (W * H);
}
} // namespace

TEST_CASE("taa jitter sequence and projection offset") {
    CHECK(halton(1, 2) == doctest::Approx(0.5f));
    CHECK(halton(2, 2) == doctest::Approx(0.25f));
    CHECK(halton(3, 3) == doctest::Approx(1.0f / 9.0f));
    math::Vec2 mean{};
    for (std::uint64_t f = 0; f < 8; ++f) {
        const math::Vec2 j = taa_jitter(f);
        CHECK(std::abs(j.x) <= 0.5f);
        CHECK(std::abs(j.y) <= 0.5f);
        mean.x += j.x / 8;
        mean.y += j.y / 8;
    }
    CHECK(std::abs(mean.x) < 0.07f);
    CHECK(std::abs(mean.y) < 0.07f);
    CHECK(taa_jitter(3).x == taa_jitter(11).x); // period 8
    const math::Mat4 proj = math::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f);
    const math::Mat4 j = jitter_projection(proj, {0.5f, -0.25f}, 100, 50);
    for (float z : {-1.0f, -10.0f, -60.0f}) {
        const math::Vec4 a = proj * math::Vec4(0.3f, 0.2f, z, 1), b = j * math::Vec4(0.3f, 0.2f, z, 1);
        CHECK(b.x / b.w - a.x / a.w == doctest::Approx(2.0f * 0.5f / 100.0f));
        CHECK(b.y / b.w - a.y / a.w == doctest::Approx(2.0f * -0.25f / 50.0f));
        CHECK(b.z / b.w == doctest::Approx(a.z / a.w));
    }
}

TEST_CASE("dynamic resolution lowers scale under load and recovers slowly") {
    DynamicResolutionSettings s;
    s.target_frame_ms = 16.0f;
    s.min_scale = 0.5f;
    s.max_scale = 1.0f;
    DynamicResolution dr(s);
    CHECK(dr.scale() == doctest::Approx(1.0f));
    for (int i = 0; i < 10; ++i) (void)dr.update(10.0f);
    CHECK(dr.scale() == doctest::Approx(1.0f)); // already at max
    // A GPU-bound frame twice as slow as the target: pixel count should roughly halve.
    for (int i = 0; i < 30; ++i) (void)dr.update(32.0f);
    CHECK(dr.scale() < 0.75f);
    CHECK(dr.scale() >= 0.5f);
    for (int i = 0; i < 200; ++i) (void)dr.update(40.0f);
    CHECK(dr.scale() == doctest::Approx(0.5f)); // clamped
    const float low = dr.scale();
    for (int i = 0; i < 29; ++i) (void)dr.update(8.0f);
    CHECK(dr.scale() == doctest::Approx(low)); // recovery waits for sustained headroom
    for (int i = 0; i < 400; ++i) (void)dr.update(8.0f);
    CHECK(dr.scale() == doctest::Approx(1.0f));
    CHECK(dr.update(std::nanf("")) == doctest::Approx(1.0f));
    CHECK(dr.update(-1.0f) == doctest::Approx(1.0f));
    dr.reset(3.0f);
    CHECK(dr.scale() == doctest::Approx(1.0f));
}

TEST_CASE("render settings: TAA is on at High/Ultra and its blend is clamped") {
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).taa);
    CHECK(RenderSettings::preset(QualityTier::High).taa);
    CHECK(RenderSettings::preset(QualityTier::Ultra).taa);
    RenderSettings s;
    s.taa_blend = 0.0f;
    CHECK(s.sanitized().taa_blend == doctest::Approx(0.04f));
    s.taa_blend = std::nanf("");
    CHECK(s.sanitized().taa_blend == doctest::Approx(0.1f));
}

TEST_CASE("gpu: taa converges to anti-aliased edges without shifting a static image") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene plain = make_scene(false);
    const auto aliased = frame(plain, 0.0f);
    Scene taa = make_scene(true);
    std::vector<std::uint8_t> resolved;
    for (int i = 0; i < 24; ++i) resolved = frame(taa, 0.0f);
    test::save_output("taa_aliased", W, H, aliased);
    test::save_output("taa_resolved", W, H, resolved);
    const int hard = intermediate_pixels(aliased), soft = intermediate_pixels(resolved);
    const std::size_t levels_hard = distinct_levels(aliased), levels_soft = distinct_levels(resolved);
    MESSAGE("partial-coverage pixels: aliased " << hard << " taa " << soft << "; grey levels " << levels_hard << " vs "
                                                << levels_soft);
    CHECK(levels_hard <= 4);  // binary coverage (plus a fixed seam shade)
    CHECK(levels_soft > 24);  // gradients along the edges (lavapipe ~90, Apple Metal 37)
    CHECK(soft > 2 * hard);
    CHECK(red(resolved, W / 2, H / 2) > 240); // interior stays solid
    CHECK(red(resolved, 4, 4) < 8);           // background stays black
    CHECK(mean_abs_diff(aliased, resolved) < 6.0);
    CHECK(taa.renderer->stats().taa);
    CHECK(taa.renderer->stats().taa_history);
    gpu().ctx->destroy(plain.cube);
    gpu().ctx->destroy(taa.cube);
}

TEST_CASE("gpu: taa reprojects history under camera motion without ghosting") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene sc = make_scene(true);
    for (int i = 0; i < 16; ++i) (void)frame(sc, 0.0f);
    // The camera moves right, so the cube moves left by about a third of the view.
    const auto moved = frame(sc, 1.5f);
    test::save_output("taa_moved", W, H, moved);
    // Where the cube was (now uncovered background) must not keep a white ghost.
    MESSAGE("uncovered pixel " << red(moved, W / 2 + 18, H / 2) << ", moved centre " << red(moved, W / 2 - 24, H / 2));
    CHECK(red(moved, W / 2 + 18, H / 2) < 24);
    CHECK(red(moved, W / 2 - 24, H / 2) > 230);
    std::vector<std::uint8_t> settled;
    for (int i = 0; i < 16; ++i) settled = frame(sc, 1.5f);
    Scene fresh = make_scene(true);
    std::vector<std::uint8_t> reference;
    for (int i = 0; i < 32; ++i) reference = frame(fresh, 1.5f);
    const double diff = mean_abs_diff(settled, reference);
    MESSAGE("settled vs fresh mean abs diff " << diff);
    CHECK(diff < 2.0);
    gpu().ctx->destroy(sc.cube);
    gpu().ctx->destroy(fresh.cube);
}

TEST_CASE("gpu: taa history state, reset and capture mode") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene sc = make_scene(true);
    (void)frame(sc, 0.0f);
    CHECK(sc.renderer->stats().taa);
    CHECK_FALSE(sc.renderer->stats().taa_history);
    const math::Vec2 j0 = sc.renderer->stats().jitter;
    (void)frame(sc, 0.0f);
    CHECK(sc.renderer->stats().taa_history);
    CHECK(sc.renderer->stats().jitter.x != j0.x);
    sc.renderer->reset_history();
    (void)frame(sc, 0.0f);
    CHECK_FALSE(sc.renderer->stats().taa_history);
    sc.renderer->set_capture_mode(true);
    (void)frame(sc, 0.0f);
    CHECK_FALSE(sc.renderer->stats().taa);
    CHECK(sc.renderer->stats().jitter.x == 0.0f);
    sc.renderer->set_capture_mode(false);
    (void)frame(sc, 0.0f);
    CHECK_FALSE(sc.renderer->stats().taa_history); // capture frames invalidate history
    // Dynamic resolution: a lower render scale reuses the history through UV resampling.
    (void)frame(sc, 0.0f);
    RenderSettings s = sc.renderer->settings();
    s.render_scale = 0.75f;
    sc.renderer->set_settings(s);
    const auto scaled = frame(sc, 0.0f);
    CHECK(sc.renderer->stats().taa_history);
    CHECK(sc.renderer->stats().render_width == 72);
    CHECK(red(scaled, W / 2, H / 2) > 240);
    CHECK(red(scaled, 4, 4) < 8);
    gpu().ctx->destroy(sc.cube);
}

TEST_CASE("temporal upscaling jitter period grows with the upscale ratio") {
    CHECK(temporal_jitter_period(1.0f) == 8u);
    CHECK(temporal_jitter_period(0.5f) == 8u);
    CHECK(temporal_jitter_period(1.5f) == 18u);
    CHECK(temporal_jitter_period(2.0f) == 32u);
    CHECK(temporal_jitter_period(4.0f) == 64u);
    CHECK(temporal_jitter_period(std::nanf("")) == 8u);
    CHECK(RenderSettings{}.temporal_upscale);
}

TEST_CASE("gpu: temporal upscaling reconstructs closer to native resolution than bilinear") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene native = make_scene(true, 1.0f);
    Scene bilinear = make_scene(true, 0.5f, false);
    Scene upscaled = make_scene(true, 0.5f, true);
    std::vector<std::uint8_t> ref, bil, up;
    for (int i = 0; i < 64; ++i) {
        ref = frame(native, 0.0f);
        bil = frame(bilinear, 0.0f);
        up = frame(upscaled, 0.0f);
    }
    test::save_output("taa_upscale_native", W, H, ref);
    test::save_output("taa_upscale_bilinear", W, H, bil);
    test::save_output("taa_upscale_temporal", W, H, up);
    CHECK(upscaled.renderer->stats().temporal_upscale);
    CHECK_FALSE(bilinear.renderer->stats().temporal_upscale);
    CHECK_FALSE(native.renderer->stats().temporal_upscale);
    CHECK(upscaled.renderer->stats().render_width == W / 2);
    const double d_bil = mean_abs_diff(ref, bil), d_up = mean_abs_diff(ref, up);
    MESSAGE("mean abs diff to native TAA: bilinear " << d_bil << ", temporal upscale " << d_up);
    CHECK(d_up < d_bil * 0.6); // lavapipe: bilinear 2.17, temporal 0.76
    CHECK(red(up, W / 2, H / 2) > 240);
    CHECK(red(up, 4, 4) < 8);
    // Static image: consecutive converged frames barely change (no shimmering from the long cycle).
    const auto next = frame(upscaled, 0.0f);
    CHECK(mean_abs_diff(up, next) < 1.5);
    gpu().ctx->destroy(native.cube);
    gpu().ctx->destroy(bilinear.cube);
    gpu().ctx->destroy(upscaled.cube);
}

TEST_CASE("gpu: temporal upscaling follows camera motion without ghosting") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene sc = make_scene(true, 0.5f);
    for (int i = 0; i < 32; ++i) (void)frame(sc, 0.0f);
    const auto moved = frame(sc, 1.5f);
    test::save_output("taa_upscale_moved", W, H, moved);
    MESSAGE("uncovered pixel " << red(moved, W / 2 + 18, H / 2) << ", moved centre " << red(moved, W / 2 - 24, H / 2));
    CHECK(red(moved, W / 2 + 18, H / 2) < 24);
    CHECK(red(moved, W / 2 - 24, H / 2) > 230);
    // Switching render scale keeps the output-resolution history (dynamic resolution).
    RenderSettings s = sc.renderer->settings();
    s.render_scale = 0.75f;
    sc.renderer->set_settings(s);
    (void)frame(sc, 1.5f);
    CHECK(sc.renderer->stats().taa_history);
    CHECK(sc.renderer->stats().temporal_upscale);
    // Back to native resolution: the plain TAA path takes over from the larger history.
    s.render_scale = 1.0f;
    sc.renderer->set_settings(s);
    const auto native = frame(sc, 1.5f);
    CHECK_FALSE(sc.renderer->stats().temporal_upscale);
    CHECK(sc.renderer->stats().taa_history);
    CHECK(red(native, W / 2 - 24, H / 2) > 230);
    gpu().ctx->destroy(sc.cube);
}
