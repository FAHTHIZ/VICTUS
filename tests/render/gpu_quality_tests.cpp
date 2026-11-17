// SPDX-License-Identifier: Apache-2.0
// Pixel-probe tests for the scalable rendering features: quality tiers, cascaded shadows,
// SSAO, bloom, auto exposure, tonemappers, FXAA and render scale.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <functional>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {

struct Frame {
    std::uint32_t w{0}, h{0};
    std::vector<std::uint8_t> px;
    SceneStats stats;
    CompiledGraphInfo graph;
    math::Mat4 view_proj;
    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return pixel(px, w, x, y); }
    [[nodiscard]] int lum(std::uint32_t x, std::uint32_t y) const {
        const auto p = at(x, y);
        return channel(p, 0) + channel(p, 1) + channel(p, 2);
    }
    [[nodiscard]] int lum_at(math::Vec3 world) const {
        const math::Vec4 c = view_proj * math::Vec4(world, 1.0f);
        const float x = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(w);
        const float y = (0.5f - c.y / c.w * 0.5f) * static_cast<float>(h);
        return lum(static_cast<std::uint32_t>(std::clamp(x, 0.0f, static_cast<float>(w - 1))),
                   static_cast<std::uint32_t>(std::clamp(y, 0.0f, static_cast<float>(h - 1))));
    }
    [[nodiscard]] double mean_lum() const {
        double s = 0;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) s += lum(x, y);
        return s / (static_cast<double>(w) * h);
    }
};

struct Meshes {
    GpuMesh cube, plane;
    Meshes() {
        auto& ctx = *gpu().ctx;
        cube = ctx.create_mesh(make_cube(), "q_cube").value();
        plane = ctx.create_mesh(make_plane({200, 200}), "q_plane").value();
    }
    ~Meshes() {
        gpu().ctx->destroy(cube);
        gpu().ctx->destroy(plane);
    }
};

// Everything off: the baseline each feature test compares against.
RenderSettings bare() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.ssao = s.bloom = s.fxaa = s.auto_exposure = s.taa = false;
    s.shadow_cascades = 1;
    return s;
}

Frame render_frame(SceneRenderer& r, const RenderSettings& settings, const Camera3D& cam, const DirectionalLight& sun,
                   const Environment& env, const std::function<void(SceneRenderer&)>& content, std::uint32_t W = 192,
                   std::uint32_t H = 128, int frames = 1, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    r.set_settings(settings);
    for (int i = 0; i < frames; ++i) {
        r.begin(cam, sun, env);
        content(r);
        auto cmd = dev.begin_commands();
        auto st = r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM);
        REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
        cmd.submit_and_wait();
    }
    Frame f;
    f.w = W;
    f.h = H;
    f.px = dev.read_texture(target).value();
    f.stats = r.stats();
    f.graph = r.graph_info();
    f.view_proj = cam.projection(static_cast<float>(W) / static_cast<float>(H)) * cam.view;
    if (save) test::save_output(save, W, H, f.px);
    dev.destroy(target);
    return f;
}

bool has_pass(const Frame& f, std::string_view name) {
    return std::find(f.graph.executed_passes.begin(), f.graph.executed_passes.end(), name) != f.graph.executed_passes.end();
}

} // namespace

