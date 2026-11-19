// SPDX-License-Identifier: Apache-2.0
// Rendering-path diagnostics (ADR-0103): which rendering features ran last frame, at what output
// quality, and a reason code for every feature that did not run or ran reduced. Answers "why does
// this frame look like that" without reading renderer code, and records the path next to perceptual
// comparisons (ADR-0102) so a difference can be traced to the decision that caused it.
// Costs the platform does not expose are reported as unavailable instead of being estimated.
#pragma once

#include "victus/core/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace victus::render {

struct RenderSettings;

enum class PathState : std::uint8_t { Active, Reduced, Off };

enum class PathReason : std::uint8_t {
    None,         // active at full quality
    QualityTier,  // off or reduced by the quality tier preset
    Setting,      // off or reduced by a render setting that differs from the tier preset
    Scene,        // the scene does not ask for it (no shadowed sun, AO intensity 0, no fog ...)
    NotNeeded,    // nothing to do (no upscale at render scale 1, no moving objects, nothing drawn ...)
    Replaced,     // another feature does the job (FXAA while TAA runs)
    Requires,     // needs another feature that is off (occlusion culling needs the depth pre-pass ...)
    CaptureMode,  // probe/reference capture: linear radiance, no temporal or screen-space effects
    Unsupported,  // the device lacks a capability (compute shaders ...)
    Budget,       // limited by a per-frame budget (light count, shadow atlas tiles)
    NoHistory,    // temporal feature without valid history this frame (first frame, camera cut)
};

[[nodiscard]] std::string_view path_state_name(PathState s) noexcept;
[[nodiscard]] std::string_view path_reason_name(PathReason r) noexcept;

struct PathFeature {
    std::string name;     // stable id, e.g. "ssao", "sun_shadows"
    PathState state{PathState::Off};
    PathReason reason{PathReason::None};
    std::string detail;   // output quality when active ("2 cascades, 1024 px"), the cause otherwise
};

// What the renderer saw and decided this frame; SceneRenderer fills it during render().
struct RenderPathFacts {
    std::uint32_t render_width{0}, render_height{0}, output_width{0}, output_height{0};
    bool capture_mode{false}, linear_output{false}, compute{true};
    bool taa{false}, taa_history{false}, temporal_upscale{false}, spatial_upscale{false}, fxaa{false};
    bool sun_shadows_requested{false};
    std::uint32_t cascades{0}, cascade_resolution{0};
    bool ao_requested{false}, ssao{false};
    bool bloom_requested{false}, bloom{false};
    bool auto_exposure_requested{false}, auto_exposure{false};
    bool prepass{false}, occlusion{false}, nothing_drawn{false};
    std::uint32_t occlusion_tested{0};
    bool fog_requested{false}, fog{false};
    std::uint32_t fog_x{0}, fog_y{0}, fog_slices{0};
    bool clouds_requested{false}, clouds{false};
    std::uint32_t cloud_width{0}, cloud_height{0}, cloud_steps{0};
    bool precipitation_requested{false};
    std::uint32_t precipitation_drops{0};
    bool irradiance_available{false}, irradiance{false};
    std::uint32_t irradiance_probes{0};
    std::uint32_t local_lights_visible{0}, local_lights_kept{0};
    std::uint32_t local_shadow_candidates{0}, local_shadow_lights{0}, local_shadow_tiles{0};
    std::uint32_t visible_draws{0}, lod_draws{0}, lod_held{0}, moving_draws{0};
    std::uint32_t main_batches{0}, instanced_draws{0};
    // Surface and effect passes (submitted this frame -> inside the view -> drawn).
    std::uint32_t decals_submitted{0}, decals_in_view{0}, decals_drawn{0}, decal_budget{0};
    std::uint32_t water_submitted{0}, water_in_view{0}, water_drawn{0}, water_budget{0};
    std::uint32_t particle_batches_submitted{0}, particle_batches_drawn{0}, particles_drawn{0}, ribbon_vertices{0};
    // CPU batches and their particles (billboards + ribbon vertices) inside the view, before the
    // max_particles budget (ADR-0109).
    std::uint32_t particle_batches_in_view{0}, particles_in_view{0}, particle_budget{0};
    std::uint32_t gpu_emitters_submitted{0}, gpu_emitters_simulated{0}, gpu_particles{0};
    std::uint32_t gpu_emitters_sorted{0}; // alpha-blended GPU emitters drawn back to front (ADR-0107)
    std::uint32_t gpu_emitters_trailed{0}, gpu_trails_reduced{0}; // GPU emitters with trails / with fewer points than asked (ADR-0110)
    std::uint32_t probes_requested{0}, probes_unbaked{0}, probes_over_budget{0}, probes_used{0}, probe_budget{0};
    std::uint32_t probe_draws{0};
};

struct RenderPathReport {
    std::string tier;              // quality tier name
    float render_scale{1.0f};
    std::uint32_t render_width{0}, render_height{0}, output_width{0}, output_height{0};
    std::vector<PathFeature> features;
    // Costs: CPU time per pass comes from the profiler (`victus run --profile`); GPU execution
    // time needs timestamp queries, which SDL_GPU does not expose, so it is reported unavailable.
    std::string gpu_timing{"unavailable: the SDL_GPU backend exposes no GPU timestamp queries"};

    [[nodiscard]] const PathFeature* find(std::string_view name) const noexcept;
    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] std::string to_text() const; // one line per feature
    // Short list of what is below the reference path: reduced features, and features that are off
    // for a reason other than the scene not asking for them. Empty when everything requested runs
    // at full quality. Example: "ssao (quality_tier), local_lights (budget)".
    [[nodiscard]] std::string reduced_summary() const;
};

[[nodiscard]] RenderPathReport describe_render_path(const RenderSettings& settings, const RenderPathFacts& facts);

} // namespace victus::render
