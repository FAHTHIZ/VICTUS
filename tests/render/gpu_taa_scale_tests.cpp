// SPDX-License-Identifier: Apache-2.0
// Scale-triggered temporal upscaling (ADR-0188): tiers without TAA (Low) keep MSAA + FXAA at their
// own render scale and switch to TAAU once the scale (set directly or by dynamic resolution) falls
// below RenderSettings::taa_below_scale, with hysteresis so the switch does not toggle every frame.
#include "gpu_fixture.hpp"
#include "victus/render/render_path.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {
constexpr std::uint32_t W = 96, H = 64;

SceneStats frame(SceneRenderer& r, const GpuMesh& cube) {
    auto& dev = gpu().ctx->device();
    Environment env;
    env.draw_sky = false;
    DirectionalLight sun;
    sun.shadows = false;
    r.begin(Camera3D::look_at({0, 1, 4}, {0, 0, 0}), sun, env);
    r.submit(cube, Material{}, math::Mat4::identity());
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    dev.destroy(target);
    return r.stats();
}

void set_scale(SceneRenderer& r, float scale) {
    RenderSettings s = r.settings();
    s.render_scale = scale;
    r.set_settings(s);
}

const PathFeature* feature(const RenderPathReport& rep, std::string_view name) {
    for (const auto& f : rep.features)
        if (f.name == name) return &f;
    return nullptr;
}
} // namespace

TEST_CASE("render settings: Low switches to TAAU below 0.65 render scale, other tiers never need to") {
    const RenderSettings low = RenderSettings::preset(QualityTier::Low);
    CHECK_FALSE(low.taa);
    CHECK(low.taa_below_scale == doctest::Approx(0.65f));
    CHECK(low.render_scale > low.taa_below_scale); // the tier's own scale keeps MSAA + FXAA
    CHECK(RenderSettings::preset(QualityTier::Medium).taa_below_scale == 0.0f);
    CHECK(RenderSettings::preset(QualityTier::High).taa);
    RenderSettings s;
    s.taa_below_scale = std::nanf("");
    CHECK(s.sanitized().taa_below_scale == 0.0f);
    s.taa_below_scale = -1.0f;
    CHECK(s.sanitized().taa_below_scale == 0.0f);
}

TEST_CASE("gpu: Low hands anti-aliasing to temporal upscaling at low render scales, with hysteresis (ADR-0188)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = **sr;
    auto cube = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.5f}), "taa_scale_cube");
    REQUIRE(cube.ok());
    r.set_settings(RenderSettings::preset(QualityTier::Low));

    SceneStats st = frame(r, *cube); // 0.75: the tier's MSAA + FXAA
    CHECK_FALSE(st.taa);
    CHECK_FALSE(st.taa_by_scale);
    CHECK(st.msaa_samples > 1);

    set_scale(r, 0.5f); // dynamic resolution dropped the scale
    st = frame(r, *cube);
    CHECK(st.taa);
    CHECK(st.taa_by_scale);
    CHECK(st.temporal_upscale);
    CHECK(st.msaa_samples == 1);
    st = frame(r, *cube);
    CHECK(st.taa_history);
    const PathFeature* taa = feature(r.path_report(), "taa");
    REQUIRE(taa != nullptr);
    CHECK(taa->state == PathState::Active);
    const PathFeature* msaa = feature(r.path_report(), "msaa");
    REQUIRE(msaa != nullptr);
    CHECK(msaa->reason == PathReason::Replaced);

    set_scale(r, 0.68f); // inside the hysteresis band: stays temporal
    CHECK(frame(r, *cube).taa_by_scale);
    set_scale(r, 0.7f); // 0.05 above the threshold: back to MSAA + FXAA
    st = frame(r, *cube);
    CHECK_FALSE(st.taa);
    CHECK(st.msaa_samples > 1);
    set_scale(r, 0.68f); // and the band does not switch it on again from above
    CHECK_FALSE(frame(r, *cube).taa);

    RenderSettings off = RenderSettings::preset(QualityTier::Low);
    off.render_scale = 0.5f;
    off.temporal_upscale = false; // the project opted out of temporal upscaling
    r.set_settings(off);
    CHECK_FALSE(frame(r, *cube).taa);
    off.temporal_upscale = true;
    off.taa_below_scale = 0.0f;
    r.set_settings(off);
    CHECK_FALSE(frame(r, *cube).taa);
    ctx.destroy(*cube);
}
