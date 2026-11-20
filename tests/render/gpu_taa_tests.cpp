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
    CHECK(d_up < d_bil * 0.6); // lavapipe: bilinear 2.17, temporal 0.97 (0.76 before the ADR-0142 kernel)
    CHECK(red(up, W / 2, H / 2) > 240);
    CHECK(red(up, 4, 4) < 8);
    // Static image: consecutive converged frames barely change (no shimmering from the long cycle).
    const auto next = frame(upscaled, 0.0f);
    CHECK(mean_abs_diff(up, next) < 1.5);
    gpu().ctx->destroy(native.cube);
    gpu().ctx->destroy(bilinear.cube);
    gpu().ctx->destroy(upscaled.cube);
}

TEST_CASE("gpu: temporal upscaling biases texture mips by the upscale ratio (ADR-0141)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene half = make_scene(true, 0.5f, true);
    Scene native = make_scene(true, 1.0f);
    Scene bilinear = make_scene(true, 0.5f, false);
    Scene no_taa = make_scene(false, 0.5f, true);
    (void)frame(half, 0.0f);
    (void)frame(native, 0.0f);
    (void)frame(bilinear, 0.0f);
    (void)frame(no_taa, 0.0f);
    CHECK(half.renderer->stats().texture_mip_bias == doctest::Approx(-1.0f).epsilon(0.02));
    CHECK(native.renderer->stats().texture_mip_bias == 0.0f);
    CHECK(bilinear.renderer->stats().texture_mip_bias == 0.0f);
    CHECK(no_taa.renderer->stats().texture_mip_bias == 0.0f);
    // The extra offset is added on top and clamped to -2..0 by sanitize.
    RenderSettings s = half.renderer->settings();
    s.upscale_mip_bias = -0.5f;
    half.renderer->set_settings(s);
    (void)frame(half, 0.0f);
    CHECK(half.renderer->stats().texture_mip_bias == doctest::Approx(-1.5f).epsilon(0.02));
    s.upscale_mip_bias = 3.0f;
    CHECK(s.sanitized().upscale_mip_bias == 0.0f);
    s.upscale_mip_bias = -9.0f;
    CHECK(s.sanitized().upscale_mip_bias == -2.0f);
    for (Scene* sc : {&half, &native, &bilinear, &no_taa}) gpu().ctx->destroy(sc->cube);
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

namespace {
// ADR-0147 scene: a red occluder 3 units from the camera in front of a wall of 2-pixel red and blue
// stripes 8 units away. A sideways camera move uncovers the wall behind the occluder (parallax).
// Every 3x3 neighbourhood of the uncovered wall holds red, so the colour clip keeps the occluder's
// red history on the blue stripes; only the depth test can tell that history belongs to something nearer.
std::vector<std::uint8_t> stripes_frame(SceneRenderer& r, GpuMesh stripe, GpuMesh box, float camera_x, float pitch = 0.2f) {
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
    r.begin(Camera3D::look_at({camera_x, 0, 6}, {camera_x, 0, 0}, {0, 1, 0}), sun, env);
    Material red, blue;
    red.unlit = blue.unlit = true;
    red.base_color = {1, 0, 0, 1};
    blue.base_color = {0, 0, 1, 1};
    // Stripes `pitch` wide (the mesh is 0.2 wide, scaled), about 2 render pixels at render scale 0.2 / pitch.
    const int n = static_cast<int>(8.0f / pitch);
    for (int i = -n; i < n; ++i)
        r.submit(stripe, (i & 1) ? blue : red,
                 math::Mat4::trs({pitch * static_cast<float>(i) + 0.5f * pitch, 0, -2}, {}, {pitch / 0.2f, 1, 1}));
    r.submit(box, red, math::Mat4::trs({0, 0, 3}, {}, {1, 1, 1}));
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    return px;
}

// Mean |a - b| over RGB, inside `mask` when given.
double mean_abs_rgb(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, const std::vector<bool>* mask = nullptr) {
    double sum = 0;
    std::size_t n = 0;
    for (std::size_t p = 0; p < static_cast<std::size_t>(W) * H; ++p) {
        if (mask && !(*mask)[p]) continue;
        for (std::size_t c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(a[p * 4 + c]) - static_cast<int>(b[p * 4 + c]));
        n += 3;
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

// Slab test: does the segment o + t d, t in [0, t_max], hit the occluder's box?
bool hits_occluder(const math::Vec3& o, const math::Vec3& d, float t_max) {
    const math::Vec3 lo{-0.5f, -0.5f, 2.9f}, hi{0.5f, 0.5f, 3.1f};
    float t0 = 0.0f, t1 = t_max;
    const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, ls[3] = {lo.x, lo.y, lo.z}, hs[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        if (std::abs(ds[k]) < 1e-9f) {
            if (os[k] < ls[k] || os[k] > hs[k]) return false;
            continue;
        }
        float a = (ls[k] - os[k]) / ds[k], b = (hs[k] - os[k]) / ds[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

// Disoccluded wall, exactly: pixels that see the wall (z = -2) from the camera at x = 1.5 where the
// occluder hid that wall point from the camera at x = 0, eroded by a pixel (anti-aliased edges).
std::vector<bool> disoccluded_mask() {
    const float t = std::tan(math::radians(30.0f));
    std::vector<bool> raw(static_cast<std::size_t>(W) * H, false), m(raw.size(), false);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const float nx = (static_cast<float>(x) + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (static_cast<float>(y) + 0.5f) / H * 2.0f;
            const math::Vec3 c{1.5f, 0, 6}, d{nx * t, ny * t, -1};
            if (hits_occluder(c, d, 8.0f)) continue;
            const math::Vec3 wall{c.x + d.x * 8.0f, c.y + d.y * 8.0f, -2.0f}, old{0, 0, 6};
            raw[static_cast<std::size_t>(y) * W + x] = hits_occluder(old, {wall.x - old.x, wall.y - old.y, wall.z - old.z}, 1.0f);
        }
    for (std::uint32_t y = 1; y + 1 < H; ++y)
        for (std::uint32_t x = 1; x + 1 < W; ++x) {
            bool all = true;
            for (std::uint32_t j = y - 1; j <= y + 1; ++j)
                for (std::uint32_t i = x - 1; i <= x + 1; ++i) all = all && raw[static_cast<std::size_t>(j) * W + i];
            m[static_cast<std::size_t>(y) * W + x] = all;
        }
    return m;
}
} // namespace

TEST_CASE("gpu: taa depth disocclusion stops occluder-coloured ghosts (ADR-0147)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto stripe = ctx.create_mesh(make_cube({0.1f, 8, 0.05f}), "stripe");
    auto box = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.1f}), "occluder");
    REQUIRE(stripe.ok());
    REQUIRE(box.ok());
    for (const float scale : {1.0f, 0.5f}) {
        CAPTURE(scale);
        const float pitch = 0.2f / scale;
        const auto run = [&](float strength, bool& active) {
            auto sr = SceneRenderer::create(ctx);
            REQUIRE(sr.ok());
            RenderSettings s = RenderSettings::preset(QualityTier::High);
            s.fxaa = s.ssao = s.bloom = false;
            s.render_scale = scale;
            s.taa_disocclusion = strength;
            (*sr)->set_settings(s);
            for (int i = 0; i < 24; ++i) (void)stripes_frame(**sr, *stripe, *box, 0.0f, pitch);
            std::vector<std::vector<std::uint8_t>> after;
            for (int i = 0; i < 4; ++i) after.push_back(stripes_frame(**sr, *stripe, *box, 1.5f, pitch));
            active = (*sr)->stats().taa_disocclusion;
            return after;
        };
        bool on_active = false, off_active = true;
        const auto on = run(1.0f, on_active);
        const auto off = run(0.0f, off_active);
        CHECK(on_active);
        CHECK_FALSE(off_active);
        // Reference: converged at the final camera with a fresh history.
        auto fresh = SceneRenderer::create(ctx);
        REQUIRE(fresh.ok());
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.fxaa = s.ssao = s.bloom = false;
        s.render_scale = scale;
        (*fresh)->set_settings(s);
        std::vector<std::uint8_t> reference;
        for (int i = 0; i < 32; ++i) reference = stripes_frame(**fresh, *stripe, *box, 1.5f, pitch);
        const auto uncovered = disoccluded_mask();
        std::size_t uncovered_px = 0;
        for (const bool u : uncovered) uncovered_px += u ? 1 : 0;
        MESSAGE("uncovered pixels " << uncovered_px);
        REQUIRE(uncovered_px > 400);
        double sum_on = 0, sum_off = 0;
        for (std::size_t f = 0; f < on.size(); ++f) {
            const double e_on = mean_abs_rgb(on[f], reference, &uncovered), e_off = mean_abs_rgb(off[f], reference, &uncovered);
            MESSAGE("scale " << scale << " frame " << f + 1 << " after the move: error with depth test " << e_on << ", without " << e_off);
            sum_on += e_on;
            sum_off += e_off;
        }
        test::save_output(scale < 1.0f ? "taa_disocclusion_upscale_on" : "taa_disocclusion_on", W, H, on[0]);
        test::save_output(scale < 1.0f ? "taa_disocclusion_upscale_off" : "taa_disocclusion_off", W, H, off[0]);
        // Measured on lavapipe: 0.36 native, 0.53 upscaling (ADR-0147).
        CHECK(sum_on < 0.65 * sum_off);
        // A static camera keeps its history: the converged image is unchanged by the test.
        auto still_on = SceneRenderer::create(ctx);
        REQUIRE(still_on.ok());
        s.taa_disocclusion = 1.0f;
        (*still_on)->set_settings(s);
        std::vector<std::uint8_t> still;
        for (int i = 0; i < 32; ++i) still = stripes_frame(**still_on, *stripe, *box, 1.5f, pitch);
        const double still_diff = mean_abs_rgb(still, reference);
        MESSAGE("scale " << scale << " static camera, depth test on vs off: " << still_diff);
        CHECK(still_diff < 0.5);
    }
    ctx.destroy(*stripe);
    ctx.destroy(*box);
}

namespace {
// ADR-0148 scene: dark wires a fraction of a render pixel wide, slightly slanted, in front of a white
// wall. The colour clip sees a wire only in the jitter phases whose sample lands on it.
std::vector<std::uint8_t> wires_frame(SceneRenderer& r, GpuMesh bar, std::uint32_t w, std::uint32_t h, bool wires = true) {
    auto& dev = gpu().ctx->device();
    Environment env;
    env.draw_sky = false;
    env.clear_color = {1, 1, 1, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    r.begin(Camera3D::look_at({0, 0, 6}, {0, 0, 0}, {0, 1, 0}), sun, env);
    Material white, black;
    white.unlit = black.unlit = true;
    white.base_color = {1, 1, 1, 1};
    black.base_color = {0, 0, 0, 1};
    r.submit(bar, white, math::Mat4::trs({0, 0, -1}, {}, {40, 40, 1}));
    // One output pixel is 6 / 83.1 = 0.072 units at the wires' distance.
    const float widths[] = {0.012f, 0.024f, 0.036f};
    for (int i = 0; i < (wires ? 6 : 0); ++i) {
        const float width = widths[i % 3];
        const math::Quat tilt = math::Quat::axis_angle({0, 0, 1}, 0.03f + 0.02f * static_cast<float>(i));
        r.submit(bar, black, math::Mat4::trs({-2.0f + 0.8f * static_cast<float>(i), 0, 0}, tilt, {width, 6, 0.01f}));
        const math::Quat flat = math::Quat::axis_angle({0, 0, 1}, 1.55f - 0.02f * static_cast<float>(i));
        r.submit(bar, black, math::Mat4::trs({0, -2.0f + 0.8f * static_cast<float>(i), 0}, flat, {width, 6, 0.01f}));
    }
    auto target = test::make_target(w, h);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, w, h, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    return px;
}
// The post pass encodes sRGB into the UNORM target; compare coverage in linear light (0..255).
double linear255(int v) {
    const double c = v / 255.0;
    return 255.0 * (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}
} // namespace

TEST_CASE("gpu: taa keeps sub-pixel wires (ADR-0148)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto bar = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.5f}), "wire");
    REQUIRE(bar.ok());
    // Ground truth: 8x8 supersampled without TAA (pixels are 0 or 255, so already linear), box-filtered down.
    constexpr std::uint32_t S = 8;
    std::vector<double> truth(static_cast<std::size_t>(W) * H, 0.0);
    {
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.ssao = s.bloom = false;
        (*sr)->set_settings(s);
        const auto big = wires_frame(**sr, *bar, W * S, H * S, true);
        for (std::uint32_t y = 0; y < H * S; ++y)
            for (std::uint32_t x = 0; x < W * S; ++x)
                truth[static_cast<std::size_t>(y / S) * W + x / S] += channel(pixel(big, W * S, x, y), 0) / double(S * S);
    }
    std::vector<bool> mask(truth.size());
    std::size_t mask_px = 0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        mask[i] = truth[i] < 250.0;
        mask_px += mask[i] ? 1 : 0;
    }
    MESSAGE("wire pixels " << mask_px);
    struct Result {
        double error, flicker, darkness, ghost;
        bool active;
    };
    const auto run = [&](float scale, float strength) {
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.fxaa = s.ssao = s.bloom = false;
        s.render_scale = scale;
        s.taa_thin_features = strength;
        (*sr)->set_settings(s);
        std::vector<std::vector<std::uint8_t>> frames;
        for (int i = 0; i < 64; ++i) {
            auto f = wires_frame(**sr, *bar, W, H, true);
            if (i >= 64 - 17) frames.push_back(std::move(f));
        }
        Result r{0, 0, 0, 0, (*sr)->stats().taa_thin_features};
        for (std::size_t f = 1; f < frames.size(); ++f)
            for (std::size_t i = 0; i < truth.size(); ++i) {
                if (!mask[i]) continue;
                const auto x = static_cast<std::uint32_t>(i % W), y = static_cast<std::uint32_t>(i / W);
                const double v = linear255(channel(pixel(frames[f], W, x, y), 0)), p = linear255(channel(pixel(frames[f - 1], W, x, y), 0));
                r.error += std::abs(v - truth[i]);
                r.flicker += std::abs(v - p);
                r.darkness += 255.0 - v;
            }
        const double n = static_cast<double>(mask_px * (frames.size() - 1));
        r.error /= n;
        r.flicker /= n;
        r.darkness /= n;
        if (strength > 0.0f) test::save_output(scale < 1.0f ? "taa_wires_upscale" : "taa_wires", W, H, frames.back());
        // The wires vanish: a lock must not hold them for long (one jitter period at most).
        std::vector<std::uint8_t> after;
        for (int i = 0; i < 10; ++i) after = wires_frame(**sr, *bar, W, H, false);
        for (std::size_t i = 0; i < truth.size(); ++i)
            if (mask[i]) r.ghost += 255.0 - linear255(channel(pixel(after, W, static_cast<std::uint32_t>(i % W), static_cast<std::uint32_t>(i / W)), 0));
        r.ghost /= static_cast<double>(mask_px);
        return r;
    };
    double truth_dark = 0;
    for (std::size_t i = 0; i < truth.size(); ++i) truth_dark += mask[i] ? 255.0 - truth[i] : 0.0;
    truth_dark /= static_cast<double>(mask_px);
    for (const float scale : {1.0f, 0.5f}) {
        CAPTURE(scale);
        const Result on = run(scale, 1.0f), off = run(scale, 0.0f);
        MESSAGE("scale " << scale << " locks on: error " << on.error << ", flicker " << on.flicker << ", darkness " << on.darkness
                         << ", ghost after 10 frames " << on.ghost << "; off: error " << off.error << ", flicker " << off.flicker
                         << ", darkness " << off.darkness << ", ghost " << off.ghost << "; truth darkness " << truth_dark);
        CHECK(on.active);
        CHECK_FALSE(off.active);
        // lavapipe, linear light: error 0.76x native, 0.42x upscaling; flicker 0.43x / 0.62x; wire
        // darkness 1.44 / 0.99 of the truth (0.34 / 0.13 with locks off). Natively the luminance-
        // weighted blend over-darkens the dark wire a little; without locks it nearly vanishes.
        CHECK(on.error < 0.85 * off.error);
        CHECK(on.flicker < 0.8 * off.flicker);
        CHECK(on.darkness > 0.6 * truth_dark);
        CHECK(on.darkness < 1.6 * truth_dark);
        CHECK(on.ghost < 2.0);
    }
    ctx.destroy(*bar);
}