TEST_CASE("gpu quality: tiers schedule the expected passes") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 4, 8}, {0, 0.5f, 0});
    DirectionalLight sun;
    Environment env;
    env.auto_exposure = true;
    auto content = [&](SceneRenderer& r) {
        r.submit(m.plane, {}, math::Mat4::identity());
        r.submit(m.cube, {}, math::Mat4::translation({0, 1, 0}));
    };
    const Frame low = render_frame(*sr.value(), RenderSettings::preset(QualityTier::Low), cam, sun, env, content);
    CHECK(low.stats.render_width == 144);
    CHECK(low.stats.render_height == 96);
    CHECK(low.stats.shadow_cascades == 1);
    CHECK(has_pass(low, "shadow"));
    CHECK(has_pass(low, "fxaa"));
    CHECK(has_pass(low, "exposure"));
    CHECK_FALSE(has_pass(low, "ssao"));
    CHECK_FALSE(has_pass(low, "depth_prepass"));
    CHECK_FALSE(has_pass(low, "bloom_down_0"));

    const Frame high = render_frame(*sr.value(), RenderSettings::preset(QualityTier::High), cam, sun, env, content);
    CHECK(high.stats.render_width == 192);
    CHECK(high.stats.shadow_cascades == 3);
    CHECK(has_pass(high, "depth_prepass"));
    CHECK(has_pass(high, "ssao"));
    CHECK(has_pass(high, "ssao_blur"));
    CHECK(has_pass(high, "bloom_down_0"));
    CHECK(has_pass(high, "bloom_up_0"));
    CHECK(high.stats.prepass_draws == 2);
    CHECK(high.stats.shadow_draws >= 2);

    const Frame ultra = render_frame(*sr.value(), RenderSettings::preset(QualityTier::Ultra), cam, sun, env, content);
    CHECK(ultra.stats.shadow_cascades == 4);
    // Low and Ultra both produce a lit, non-black image of the same scene.
    CHECK(low.mean_lum() > 100);
    CHECK(ultra.mean_lum() > 100);
}

TEST_CASE("gpu quality: cascaded shadows reach distant casters") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 3, 0}, {0, 2.2f, -20});
    DirectionalLight sun;
    sun.direction = {0.0f, -1.0f, 0.0f};
    sun.shadow_distance = 60.0f;
    Environment env;
    Material ground;
    ground.base_color = {0.7f, 0.7f, 0.7f, 1};
    // Two casters floating above the ground: one near (cascade 0), one far (last cascade).
    auto content = [&](SceneRenderer& r) {
        r.submit(m.plane, ground, math::Mat4::identity());
        r.submit(m.cube, {}, math::Mat4::trs({0, 1.5f, -6}, {}, {1, 0.2f, 1}));
        r.submit(m.cube, {}, math::Mat4::trs({0, 3.5f, -40}, {}, {6, 0.2f, 6}));
    };
    RenderSettings s = bare();
    s.shadow_cascades = 4;
    s.shadow_resolution = 512;
    const Frame f = render_frame(*sr.value(), s, cam, sun, env, content, 256, 160, 1, "quality_csm");
    auto& r = *sr.value();
    const auto splits = r.cascade_splits();
    CHECK(splits[0] > 0.1f);
    CHECK(splits[0] < splits[1]);
    CHECK(splits[1] < splits[2]);
    CHECK(splits[2] < splits[3]);
    CHECK(splits[3] == doctest::Approx(60.0f));
    // Shadow below each caster vs. lit ground beside it at the same distance.
    const int near_shadow = f.lum_at({0, 0, -6}), near_lit = f.lum_at({2.5f, 0, -6});
    const int far_shadow = f.lum_at({0, 0, -40}), far_lit = f.lum_at({12, 0, -40});
    MESSAGE("near " << near_shadow << "/" << near_lit << " far " << far_shadow << "/" << far_lit);
    CHECK(near_lit > near_shadow + 60);
    CHECK(far_lit > far_shadow + 60);
    // With a single short cascade the far caster's shadow is gone (beyond shadow distance).
    s.shadow_cascades = 1;
    DirectionalLight short_sun = sun;
    short_sun.shadow_distance = 15.0f;
    const Frame one = render_frame(*sr.value(), s, cam, short_sun, env, content);
    CHECK(std::abs(one.lum_at({0, 0, -40}) - one.lum_at({12, 0, -40})) < 20);
}

