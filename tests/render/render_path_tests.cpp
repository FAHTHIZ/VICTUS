// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/render_path.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;

namespace {
RenderPathFacts full_frame() {
    RenderPathFacts f;
    f.render_width = f.output_width = 1280;
    f.render_height = f.output_height = 720;
    f.taa = f.taa_history = true;
    f.sun_shadows_requested = true;
    f.cascades = 4;
    f.cascade_resolution = 4096;
    f.ao_requested = f.ssao = true;
    f.bloom_requested = f.bloom = true;
    f.auto_exposure_requested = f.auto_exposure = true;
    f.prepass = f.occlusion = true;
    f.occlusion_tested = 40;
    f.local_lights_visible = f.local_lights_kept = 3;
    f.local_shadow_candidates = f.local_shadow_lights = 1;
    f.local_shadow_tiles = 6;
    f.visible_draws = 50;
    f.main_batches = 20;
    return f;
}
const PathFeature& get(const RenderPathReport& r, std::string_view name) {
    const PathFeature* f = r.find(name);
    REQUIRE(f != nullptr);
    return *f;
}
} // namespace

TEST_CASE("render path: ultra with everything requested is at the reference") {
    const auto r = describe_render_path(RenderSettings::preset(QualityTier::Ultra), full_frame());
    CHECK(r.tier == "ultra");
    for (const char* n : {"taa", "sun_shadows", "ssao", "bloom", "auto_exposure", "occlusion_culling", "local_lights", "local_shadows", "lods"})
        CHECK_MESSAGE(get(r, n).state == PathState::Active, n);
    CHECK(get(r, "fxaa").reason == PathReason::Replaced);
    CHECK(get(r, "temporal_upscale").reason == PathReason::NotNeeded);
    CHECK(get(r, "volumetric_fog").reason == PathReason::Scene);
    CHECK(get(r, "irradiance_volume").reason == PathReason::Scene);
    CHECK(r.reduced_summary().empty());
    CHECK(r.gpu_timing.starts_with("unavailable"));
}

TEST_CASE("render path: low tier reductions are attributed to the tier, overrides to settings") {
    RenderPathFacts f = full_frame();
    f.taa = f.taa_history = false;
    f.fxaa = true;
    f.ssao = false;
    f.prepass = f.occlusion = false;
    RenderSettings low = RenderSettings::preset(QualityTier::Low);
    auto r = describe_render_path(low, f);
    CHECK(get(r, "taa").state == PathState::Off);
    CHECK(get(r, "taa").reason == PathReason::QualityTier);
    CHECK(get(r, "fxaa").state == PathState::Active);
    CHECK(get(r, "sun_shadows").state == PathState::Reduced);
    CHECK(get(r, "sun_shadows").reason == PathReason::QualityTier);
    CHECK(get(r, "motion_vectors").reason == PathReason::Requires);
    CHECK_FALSE(r.reduced_summary().empty());
    CHECK(r.reduced_summary().find("taa off (quality_tier)") != std::string::npos);

    RenderSettings ultra = RenderSettings::preset(QualityTier::Ultra);
    ultra.ssao = false;
    ultra.shadow_resolution = 512;
    f = full_frame();
    f.ssao = false;
    r = describe_render_path(ultra, f);
    CHECK(get(r, "ssao").reason == PathReason::Setting);
    CHECK(get(r, "sun_shadows").reason == PathReason::Setting);
}

TEST_CASE("render path: budgets, history, capture mode, upscaling and capabilities") {
    RenderPathFacts f = full_frame();
    f.local_lights_visible = 700;
    f.local_lights_kept = 512;
    f.local_shadow_candidates = 5;
    f.local_shadow_lights = 2;
    f.taa_history = false;
    auto r = describe_render_path(RenderSettings::preset(QualityTier::Ultra), f);
    CHECK(get(r, "local_lights").reason == PathReason::Budget);
    CHECK(get(r, "local_lights").detail.find("512 of 700") != std::string::npos);
    CHECK(get(r, "local_shadows").reason == PathReason::Budget);
    CHECK(get(r, "taa").reason == PathReason::NoHistory);

    f = full_frame();
    f.render_width = 640;
    f.render_height = 360;
    f.temporal_upscale = true;
    RenderSettings scaled = RenderSettings::preset(QualityTier::Ultra);
    scaled.render_scale = 0.5f;
    r = describe_render_path(scaled, f);
    CHECK(get(r, "render_scale").state == PathState::Reduced);
    CHECK(get(r, "render_scale").reason == PathReason::Setting);
    CHECK(get(r, "temporal_upscale").state == PathState::Active);
    CHECK(get(r, "spatial_upscale").reason == PathReason::Replaced);

    f = full_frame();
    f.capture_mode = f.linear_output = true;
    f.taa = false;
    f.fog_requested = true;
    r = describe_render_path(RenderSettings::preset(QualityTier::Ultra), f);
    CHECK(get(r, "taa").reason == PathReason::CaptureMode);
    CHECK(get(r, "fxaa").reason == PathReason::CaptureMode);
    CHECK(get(r, "volumetric_fog").reason == PathReason::CaptureMode);

    f = full_frame();
    f.compute = false;
    f.occlusion = false;
    f.fog_requested = true;
    r = describe_render_path(RenderSettings::preset(QualityTier::Ultra), f);
    CHECK(get(r, "occlusion_culling").reason == PathReason::Unsupported);
    CHECK(get(r, "volumetric_fog").reason == PathReason::Unsupported);
    CHECK(r.reduced_summary().find("occlusion_culling off (unsupported)") != std::string::npos);
}

TEST_CASE("render path report serialises every feature") {
    const auto r = describe_render_path(RenderSettings::preset(QualityTier::Medium), full_frame());
    const json::Value j = r.to_json();
    const auto* features = j.find("features");
    REQUIRE(features != nullptr);
    CHECK(features->as_array().size() == r.features.size());
    CHECK(r.features.size() >= 17);
    CHECK(j.find("tier")->as_string_or("") == "medium");
    const std::string text = r.to_text();
    CHECK(text.find("sun_shadows") != std::string::npos);
    CHECK(text.find("gpu timing: unavailable") != std::string::npos);
    CHECK(path_reason_name(PathReason::Budget) == "budget");
    CHECK(path_state_name(PathState::Reduced) == "reduced");
}
