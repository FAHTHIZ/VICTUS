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

TEST_CASE("render path: decals, water, particles and reflection probes") {
    const RenderSettings ultra = RenderSettings::preset(QualityTier::Ultra);
    RenderPathFacts f = full_frame();
    auto r = describe_render_path(ultra, f);
    for (const char* n : {"decals", "water", "particles", "gpu_particles", "reflection_probes"}) {
        CHECK_MESSAGE(get(r, n).state == PathState::Off, n);
        CHECK_MESSAGE(get(r, n).reason == PathReason::Scene, n);
    }
    CHECK(r.reduced_summary().empty());

    // Everything in view and within budget.
    f.decals_submitted = f.decals_in_view = f.decals_drawn = 5;
    f.decal_budget = 1024;
    f.water_submitted = f.water_in_view = f.water_drawn = 1;
    f.water_budget = 16;
    f.particle_batches_submitted = 2;
    f.particle_batches_drawn = 3;
    f.particles_drawn = 900;
    f.gpu_emitters_submitted = f.gpu_emitters_simulated = 1;
    f.gpu_particles = 600;
    f.probes_requested = f.probes_used = 2;
    f.probe_budget = 64;
    f.probe_draws = 12;
    r = describe_render_path(ultra, f);
    for (const char* n : {"decals", "water", "particles", "gpu_particles", "reflection_probes"})
        CHECK_MESSAGE(get(r, n).state == PathState::Active, n);
    CHECK(get(r, "decals").detail == "5 of 5 in view");
    CHECK(get(r, "particles").detail == "900 particles in 3 batches");
    CHECK(get(r, "gpu_particles").detail == "600 alive in 1 emitter");
    f.gpu_emitters_sorted = 1;
    CHECK(describe_render_path(ultra, f).find("gpu_particles")->detail == "600 alive in 1 emitter, 1 depth-sorted");
    f.gpu_emitters_sorted = 0;
    CHECK(get(r, "reflection_probes").detail == "2 probes, 12 of 50 draws inside a probe box");
    CHECK(r.reduced_summary().empty());

    // Budgets and culling.
    f.decals_in_view = 1500;
    f.decals_submitted = 2000;
    f.decals_drawn = 1024;
    f.water_submitted = 3;
    f.water_in_view = 0;
    f.water_drawn = 0;
    f.probes_over_budget = 4;
    r = describe_render_path(ultra, f);
    CHECK(get(r, "decals").state == PathState::Reduced);
    CHECK(get(r, "decals").reason == PathReason::Budget);
    CHECK(get(r, "decals").detail == "nearest 1024 of 1500 decals in view (max 1024)");
    CHECK(get(r, "water").reason == PathReason::NotNeeded);
    CHECK(get(r, "water").detail == "3 surfaces outside the view");
    CHECK(get(r, "reflection_probes").reason == PathReason::Budget);
    CHECK(r.reduced_summary() == "decals (budget), reflection_probes (budget)");

    // Unbaked probes and no compute.
    f.probes_over_budget = 0;
    f.probes_used = 0;
    f.probes_unbaked = 2;
    f.compute = false;
    f.gpu_emitters_simulated = 0;
    f.gpu_particles = 0;
    r = describe_render_path(ultra, f);
    CHECK(get(r, "reflection_probes").state == PathState::Off);
    CHECK(get(r, "reflection_probes").detail == "2 probes not captured yet (bake the probes)");
    CHECK(get(r, "gpu_particles").reason == PathReason::Unsupported);
    CHECK(r.reduced_summary().find("gpu_particles off (unsupported)") != std::string::npos);
}

TEST_CASE("render path: effect budgets scale with the tier and name the tier, a setting or the budget") {
    const RenderSettings low = RenderSettings::preset(QualityTier::Low);
    CHECK(low.max_decals == 128);
    CHECK(low.max_water_surfaces == 4);
    CHECK(low.max_particles == 16384);
    CHECK(RenderSettings::preset(QualityTier::Medium).max_decals == 256);
    CHECK(RenderSettings::preset(QualityTier::High).max_particles == 262144);
    const RenderSettings ultra = RenderSettings::preset(QualityTier::Ultra);
    CHECK(ultra.max_decals == SceneRenderer::kMaxDecals);
    CHECK(ultra.max_water_surfaces == kMaxWaterSurfaces);
    RenderSettings over = ultra;
    over.max_decals = 5000;
    over.max_water_surfaces = 99;
    CHECK(over.sanitized().max_decals == SceneRenderer::kMaxDecals);
    CHECK(over.sanitized().max_water_surfaces == kMaxWaterSurfaces);

    RenderPathFacts f = full_frame();
    f.decals_submitted = f.decals_in_view = 300;
    f.decals_drawn = 128;
    f.decal_budget = 128;
    f.water_submitted = f.water_in_view = 6;
    f.water_drawn = f.water_budget = 4;
    f.particle_batches_submitted = f.particle_batches_in_view = 3;
    f.particles_in_view = 30000;
    f.particle_batches_drawn = 2;
    f.particles_drawn = 12000;
    f.particle_budget = 16384;
    auto r = describe_render_path(low, f);
    CHECK(get(r, "decals").reason == PathReason::QualityTier);
    CHECK(get(r, "decals").detail == "nearest 128 of 300 decals in view (max 128)");
    CHECK(get(r, "water").reason == PathReason::QualityTier);
    CHECK(get(r, "particles").state == PathState::Reduced);
    CHECK(get(r, "particles").reason == PathReason::QualityTier);
    CHECK(get(r, "particles").detail == "12000 particles in 2 batches; nearest 12000 of 30000 CPU particles in view (max_particles 16384)");
    CHECK(r.reduced_summary().find("decals (quality_tier), water (quality_tier), particles (quality_tier)") != std::string::npos);

    // Lowered by hand on High: a setting. Ultra cannot hold them all: the budget.
    RenderSettings high = RenderSettings::preset(QualityTier::High);
    high.max_decals = 128;
    CHECK(get(describe_render_path(high, f), "decals").reason == PathReason::Setting);
    f.decals_in_view = 1500;
    f.decals_drawn = f.decal_budget = 1024;
    CHECK(get(describe_render_path(ultra, f), "decals").reason == PathReason::Budget);
    // Everything fits: active, whatever the tier.
    f.particles_in_view = 12000;
    CHECK(get(describe_render_path(low, f), "particles").state == PathState::Active);
}