TEST_CASE("gpu quality: SSAO darkens creases but not open ground") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({3, 3, 5}, {0, 0.5f, 0});
    DirectionalLight sun;
    sun.intensity = 0.0f; // ambient only: AO is the only thing that changes
    Environment env;
    env.ambient_intensity = 1.5f;
    env.ao_radius = 0.8f;
    env.ao_intensity = 1.5f;
    auto content = [&](SceneRenderer& r) {
        r.submit(m.plane, {}, math::Mat4::identity());
        r.submit(m.cube, {}, math::Mat4::trs({0, 1, 0}, {}, {2, 2, 2}));
    };
    RenderSettings s = bare();
    const Frame off = render_frame(*sr.value(), s, cam, sun, env, content);
    s.ssao = true;
    s.ssao_half_resolution = false;
    s.ssao_samples = 16;
    const Frame on = render_frame(*sr.value(), s, cam, sun, env, content, 192, 128, 1, "quality_ssao");
    CHECK(has_pass(on, "ssao"));
    const math::Vec3 crease{1.1f, 0.0f, 1.1f}; // ground right next to the cube's corner
    const math::Vec3 open{-2.5f, 0.0f, 3.0f};  // far from any occluder
    MESSAGE("crease off " << off.lum_at(crease) << " on " << on.lum_at(crease) << "; open off " << off.lum_at(open)
                          << " on " << on.lum_at(open));
    CHECK(on.lum_at(crease) < off.lum_at(crease) - 15);
    CHECK(std::abs(on.lum_at(open) - off.lum_at(open)) <= 12);
}

TEST_CASE("gpu quality: bloom spreads bright emissive light") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 0, 6}, {0, 0, 0});
    DirectionalLight sun;
    sun.intensity = 0.0f;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.ambient_intensity = 0.0f;
    env.vignette = 0.0f;
    env.bloom_intensity = 0.2f;
    Material glow;
    glow.base_color = {0, 0, 0, 1};
    glow.emissive = {1.0f, 0.6f, 0.2f, 1};
    glow.emissive_strength = 30.0f;
    auto content = [&](SceneRenderer& r) { r.submit(m.cube, glow, math::Mat4::scale({0.5f, 0.5f, 0.5f})); };
    RenderSettings s = bare();
    const Frame off = render_frame(*sr.value(), s, cam, sun, env, content);
    s.bloom = true;
    const Frame on = render_frame(*sr.value(), s, cam, sun, env, content, 192, 128, 1, "quality_bloom");
    const std::uint32_t cx = 96, cy = 64;
    MESSAGE("halo off " << off.lum(cx + 30, cy) << " on " << on.lum(cx + 30, cy) << " corner " << on.lum(2, 2));
    CHECK(off.lum(cx + 30, cy) < 6);         // pure black next to the cube without bloom
    CHECK(on.lum(cx + 30, cy) > 40);         // glow halo with bloom
    CHECK(on.lum(cx + 30, cy) > on.lum(cx + 60, cy)); // halo falls off with distance
    CHECK(on.lum(cx, cy) >= off.lum(cx, cy)); // core never gets darker
    CHECK(on.lum(2, 2) < 40);                 // far corner stays dark
}

TEST_CASE("gpu quality: auto exposure normalises scene brightness and adapts over time") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 4, 8}, {0, 0.5f, 0});
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0.02f, 0.02f, 0.02f, 1};
    env.vignette = 0.0f;
    auto content = [&](SceneRenderer& r) {
        r.submit(m.plane, {}, math::Mat4::identity());
        r.submit(m.cube, {}, math::Mat4::translation({0, 1, 0}));
    };
    DirectionalLight dim, bright;
    dim.intensity = 0.08f;
    bright.intensity = 12.0f;
    env.auto_exposure_min_ev = -12.0f;
    Environment dim_env = env, bright_env = env;
    dim_env.ambient_intensity = 0.02f;
    bright_env.ambient_intensity = 1.5f;
    RenderSettings s = bare();
    s.auto_exposure = true;
    const Frame manual_dim = render_frame(*sr.value(), s, cam, dim, dim_env, content);
    const Frame manual_bright = render_frame(*sr.value(), s, cam, bright, bright_env, content);
    dim_env.auto_exposure = bright_env.auto_exposure = true;
    auto& r = *sr.value();
    r.reset_adaptation();
    const Frame auto_dim = render_frame(r, s, cam, dim, dim_env, content);
    r.reset_adaptation();
    const Frame auto_bright = render_frame(r, s, cam, bright, bright_env, content);
    MESSAGE("manual " << manual_dim.mean_lum() << " / " << manual_bright.mean_lum() << ", auto " << auto_dim.mean_lum()
                      << " / " << auto_bright.mean_lum());
    CHECK(manual_bright.mean_lum() > manual_dim.mean_lum() * 3.0);
    CHECK(auto_dim.mean_lum() > manual_dim.mean_lum() * 2.0);
    CHECK(std::abs(auto_dim.mean_lum() - auto_bright.mean_lum()) < 0.35 * auto_bright.mean_lum());

    // Adaptation: after a cut from bright to dim, one short frame only moves part of the way.
    r.reset_adaptation();
    (void)render_frame(r, s, cam, bright, bright_env, content);
    r.set_frame_time(0.05f);
    const Frame step = render_frame(r, s, cam, dim, dim_env, content);
    r.set_frame_time(1.0f);
    const Frame settled = render_frame(r, s, cam, dim, dim_env, content, 192, 128, 6);
    MESSAGE("adapting " << step.mean_lum() << " -> " << settled.mean_lum());
    CHECK(step.mean_lum() < settled.mean_lum() * 0.8);
    CHECK(settled.mean_lum() == doctest::Approx(auto_dim.mean_lum()).epsilon(0.1));
    r.set_frame_time(1.0f / 60.0f);
}

