// SPDX-License-Identifier: Apache-2.0
#include "victus/render/render_path.hpp"

#include "victus/render/scene_renderer.hpp"

#include <format>

namespace victus::render {

std::string_view path_state_name(PathState s) noexcept {
    switch (s) {
    case PathState::Active: return "active";
    case PathState::Reduced: return "reduced";
    case PathState::Off: return "off";
    }
    return "off";
}

std::string_view path_reason_name(PathReason r) noexcept {
    switch (r) {
    case PathReason::None: return "none";
    case PathReason::QualityTier: return "quality_tier";
    case PathReason::Setting: return "setting";
    case PathReason::Scene: return "scene";
    case PathReason::NotNeeded: return "not_needed";
    case PathReason::Replaced: return "replaced";
    case PathReason::Requires: return "requires";
    case PathReason::CaptureMode: return "capture_mode";
    case PathReason::Unsupported: return "unsupported";
    case PathReason::Budget: return "budget";
    case PathReason::NoHistory: return "no_history";
    }
    return "none";
}

const PathFeature* RenderPathReport::find(std::string_view name) const noexcept {
    for (const auto& f : features)
        if (f.name == name) return &f;
    return nullptr;
}

json::Value RenderPathReport::to_json() const {
    auto root = json::Value::object();
    root.set("tier", tier);
    root.set("render_scale", static_cast<double>(render_scale));
    auto res = json::Value::array();
    for (const auto v : {render_width, render_height, output_width, output_height}) res.push(static_cast<double>(v));
    root.set("resolution", std::move(res)); // render w, h, output w, h
    auto list = json::Value::array();
    for (const auto& f : features) {
        auto o = json::Value::object();
        o.set("name", f.name);
        o.set("state", std::string(path_state_name(f.state)));
        o.set("reason", std::string(path_reason_name(f.reason)));
        o.set("detail", f.detail);
        list.push(std::move(o));
    }
    root.set("features", std::move(list));
    root.set("gpu_timing", gpu_timing);
    return root;
}

std::string RenderPathReport::to_text() const {
    std::string out = std::format("render path: {} tier, {}x{} of {}x{} (scale {:.2f})\n", tier, render_width, render_height, output_width,
                                  output_height, render_scale);
    for (const auto& f : features)
        out += std::format("  {:<18} {:<8} {:<13} {}\n", f.name, path_state_name(f.state),
                           f.reason == PathReason::None ? "" : path_reason_name(f.reason), f.detail);
    out += "  gpu timing: " + gpu_timing + "\n";
    return out;
}

std::string RenderPathReport::reduced_summary() const {
    std::string out;
    for (const auto& f : features) {
        const bool off_lowered = f.state == PathState::Off && (f.reason == PathReason::QualityTier || f.reason == PathReason::Setting ||
                                                               f.reason == PathReason::Budget || f.reason == PathReason::Unsupported);
        if (f.state != PathState::Reduced && !off_lowered) continue;
        if (!out.empty()) out += ", ";
        out += std::format("{}{} ({})", f.name, f.state == PathState::Off ? " off" : "", path_reason_name(f.reason));
    }
    return out;
}

namespace {
std::string count(std::uint32_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}
} // namespace

RenderPathReport describe_render_path(const RenderSettings& settings, const RenderPathFacts& f) {
    const RenderSettings S = settings.sanitized();
    const RenderSettings tier = RenderSettings::preset(S.tier);
    // Ultra is the reference path: anything cheaper than it is reported as reduced.
    const RenderSettings ref = RenderSettings::preset(QualityTier::Ultra);
    RenderPathReport r;
    r.tier = std::string(quality_tier_name(S.tier));
    r.render_scale = S.render_scale;
    r.render_width = f.render_width;
    r.render_height = f.render_height;
    r.output_width = f.output_width;
    r.output_height = f.output_height;
    const auto add = [&](std::string name, PathState state, PathReason reason, std::string detail) {
        r.features.push_back({std::move(name), state, reason, std::move(detail)});
    };
    // Off or reduced by settings: the tier preset if the setting matches it, else a user override.
    const auto by = [](bool matches_tier) { return matches_tier ? PathReason::QualityTier : PathReason::Setting; };
    const bool scaled_down = f.render_width < f.output_width || f.render_height < f.output_height;

    // Resolution.
    if (scaled_down)
        add("render_scale", PathState::Reduced, by(S.render_scale == tier.render_scale),
            std::format("{}x{} rendered for {}x{} output", f.render_width, f.render_height, f.output_width, f.output_height));
    else
        add("render_scale", PathState::Active, PathReason::None, std::format("{}x{}", f.render_width, f.render_height));

    // Anti-aliasing and reconstruction.
    if (f.capture_mode) add("taa", PathState::Off, PathReason::CaptureMode, "captures are not jittered");
    else if (!S.taa) add("taa", PathState::Off, by(S.taa == tier.taa), "no temporal anti-aliasing");
    else if (!f.taa_history) add("taa", PathState::Reduced, PathReason::NoHistory, "no history this frame (first frame or reset)");
    else add("taa", PathState::Active, PathReason::None, std::format("current-frame weight {:.2f}", S.taa_blend));

    if (f.temporal_upscale) add("temporal_upscale", PathState::Active, PathReason::None, "TAA reconstructs at output resolution");
    else if (!scaled_down) add("temporal_upscale", PathState::Off, PathReason::NotNeeded, "rendering at output resolution");
    else if (!f.taa) add("temporal_upscale", PathState::Off, PathReason::Requires, "needs TAA");
    else add("temporal_upscale", PathState::Off, by(S.temporal_upscale == tier.temporal_upscale), "upscaled without temporal reconstruction");

    if (f.spatial_upscale) add("spatial_upscale", PathState::Active, PathReason::None, "Catmull-Rom + RCAS");
    else if (!scaled_down) add("spatial_upscale", PathState::Off, PathReason::NotNeeded, "rendering at output resolution");
    else if (f.temporal_upscale) add("spatial_upscale", PathState::Off, PathReason::Replaced, "temporal upscaling runs instead");
    else if (f.linear_output) add("spatial_upscale", PathState::Off, f.capture_mode ? PathReason::CaptureMode : PathReason::Scene, "linear output");
    else add("spatial_upscale", PathState::Off, by(S.spatial_upscale == tier.spatial_upscale), "bilinear upscale");

    if (f.fxaa) add("fxaa", PathState::Active, PathReason::None, "");
    else if (f.taa) add("fxaa", PathState::Off, PathReason::Replaced, "TAA resolves edges");
    else if (f.linear_output) add("fxaa", PathState::Off, f.capture_mode ? PathReason::CaptureMode : PathReason::Scene, "linear output");
    else add("fxaa", PathState::Off, by(S.fxaa == tier.fxaa), "edges are not anti-aliased");

    // Shadows and lights.
    if (!f.sun_shadows_requested) add("sun_shadows", PathState::Off, PathReason::Scene, "no shadow-casting directional light");
    else {
        const std::string d = std::format("{}, {} px", count(f.cascades, "cascade", "cascades"), f.cascade_resolution);
        const bool reduced = S.shadow_cascades < ref.shadow_cascades || S.shadow_resolution < ref.shadow_resolution || !S.soft_shadows ||
                             S.fast_soft_shadows != ref.fast_soft_shadows;
        const bool matches = S.shadow_cascades == tier.shadow_cascades && S.shadow_resolution == tier.shadow_resolution &&
                             S.soft_shadows == tier.soft_shadows && S.fast_soft_shadows == tier.fast_soft_shadows;
        add("sun_shadows", reduced ? PathState::Reduced : PathState::Active, reduced ? by(matches) : PathReason::None, d);
    }
    if (f.local_lights_visible == 0) add("local_lights", PathState::Off, PathReason::NotNeeded, "no point or spot lights in view");
    else if (f.local_lights_kept < f.local_lights_visible)
        add("local_lights", PathState::Reduced, PathReason::Budget,
            std::format("{} of {} lights in view (max_point_lights {}); the farthest are dropped", f.local_lights_kept, f.local_lights_visible,
                        S.max_point_lights));
    else add("local_lights", PathState::Active, PathReason::None, count(f.local_lights_kept, "light", "lights"));
    if (!S.local_shadows) add("local_shadows", PathState::Off, by(S.local_shadows == tier.local_shadows), "point and spot lights cast no shadows");
    else if (f.local_shadow_candidates == 0) add("local_shadows", PathState::Off, PathReason::NotNeeded, "no shadow-casting local lights in view");
    else if (f.local_shadow_lights < f.local_shadow_candidates)
        add("local_shadows", PathState::Reduced, PathReason::Budget,
            std::format("{} of {} shadowed lights fit the atlas ({} tiles of {} px)", f.local_shadow_lights, f.local_shadow_candidates,
                        f.local_shadow_tiles, S.local_shadow_tile));
    else
        add("local_shadows", PathState::Active, PathReason::None,
            std::format("{}, {} of {} px", count(f.local_shadow_lights, "light", "lights"), count(f.local_shadow_tiles, "tile", "tiles"), S.local_shadow_tile));

    // Indirect light and screen-space effects.
    if (!f.irradiance_available) add("irradiance_volume", PathState::Off, PathReason::Scene, "no baked irradiance volume");
    else if (!S.irradiance_volumes) add("irradiance_volume", PathState::Off, by(S.irradiance_volumes == tier.irradiance_volumes), "");
    else add("irradiance_volume", PathState::Active, PathReason::None, std::format("{} probes", f.irradiance_probes));

    if (!f.ao_requested) add("ssao", PathState::Off, PathReason::Scene, "environment AO intensity or radius is 0");
    else if (!S.ssao) add("ssao", PathState::Off, by(S.ssao == tier.ssao), "no ambient occlusion");
    else {
        const bool reduced = S.ssao_samples < ref.ssao_samples || (S.ssao_half_resolution && !ref.ssao_half_resolution);
        const bool matches = S.ssao_samples == tier.ssao_samples && S.ssao_half_resolution == tier.ssao_half_resolution;
        add("ssao", reduced ? PathState::Reduced : PathState::Active, reduced ? by(matches) : PathReason::None,
            std::format("{} samples, {} resolution", S.ssao_samples, S.ssao_half_resolution ? "half" : "full"));
    }
    if (!f.bloom_requested) add("bloom", PathState::Off, PathReason::Scene, "environment bloom intensity is 0");
    else if (!S.bloom) add("bloom", PathState::Off, by(S.bloom == tier.bloom), "");
    else {
        const bool reduced = S.bloom_levels < ref.bloom_levels;
        add("bloom", reduced ? PathState::Reduced : PathState::Active, reduced ? by(S.bloom_levels == tier.bloom_levels) : PathReason::None,
            std::format("{} levels", S.bloom_levels));
    }
    if (!S.auto_exposure) add("auto_exposure", PathState::Off, by(S.auto_exposure == tier.auto_exposure), "fixed exposure");
    else if (!f.auto_exposure_requested) add("auto_exposure", PathState::Off, PathReason::Scene, "the environment uses fixed exposure");
    else add("auto_exposure", PathState::Active, PathReason::None, "");

    if (!f.fog_requested) add("volumetric_fog", PathState::Off, PathReason::Scene, "no volumetric fog or fog volumes");
    else if (f.capture_mode) add("volumetric_fog", PathState::Off, PathReason::CaptureMode, "");
    else if (!f.compute) add("volumetric_fog", PathState::Off, PathReason::Unsupported, "needs compute shaders");
    else if (!S.volumetric_fog) add("volumetric_fog", PathState::Off, by(S.volumetric_fog == tier.volumetric_fog), "");
    else if (!f.fog) add("volumetric_fog", PathState::Off, PathReason::Unsupported, "fog pipelines unavailable");
    else {
        const bool reduced = S.volumetric_fog_tile > ref.volumetric_fog_tile || S.volumetric_fog_slices < ref.volumetric_fog_slices;
        const bool matches = S.volumetric_fog_tile == tier.volumetric_fog_tile && S.volumetric_fog_slices == tier.volumetric_fog_slices;
        add("volumetric_fog", reduced ? PathState::Reduced : PathState::Active, reduced ? by(matches) : PathReason::None,
            std::format("{}x{}x{} froxels", f.fog_x, f.fog_y, f.fog_slices));
    }

    // Surface and effect passes. None of them is scaled by the quality tier today, so the reasons
    // are scene content, view culling and the fixed per-frame budgets.
    if (f.decals_submitted == 0) add("decals", PathState::Off, PathReason::Scene, "no decals in the scene");
    else if (f.decals_in_view == 0) add("decals", PathState::Off, PathReason::NotNeeded, std::format("{} outside the view", count(f.decals_submitted, "decal", "decals")));
    else if (f.decals_drawn < f.decals_in_view)
        add("decals", PathState::Reduced, PathReason::Budget,
            std::format("nearest {} of {} decals in view (max {})", f.decals_drawn, f.decals_in_view, f.decal_budget));
    else add("decals", PathState::Active, PathReason::None, std::format("{} of {} in view", f.decals_drawn, f.decals_submitted));

    if (f.water_submitted == 0) add("water", PathState::Off, PathReason::Scene, "no water surfaces in the scene");
    else if (f.water_in_view == 0) add("water", PathState::Off, PathReason::NotNeeded, std::format("{} outside the view", count(f.water_submitted, "surface", "surfaces")));
    else if (f.water_drawn < f.water_in_view)
        add("water", PathState::Reduced, PathReason::Budget,
            std::format("nearest {} of {} surfaces in view (max {})", f.water_drawn, f.water_in_view, f.water_budget));
    else add("water", PathState::Active, PathReason::None, std::format("{}, refraction from the opaque scene", count(f.water_drawn, "surface", "surfaces")));

    const std::uint32_t emitters = f.particle_batches_submitted + f.gpu_emitters_submitted;
    if (emitters == 0) add("particles", PathState::Off, PathReason::Scene, "no particle emitters in the scene");
    else if (f.particle_batches_drawn == 0 && f.gpu_emitters_submitted == 0)
        add("particles", PathState::Off, PathReason::NotNeeded, std::format("{} outside the view", count(f.particle_batches_submitted, "batch", "batches")));
    else
        add("particles", PathState::Active, PathReason::None,
            std::format("{} in {}{}", count(f.particles_drawn, "particle", "particles"), count(f.particle_batches_drawn, "batch", "batches"),
                        f.ribbon_vertices > 0 ? std::format(", {} ribbon vertices", f.ribbon_vertices) : std::string{}));
    if (f.gpu_emitters_submitted == 0) add("gpu_particles", PathState::Off, PathReason::Scene, "no GPU emitters");
    else if (!f.compute) add("gpu_particles", PathState::Off, PathReason::Unsupported, "needs compute shaders; GPU emitters are not drawn");
    else if (f.gpu_emitters_simulated < f.gpu_emitters_submitted)
        add("gpu_particles", PathState::Reduced, PathReason::Unsupported,
            std::format("{} of {} emitters simulated (pipeline unavailable)", f.gpu_emitters_simulated, f.gpu_emitters_submitted));
    else
        add("gpu_particles", PathState::Active, PathReason::None,
            std::format("{} alive in {}", f.gpu_particles, count(f.gpu_emitters_simulated, "emitter", "emitters")));

    if (f.probes_requested == 0) add("reflection_probes", PathState::Off, PathReason::Scene, "no reflection probes; reflections use the environment map");
    else if (f.probes_used == 0) add("reflection_probes", PathState::Off, PathReason::Scene, std::format("{} not captured yet (bake the probes)", count(f.probes_unbaked, "probe", "probes")));
    else if (f.probes_over_budget > 0)
        add("reflection_probes", PathState::Reduced, PathReason::Budget,
            std::format("{} of {} probes used (max {})", f.probes_used, f.probes_used + f.probes_over_budget, f.probe_budget));
    else if (f.probes_unbaked > 0)
        add("reflection_probes", PathState::Reduced, PathReason::Scene,
            std::format("{} not captured; {} of {} draws inside a probe box", count(f.probes_unbaked, "probe", "probes"), f.probe_draws, f.visible_draws));
    else
        add("reflection_probes", PathState::Active, PathReason::None,
            std::format("{}, {} of {} draws inside a probe box", count(f.probes_used, "probe", "probes"), f.probe_draws, f.visible_draws));

    // Geometry cost reduction (invisible by design; reported so differences can be traced).
    if (!S.lods) add("lods", PathState::Off, by(S.lods == tier.lods), "full-detail meshes everywhere");
    else if (S.force_lod >= 0) add("lods", PathState::Reduced, PathReason::Setting, std::format("forced to LOD {}", S.force_lod));
    else {
        const bool reduced = S.lod_error_pixels > ref.lod_error_pixels;
        add("lods", reduced ? PathState::Reduced : PathState::Active, reduced ? by(S.lod_error_pixels == tier.lod_error_pixels) : PathReason::None,
            std::format("{} of {} draws coarser than LOD 0 at {:.2f} px error, {} held by hysteresis", f.lod_draws, f.visible_draws,
                        S.lod_error_pixels, f.lod_held));
    }
    if (f.occlusion) add("occlusion_culling", PathState::Active, PathReason::None, std::format("{} instances tested on the GPU", f.occlusion_tested));
    else if (!S.occlusion_culling) add("occlusion_culling", PathState::Off, by(S.occlusion_culling == tier.occlusion_culling), "");
    else if (!f.compute) add("occlusion_culling", PathState::Off, PathReason::Unsupported, "needs compute shaders");
    else if (!f.prepass) add("occlusion_culling", PathState::Off, PathReason::Requires, "needs the depth pre-pass (SSAO or depth_prepass)");
    else add("occlusion_culling", PathState::Off, PathReason::NotNeeded, f.nothing_drawn ? "nothing to draw" : "pipelines unavailable");
    if (!S.instancing) add("instancing", PathState::Off, by(S.instancing == tier.instancing), "one draw per object");
    else add("instancing", PathState::Active, PathReason::None, std::format("{} batches for {} draws", f.main_batches, f.instanced_draws));
    if (!f.taa) add("motion_vectors", PathState::Off, PathReason::Requires, "needs TAA");
    else if (!S.motion_vectors) add("motion_vectors", PathState::Off, by(S.motion_vectors == tier.motion_vectors), "camera motion only");
    else add("motion_vectors", PathState::Active, PathReason::None, std::format("{} moving draws", f.moving_draws));
    return r;
}

} // namespace victus::render