TEST_CASE("gpu quality: tonemappers and grading") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 0, 3}, {0, 0, 0});
    DirectionalLight sun;
    sun.intensity = 0.0f;
    Environment env;
    env.draw_sky = false;
    env.vignette = 0.0f;
    Material lamp;
    lamp.unlit = true;
    lamp.base_color = {4.0f, 1.0f, 0.25f, 1}; // HDR, beyond display range
    auto content = [&](SceneRenderer& r) { r.submit(m.cube, lamp, math::Mat4::identity()); };
    RenderSettings s = bare();
    auto centre = [&](Tonemapper t, float sat = 1.0f) {
        Environment e = env;
        e.tonemapper = t;
        e.saturation = sat;
        return render_frame(*sr.value(), s, cam, sun, e, content).at(96, 64);
    };
    const auto none = centre(Tonemapper::None), aces = centre(Tonemapper::Aces), neutral = centre(Tonemapper::Neutral),
               reinhard = centre(Tonemapper::Reinhard);
    CHECK(channel(none, 0) == 255); // clamp only
    CHECK(channel(none, 1) == 255);
    CHECK(channel(aces, 0) > channel(aces, 1));
    CHECK(channel(neutral, 0) > channel(neutral, 2));
    CHECK(channel(reinhard, 0) > channel(reinhard, 2));
    CHECK(aces != neutral);
    CHECK(neutral != reinhard);
    const auto grey = centre(Tonemapper::Aces, 0.0f);
    CHECK(std::abs(int(channel(grey, 0)) - int(channel(grey, 1))) <= 1);
    CHECK(std::abs(int(channel(grey, 1)) - int(channel(grey, 2))) <= 1);
    CHECK(parse_tonemapper("neutral") == Tonemapper::Neutral);
    CHECK_FALSE(parse_tonemapper("filmic").has_value());
}

TEST_CASE("gpu quality: FXAA smooths aliased edges") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 0, 5}, {0, 0, 0});
    DirectionalLight sun;
    sun.intensity = 0.0f;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.vignette = 0.0f;
    env.tonemapper = Tonemapper::None;
    Material white;
    white.unlit = true;
    auto content = [&](SceneRenderer& r) {
        r.submit(m.cube, white, math::Mat4::trs({}, math::Quat::axis_angle({0, 0, 1}, 0.35f), {2, 2, 2}));
    };
    // Hard steps: neighbouring pixels whose brightness jumps by more than 60% of full range.
    auto hard_steps = [](const Frame& f) {
        int n = 0;
        for (std::uint32_t y = 0; y + 1 < f.h; ++y)
            for (std::uint32_t x = 0; x + 1 < f.w; ++x) {
                const int l = f.lum(x, y);
                if (std::abs(l - f.lum(x + 1, y)) > 460) ++n;
                if (std::abs(l - f.lum(x, y + 1)) > 460) ++n;
            }
        return n;
    };
    RenderSettings s = bare();
    const Frame off = render_frame(*sr.value(), s, cam, sun, env, content, 192, 128, 1, "quality_fxaa_off");
    s.fxaa = true;
    const Frame on = render_frame(*sr.value(), s, cam, sun, env, content, 192, 128, 1, "quality_fxaa");
    // Blended edge pixels: neither background nor fully lit.
    auto blended = [](const Frame& f) {
        int n = 0;
        for (std::uint32_t y = 0; y < f.h; ++y)
            for (std::uint32_t x = 0; x < f.w; ++x) n += (f.lum(x, y) > 90 && f.lum(x, y) < 675) ? 1 : 0;
        return n;
    };
    MESSAGE("hard steps off " << hard_steps(off) << " on " << hard_steps(on) << "; blended off " << blended(off)
                              << " on " << blended(on));
    CHECK(hard_steps(off) > 100);                       // aliased staircase edges
    // FXAA turns steps into gradients. Drivers differ in exactly which pixels blend (lavapipe
    // 283 -> 112 steps, Apple Metal 288 -> 204), so the bound is loose but still requires a clear win.
    CHECK(hard_steps(on) * 4 < hard_steps(off) * 3);
    CHECK(blended(on) >= blended(off) + 80);
    CHECK(on.at(96, 64) == off.at(96, 64));     // flat interior untouched
    CHECK(on.at(2, 2) == off.at(2, 2));
}

TEST_CASE("gpu quality: render scale upsamples a lower internal resolution") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto sr = SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    Meshes m;
    const Camera3D cam = Camera3D::look_at({0, 4, 8}, {0, 0.5f, 0});
    DirectionalLight sun;
    Environment env;
    auto content = [&](SceneRenderer& r) {
        r.submit(m.plane, {}, math::Mat4::identity());
        r.submit(m.cube, {}, math::Mat4::translation({0, 1, 0}));
    };
    RenderSettings s = bare();
    const Frame full = render_frame(*sr.value(), s, cam, sun, env, content);
    s.render_scale = 0.5f;
    const Frame half = render_frame(*sr.value(), s, cam, sun, env, content);
    CHECK(half.stats.render_width == 96);
    CHECK(half.stats.render_height == 64);
    // Same picture, slightly softer: mean brightness within a few percent.
    CHECK(half.mean_lum() == doctest::Approx(full.mean_lum()).epsilon(0.05));
}

#include "victus/runtime/scene_rendering.hpp"

TEST_CASE("gpu quality: Environment component drives the scene look") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity e = s.create("Environment");
    auto& env = s.world().add<gameplay::EnvironmentComponent>(e);
    env.draw_sky = false;
    env.clear_color = {0.8f, 0.05f, 0.05f, 1};
    env.tonemapper = gameplay::EnvironmentComponent::Tonemapper::None;
    env.vignette = 0.0f;
    env.shadow_distance = 12.0f;
    s.update_transforms();
    auto& r = *sr.value();
    RenderSettings st = bare();
    r.set_settings(st);
    (void)runtime::extract_scene(s, cache, r);
    auto target = test::make_target(64, 32);
    auto cmd = ctx.device().begin_commands();
    REQUIRE(r.render(cmd, target, 64, 32, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = ctx.device().read_texture(target).value();
    const auto p = pixel(px, 64, 32, 16);
    CHECK(channel(p, 0) > 200); // linear 0.8 -> sRGB ~231, untouched by tonemapping
    CHECK(channel(p, 1) < 80);
    ctx.device().destroy(target);

    // Clamping: negative/invalid values from hand-edited scenes cannot break the renderer.
    gameplay::EnvironmentComponent bad;
    bad.ambient_intensity = -3.0f;
    bad.vignette = 7.0f;
    bad.auto_exposure_min_ev = 4.0f;
    bad.auto_exposure_max_ev = -4.0f;
    bad.shadow_distance = -1.0f;
    Environment out;
    DirectionalLight sun;
    runtime::apply_environment(bad, out, sun);
    CHECK(out.ambient_intensity == 0.0f);
    CHECK(out.vignette == 1.0f);
    CHECK(out.auto_exposure_max_ev >= out.auto_exposure_min_ev);
    CHECK(sun.shadow_distance == 1.0f);
}
