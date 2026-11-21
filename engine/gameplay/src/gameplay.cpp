// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/framework.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/gameplay/water.hpp"

#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <mutex>
#include <cmath>

namespace victus::gameplay {

using json::Value;

void Simulation::add_system(std::string name, Phase phase, SystemFn fn) {
    add_system(std::move(name), phase, std::move(fn), SystemOptions{});
}

void Simulation::add_system(std::string name, Phase phase, SystemFn fn, SystemOptions options) {
    const char* zone = profiler::intern("system." + name);
    options.interval = std::max<std::uint32_t>(options.interval, 1);
    options.offset %= options.interval;
    SystemInfo info;
    info.name = std::move(name);
    info.phase = phase;
    info.fn = std::move(fn);
    info.zone = zone;
    info.options = std::move(options);
    systems_.push_back(std::move(info));
    order_dirty_ = true;
}

SystemInfo* Simulation::find(std::string_view name) {
    for (auto& s : systems_)
        if (s.name == name) return &s;
    return nullptr;
}

void Simulation::reset() noexcept {
    accumulator_ = 0;
    time_ = {};
    fixed_steps_ = 0;
    for (auto& s : systems_) {
        s.passes = 0;
        s.runs = 0;
        s.pending_dt = 0;
    }
}

// Stable topological sort per phase (Kahn's algorithm, ready systems taken in registration
// order). Members of a cycle are appended in registration order and reported.
void Simulation::build_order() {
    if (!order_dirty_) return;
    order_dirty_ = false;
    diagnostics_.clear();
    for (std::size_t p = 0; p < order_.size(); ++p) {
        auto& order = order_[p];
        order.clear();
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < systems_.size(); ++i)
            if (static_cast<std::size_t>(systems_[i].phase) == p) members.push_back(i);
        if (members.empty()) continue;
        const auto local = [&](std::string_view name) -> std::ptrdiff_t {
            for (std::size_t k = 0; k < members.size(); ++k)
                if (systems_[members[k]].name == name) return static_cast<std::ptrdiff_t>(k);
            return -1;
        };
        std::vector<std::vector<std::size_t>> edges(members.size()); // a -> b: a runs before b
        std::vector<std::size_t> indegree(members.size(), 0);
        const auto edge = [&](std::size_t a, std::size_t b) {
            if (a == b) return;
            if (std::find(edges[a].begin(), edges[a].end(), b) != edges[a].end()) return;
            edges[a].push_back(b);
            ++indegree[b];
        };
        for (std::size_t k = 0; k < members.size(); ++k) {
            const SystemInfo& s = systems_[members[k]];
            for (const auto& a : s.options.after) {
                const auto o = local(a);
                if (o < 0) diagnostics_.push_back("system '" + s.name + "': after '" + a + "' is not in its phase");
                else edge(static_cast<std::size_t>(o), k);
            }
            for (const auto& b : s.options.before) {
                const auto o = local(b);
                if (o < 0) diagnostics_.push_back("system '" + s.name + "': before '" + b + "' is not in its phase");
                else edge(k, static_cast<std::size_t>(o));
            }
        }
        // Members of cycles: whatever Kahn's algorithm cannot remove.
        std::vector<std::size_t> in = indegree;
        std::vector<bool> removed(members.size(), false);
        for (bool progress = true; progress;) {
            progress = false;
            for (std::size_t k = 0; k < members.size(); ++k)
                if (!removed[k] && in[k] == 0) {
                    removed[k] = true;
                    progress = true;
                    for (const std::size_t b : edges[k]) --in[b];
                }
        }
        // Each system is placed in registration order, right after the systems it must follow
        // (placed first, recursively), so constraints move as few systems as possible.
        std::vector<std::vector<std::size_t>> preds(members.size());
        for (std::size_t k = 0; k < members.size(); ++k)
            for (const std::size_t b : edges[k]) preds[b].push_back(k);
        for (auto& pr : preds) std::sort(pr.begin(), pr.end());
        std::vector<bool> placed(members.size(), false);
        const auto place = [&](auto&& self, std::size_t k) -> void {
            if (placed[k] || !removed[k]) return;
            placed[k] = true;
            for (const std::size_t q : preds[k]) self(self, q);
            order.push_back(members[k]);
        };
        // A system's predecessors are emitted before it, so mark-then-recurse is safe in a DAG.
        for (std::size_t k = 0; k < members.size(); ++k) {
            if (placed[k] || !removed[k]) continue;
            place(place, k);
        }
        std::string cycle;
        for (std::size_t k = 0; k < members.size(); ++k)
            if (!removed[k]) {
                if (!cycle.empty()) cycle += ", ";
                cycle += systems_[members[k]].name;
                order.push_back(members[k]);
            }
        if (!cycle.empty()) diagnostics_.push_back("ordering cycle among: " + cycle + " (registration order kept)");
    }
}

std::vector<std::string> Simulation::schedule(Phase phase) {
    build_order();
    std::vector<std::string> names;
    for (const std::size_t i : order_[static_cast<std::size_t>(phase)]) names.push_back(systems_[i].name);
    return names;
}

const std::vector<std::string>& Simulation::schedule_diagnostics() {
    build_order();
    return diagnostics_;
}

void Simulation::run_phase(scene::Scene& scene, Phase phase, const FrameTime& t) {
    build_order();
    for (const std::size_t i : order_[static_cast<std::size_t>(phase)]) {
        SystemInfo& s = systems_[i];
        if (!s.enabled || !s.fn) continue;
        const std::uint32_t interval = s.options.interval;
        const std::uint64_t pass = s.passes++;
        if (interval <= 1) {
            ++s.runs;
            profiler::ScopedZone zone(s.zone ? s.zone : "system");
            s.fn(scene, t);
            continue;
        }
        s.pending_dt += t.dt;
        if ((pass + s.options.offset) % interval != 0) continue;
        FrameTime st = t;
        st.dt = static_cast<float>(s.pending_dt);
        s.pending_dt = 0;
        ++s.runs;
        profiler::ScopedZone zone(s.zone ? s.zone : "system");
        s.fn(scene, st);
    }
}

bool game_paused(scene::Scene& scene) noexcept {
    const auto* p = scene.world().resource<GamePause>();
    return p && p->paused;
}

void set_game_paused(scene::Scene& scene, bool paused) {
    if (auto* p = scene.world().resource<GamePause>()) p->paused = paused;
    else if (paused) scene.world().set_resource<GamePause>().paused = true;
}

void Simulation::tick(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("simulation.tick");
    dt = std::clamp(dt, 0.0f, max_frame_dt);
    FrameTime t = time_;
    t.dt = dt;
    run_phase(scene, Phase::PreUpdate, t);
    if (game_paused(scene)) {
        FrameTime still = t;
        still.dt = 0;
        run_phase(scene, Phase::PostUpdate, still);
        scene.update_transforms();
        ++time_.frame;
        return;
    }
    accumulator_ += dt;
    std::uint32_t steps = 0;
    while (accumulator_ >= fixed_dt_ && steps < max_fixed_steps) {
        FrameTime ft = t;
        ft.dt = fixed_dt_;
        ft.time = static_cast<double>(fixed_steps_) * fixed_dt_;
        run_phase(scene, Phase::FixedUpdate, ft);
        accumulator_ -= fixed_dt_;
        ++fixed_steps_;
        ++steps;
    }
    if (steps == max_fixed_steps) accumulator_ = std::min(accumulator_, static_cast<double>(fixed_dt_));
    run_phase(scene, Phase::Update, t);
    run_phase(scene, Phase::PostUpdate, t);
    scene.update_transforms();
    time_.time += dt;
    time_.dt = dt;
    ++time_.frame;
}

void register_components() {
    register_framework_components();
    register_water_components();
    register_simulation_lod_components();
    auto& r = scene::SerializerRegistry::instance();
    r.add<RotatorComponent>(
        "Rotator",
        [](const RotatorComponent& c) {
            Value v = Value::object();
            v.set("axis", scene::to_json(c.axis));
            v.set("degrees_per_second", c.degrees_per_second);
            return v;
        },
        [](RotatorComponent& c, const Value& v) -> Status {
            if (const Value* a = v.find("axis")) scene::from_json(*a, c.axis);
            c.degrees_per_second = v["degrees_per_second"].as_float(c.degrees_per_second);
            return {};
        });
    r.add<OscillatorComponent>(
        "Oscillator",
        [](const OscillatorComponent& c) {
            Value v = Value::object();
            v.set("amplitude", scene::to_json(c.amplitude));
            v.set("frequency", c.frequency);
            v.set("phase", c.phase);
            return v;
        },
        [](OscillatorComponent& c, const Value& v) -> Status {
            if (const Value* a = v.find("amplitude")) scene::from_json(*a, c.amplitude);
            c.frequency = v["frequency"].as_float(c.frequency);
            c.phase = v["phase"].as_float(c.phase);
            return {};
        });
    static std::once_flag once;
    std::call_once(once, [] {
        using O = MaterialOverridesComponent;
        scene::Schema<O>("MaterialOverrides", "Rendering")
            .field(&O::uv_offset, "uv_offset", {.speed = 0.01f, .label = "UV Offset", .tooltip = "Added to the material's texture offset"})
            .field(&O::uv_scroll, "uv_scroll", {.min = -100, .max = 100, .speed = 0.01f, .label = "UV Scroll", .tooltip = "Texture scroll in UV units per second while playing"})
            .field(&O::override_emissive, "override_emissive", {.label = "Override Emissive", .tooltip = "Replace the material's emissive colour"})
            .field(&O::emissive, "emissive", {.label = "Emissive", .hint = "color_hdr"})
            .field(&O::emissive_strength, "emissive_strength", {.min = 0, .max = 1000, .speed = 0.05f, .label = "Emissive Strength"})
            .commit();
        using P = ReflectionProbeComponent;
        scene::Schema<P>("ReflectionProbe", "Rendering")
            .field(&P::size, "size", {.min = 0.1f, .max = 10000, .speed = 0.05f, .tooltip = "World-aligned box (metres) that receives this probe and corrects its parallax"})
            .field(&P::capture_offset, "capture_offset", {.speed = 0.05f, .tooltip = "Capture point relative to the entity; keep it inside the box"})
            .field(&P::intensity, "intensity", {.min = 0, .max = 64, .speed = 0.01f})
            .field(&P::resolution, "resolution", {.min = 16, .max = 512, .tooltip = "Cubemap face size of the capture"})
            .field(&P::revision, "revision", {.tooltip = "Change to recapture (also recaptured when the probe moves)"})
            .commit();
        using G = IrradianceVolumeComponent;
        scene::Schema<G>("IrradianceVolume", "Rendering")
            .field(&G::size, "size", {.min = 0.1f, .max = 10000, .speed = 0.05f, .tooltip = "World-aligned box (metres) filled with irradiance probes"})
            .field(&G::spacing, "spacing", {.min = 0.25f, .max = 100, .speed = 0.05f, .tooltip = "Distance between probes; at most 32 per axis (16 vertically)"})
            .field(&G::intensity, "intensity", {.min = 0, .max = 64, .speed = 0.01f})
            .field(&G::normal_offset, "normal_offset", {.min = 0, .max = 4, .speed = 0.01f, .tooltip = "Pushes lookups off surfaces to reduce light leaking through walls"})
            .field(&G::resolution, "resolution", {.min = 16, .max = 128, .tooltip = "Capture face size per probe"})
            .field(&G::bounces, "bounces", {.min = 1, .max = 4, .tooltip = "Bake passes; each pass after the first adds one light bounce"})
            .field(&G::revision, "revision", {.tooltip = "Change to rebake (also rebaked when the volume moves or resizes)"})
            .field(&G::dynamic, "dynamic", {.tooltip = "Keep re-capturing probes so lighting changes propagate (High/Ultra quality)"})
            .field(&G::dynamic_blend, "dynamic_blend", {.min = 0.01f, .max = 1, .speed = 0.01f, .tooltip = "Weight of each new capture: higher reacts faster, lower is steadier"})
            .commit();
        using L2 = Light2DComponent;
        scene::Schema<L2>("Light2D", "Rendering")
            .enumeration(&L2::type, "type", {"point", "spot", "global"})
            .field(&L2::color, "color", {.hint = "color_hdr"})
            .field(&L2::intensity, "intensity", {.min = 0, .max = 64, .speed = 0.01f})
            .field(&L2::radius, "radius", {.min = 0.01f, .max = 10000, .speed = 0.05f, .tooltip = "Reach in world units; light fades smoothly to zero here"})
            .field(&L2::inner_angle_degrees, "inner_angle_degrees", {.min = 0, .max = 180, .speed = 0.5f, .label = "Inner Angle", .tooltip = "Spot: full light inside this half-angle", .hint = "degrees"})
            .field(&L2::outer_angle_degrees, "outer_angle_degrees", {.min = 0.1f, .max = 180, .speed = 0.5f, .label = "Outer Angle", .tooltip = "Spot: no light beyond this half-angle (spots shine along local -Y)", .hint = "degrees"})
            .field(&L2::falloff, "falloff", {.min = 0.05f, .max = 16, .speed = 0.01f, .tooltip = "Below 1 spreads light further, above 1 concentrates it at the centre"})
            .field(&L2::enabled, "enabled")
            .commit();
        using D = DecalComponent;
        scene::Schema<D>("Decal", "Rendering")
            .field(&D::texture, "texture", {.tooltip = "Projected texture (alpha = coverage); empty = solid colour", .hint = "texture"})
            .field(&D::size, "size", {.min = 0.01f, .max = 1000, .speed = 0.01f, .tooltip = "Box width, height and projection depth in metres (local -Z projects)"})
            .field(&D::color, "color", {.tooltip = "Linear tint; alpha is the opacity", .hint = "color_hdr"})
            .enumeration(&D::blend, "blend", {"multiply", "alpha", "additive"})
            .field(&D::uv_min, "uv_min", {.min = 0, .max = 1, .speed = 0.005f, .label = "UV Min", .tooltip = "Top-left of the texture region"})
            .field(&D::uv_max, "uv_max", {.min = 0, .max = 1, .speed = 0.005f, .label = "UV Max", .tooltip = "Bottom-right of the texture region"})
            .field(&D::angle_fade, "angle_fade", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Fades surfaces facing away from the projection axis (0 = none)"})
            .field(&D::depth_fade, "depth_fade", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Fraction of the box depth over which the decal fades at its ends"})
            .field(&D::sort_order, "sort_order", {.min = -1000, .max = 1000, .tooltip = "Higher values draw over lower ones"})
            .field(&D::visible, "visible")
            .commit();
        using E = EnvironmentComponent;
        scene::Schema<E>("Environment", "Rendering")
            .field(&E::sky_zenith, "sky_zenith", {.hint = "color_hdr"})
            .field(&E::sky_horizon, "sky_horizon", {.hint = "color_hdr"})
            .field(&E::ground, "ground", {.hint = "color_hdr"})
            .field(&E::ambient_intensity, "ambient_intensity", {.min = 0, .max = 16, .speed = 0.01f})
            .field(&E::draw_sky, "draw_sky")
            .field(&E::clear_color, "clear_color", {.tooltip = "Background when the sky is not drawn"})
            .enumeration(&E::tonemapper, "tonemapper", {"none", "aces", "neutral", "reinhard"})
            .field(&E::exposure, "exposure", {.min = 0.001f, .max = 1000, .speed = 0.01f, .tooltip = "Manual exposure multiplier"})
            .field(&E::exposure_compensation, "exposure_compensation", {.min = -10, .max = 10, .speed = 0.05f, .tooltip = "EV stops"})
            .field(&E::auto_exposure, "auto_exposure", {.tooltip = "Eye adaptation to the scene's average brightness"})
            .field(&E::auto_exposure_min_ev, "auto_exposure_min_ev", {.min = -16, .max = 16, .speed = 0.05f})
            .field(&E::auto_exposure_max_ev, "auto_exposure_max_ev", {.min = -16, .max = 16, .speed = 0.05f})
            .field(&E::saturation, "saturation", {.min = 0, .max = 2, .speed = 0.01f})
            .field(&E::contrast, "contrast", {.min = 0.5f, .max = 2, .speed = 0.01f})
            .field(&E::vignette, "vignette", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&E::lens_distortion, "lens_distortion", {.min = -1, .max = 1, .speed = 0.01f, .tooltip = "Barrel (> 0) or pincushion (< 0) lens distortion"})
            .field(&E::chromatic_aberration, "chromatic_aberration", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Lateral colour fringing toward the frame edges"})
            .field(&E::film_grain, "film_grain", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Animated sensor noise, strongest in the shadows"})
            .field(&E::bloom_intensity, "bloom_intensity", {.min = 0, .max = 4, .speed = 0.005f})
            .field(&E::bloom_threshold, "bloom_threshold", {.min = 0, .max = 64, .speed = 0.01f})
            .field(&E::ao_intensity, "ao_intensity", {.min = 0, .max = 4, .speed = 0.01f})
            .field(&E::ao_radius, "ao_radius", {.min = 0.01f, .max = 10, .speed = 0.01f})
            .field(&E::contact_shadow_length, "contact_shadow_length", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Screen-space sun shadows for small details the shadow map misses (feet, cracks, grass); metres, 0 = off"})
            .field(&E::contact_shadow_thickness, "contact_shadow_thickness", {.min = 0.01f, .max = 10, .speed = 0.01f, .tooltip = "How thick on-screen occluders are assumed to be (larger = fewer light leaks, more false shadows)"})
            .field(&E::shadow_distance, "shadow_distance", {.min = 1, .max = 5000, .speed = 0.5f})
            .enumeration(&E::fog, "fog", {"none", "linear", "exponential", "exponential_squared"})
            .field(&E::fog_color, "fog_color", {.hint = "color_hdr"})
            .field(&E::fog_start, "fog_start", {.min = 0, .max = 100000, .speed = 0.5f, .tooltip = "Metres from the camera with no fog"})
            .field(&E::fog_end, "fog_end", {.min = 0, .max = 100000, .speed = 0.5f, .tooltip = "Linear fog: fully fogged at this distance"})
            .field(&E::fog_density, "fog_density", {.min = 0, .max = 1, .speed = 0.0005f, .tooltip = "Exponential fog, per metre"})
            .field(&E::fog_max_opacity, "fog_max_opacity", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&E::coverage_amount, "coverage_amount", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Snow/dust layer on upward faces"})
            .field(&E::coverage_color, "coverage_color")
            .field(&E::coverage_roughness, "coverage_roughness", {.min = 0.05f, .max = 1, .speed = 0.01f})
            .field(&E::coverage_slope, "coverage_slope", {.min = -1, .max = 1, .speed = 0.01f, .tooltip = "Surface normal Y where coverage starts"})
            .field(&E::lightmap, "lightmap", {.label = "Lightmap", .tooltip = "Project-relative baked lightmap (.vlightmap) for Lightmapped mesh renderers; written by Bake Lighting", .hint = "file"})
            .field(&E::environment_map, "environment_map", {.label = "Environment Map", .tooltip = "HDR (.hdr) equirectangular image for image-based lighting, reflections and the sky; empty = analytic sky", .hint = "texture"})
            .field(&E::environment_intensity, "environment_intensity", {.min = 0, .max = 64, .speed = 0.01f, .tooltip = "Scales environment lighting, reflections and background"})
            .field(&E::environment_rotation, "environment_rotation", {.min = -360, .max = 360, .speed = 0.5f, .tooltip = "Degrees about the up axis"})
            .field(&E::sky_blur, "sky_blur", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Blurs the environment background (0 sharp .. 1 rough)"})
            .field(&E::physical_sky, "physical_sky", {.label = "Physical Sky", .tooltip = "Atmospheric scattering sky lit by the directional light; also lights the scene (used when no environment map is set)"})
            .field(&E::atmosphere_density, "atmosphere_density", {.min = 0, .max = 20, .speed = 0.01f, .tooltip = "Air density (1 = Earth): bluer sky, redder sunsets"})
            .field(&E::atmosphere_haze, "atmosphere_haze", {.min = 0, .max = 50, .speed = 0.05f, .tooltip = "Aerosol density (1 = clear): whiter sky, larger sun halo"})
            .field(&E::atmosphere_intensity, "atmosphere_intensity", {.min = 0, .max = 64, .speed = 0.01f, .tooltip = "Sky and ambient brightness relative to the sun"})
            .field(&E::atmosphere_tints_sun, "atmosphere_tints_sun", {.tooltip = "Colour the sun light by the atmosphere (warm, dim sunsets)"})
            .field(&E::volumetric_fog, "volumetric_fog", {.label = "Volumetric Fog", .tooltip = "Lit, shadowed fog with light shafts (off on the Low quality tier)"})
            .field(&E::volumetric_density, "volumetric_density", {.min = 0, .max = 2, .speed = 0.001f, .tooltip = "Extinction per metre at and below the fog height"})
            .field(&E::volumetric_height, "volumetric_height", {.min = -10000, .max = 10000, .speed = 0.1f, .tooltip = "World height where the fog starts thinning"})
            .field(&E::volumetric_falloff, "volumetric_falloff", {.min = 0, .max = 10, .speed = 0.005f, .tooltip = "Thinning per metre above the fog height (0 = uniform)"})
            .field(&E::volumetric_albedo, "volumetric_albedo", {.tooltip = "Scattered fraction of the light the fog absorbs (colour of lit fog)"})
            .field(&E::volumetric_anisotropy, "volumetric_anisotropy", {.min = -0.95f, .max = 0.95f, .speed = 0.01f, .tooltip = "0 scatters evenly, towards 1 glows around lights seen through the fog"})
            .field(&E::volumetric_distance, "volumetric_distance", {.min = 8, .max = 1000, .speed = 0.5f, .tooltip = "Metres of view depth the fog covers"})
            .field(&E::volumetric_sun, "volumetric_sun", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Sun light scattered by the fog"})
            .field(&E::volumetric_ambient, "volumetric_ambient", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Sky light scattered by the fog"})
            .field(&E::volumetric_local_lights, "volumetric_local_lights", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Point and spot light scattered by the fog"})
            .field(&E::volumetric_temporal, "volumetric_temporal", {.min = 0, .max = 0.98f, .speed = 0.01f, .tooltip = "Blend with previous frames: smoother but slower to react"})
            .field(&E::volumetric_noise, "volumetric_noise", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Breaks the fog into drifting patches (0 = even fog)"})
            .field(&E::volumetric_noise_scale, "volumetric_noise_scale", {.min = 0.25f, .max = 1000, .speed = 0.05f, .tooltip = "Size of the fog patches in metres"})
            .field(&E::volumetric_wind, "volumetric_wind", {.speed = 0.05f, .tooltip = "Metres per second the fog patches drift"})
            .field(&E::volumetric_self_shadow, "volumetric_self_shadow", {.min = 0, .max = 4, .speed = 0.01f, .tooltip = "Thick fog shades the sun light behind it (1 = physical, 0 = off)"})
            .field(&E::clouds, "clouds", {.label = "Volumetric Clouds", .tooltip = "Ray-marched cloud layer lit by the sun (off on the Low quality tier)"})
            .field(&E::cloud_coverage, "cloud_coverage", {.min = 0, .max = 1, .speed = 0.005f, .tooltip = "0 clear sky .. 1 overcast"})
            .field(&E::cloud_density, "cloud_density", {.min = 0, .max = 1, .speed = 0.001f, .tooltip = "Extinction per metre inside dense cloud (thicker, darker clouds)"})
            .field(&E::cloud_bottom, "cloud_bottom", {.min = -10000, .max = 20000, .speed = 10.0f, .tooltip = "World height of the cloud base (m)"})
            .field(&E::cloud_thickness, "cloud_thickness", {.min = 10, .max = 20000, .speed = 10.0f, .tooltip = "Depth of the cloud layer (m)"})
            .field(&E::cloud_size, "cloud_size", {.min = 10, .max = 100000, .speed = 10.0f, .tooltip = "Size of the cloud shapes (m)"})
            .field(&E::cloud_detail, "cloud_detail", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Erodes cloud edges into wisps"})
            .field(&E::cloud_wind, "cloud_wind", {.tooltip = "Cloud drift in metres per second"})
            .field(&E::cloud_anisotropy, "cloud_anisotropy", {.min = 0, .max = 0.95f, .speed = 0.01f, .tooltip = "Bright silver lining when looking towards the sun"})
            .field(&E::cloud_sun, "cloud_sun", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Sun light scattered by the clouds"})
            .field(&E::cloud_ambient, "cloud_ambient", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Sky light scattered by the clouds"})
            .field(&E::cloud_distance, "cloud_distance", {.min = 100, .max = 200000, .speed = 100.0f, .tooltip = "Metres of view ray covered; the layer fades out towards it"})
            .field(&E::cloud_shadows, "cloud_shadows", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "How strongly the clouds shade the sun light on the ground (0 = no cloud shadows)"})
            .enumeration(&E::precipitation, "precipitation", {"none", "rain", "snow"})
            .field(&E::precipitation_intensity, "precipitation_intensity", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "0 none .. 1 the quality tier's full drop count"})
            .field(&E::precipitation_wind, "precipitation_wind", {.tooltip = "Wind in metres per second: slants rain, drifts snow"})
            .field(&E::precipitation_radius, "precipitation_radius", {.min = 2, .max = 100, .speed = 0.1f, .tooltip = "Metres around the camera that rain or snow fills"})
            .field(&E::precipitation_size, "precipitation_size", {.min = 0, .max = 1, .speed = 0.001f, .tooltip = "Drop width or flake size in metres (0 = automatic)"})
            .field(&E::precipitation_speed, "precipitation_speed", {.min = 0, .max = 100, .speed = 0.05f, .tooltip = "Fall speed in metres per second (0 = 9 for rain, 1.2 for snow)"})
            .field(&E::precipitation_color, "precipitation_color", {.tooltip = "Tint of the drops or flakes"})
            .field(&E::precipitation_opacity, "precipitation_opacity", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Opacity of each drop (0 = automatic)"})
            .field(&E::precipitation_brightness, "precipitation_brightness", {.min = 0, .max = 16, .speed = 0.01f, .tooltip = "Light on the drops or flakes"})
            .field(&E::wetness, "wetness", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Wet surfaces: rough materials darken, everything facing up gets glossier"})
            .field(&E::puddles, "puddles", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "How much flat ground holds standing water (0 none, 1 all)"})
            .field(&E::puddle_size, "puddle_size", {.min = 0.25f, .max = 100, .speed = 0.05f, .tooltip = "Typical puddle size in metres"})
            .field(&E::precipitation_ripples, "precipitation_ripples", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Rain ripples on puddles and wet ground"})
            .field(&E::precipitation_splashes, "precipitation_splashes", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Rain splashes where drops land"})
            .commit();
        using FV = FogVolumeComponent;
        scene::Schema<FV>("FogVolume", "Rendering")
            .field(&FV::size, "size", {.min = 0.01f, .max = 10000, .speed = 0.05f, .tooltip = "Box size in metres (scaled by the entity)"})
            .field(&FV::density, "density", {.min = 0, .max = 10, .speed = 0.005f, .tooltip = "Extinction per metre added inside the box"})
            .field(&FV::albedo, "albedo", {.tooltip = "Colour of the light the box scatters"})
            .field(&FV::edge_fade, "edge_fade", {.min = 0, .max = 0.5f, .speed = 0.005f, .tooltip = "Soft edge width as a fraction of the box size"})
            .field(&FV::visible, "visible")
            .commit();
    });
}

void add_builtin_systems(Simulation& sim) {
    sim.add_system("gameplay.events", Phase::PreUpdate, [](scene::Scene& s, const FrameTime&) { begin_event_tick(s); });
    sim.add_system("simlod.assign", Phase::PreUpdate, [](scene::Scene& s, const FrameTime& t) { update_simulation_lod(s, t); });
    sim.add_system("gameplay.water", Phase::FixedUpdate, [](scene::Scene& s, const FrameTime& t) { advance_water_time(s, t.dt); });
    sim.add_system("gameplay.timers", Phase::FixedUpdate, [](scene::Scene& s, const FrameTime& t) { update_timers(s, static_cast<float>(t.dt)); });
    sim.add_system("gameplay.attributes", Phase::FixedUpdate,
                   [](scene::Scene& s, const FrameTime& t) { update_attributes(s, static_cast<float>(t.dt)); });
    sim.add_system("builtin.rotator", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        VICTUS_PROFILE_SCOPE("gameplay.rotator");
        auto& w = s.world();
        w.each<RotatorComponent, scene::Transform>([&](scene::Entity e, const RotatorComponent& r, scene::Transform& tr) {
            if (math::length(r.axis) <= 0) return;
            const LodTick lt = simulation_lod_tick(w.get<SimulationLodComponent>(e), t.dt);
            if (!lt.due) return;
            const math::Quat dq = math::Quat::axis_angle(r.axis, math::radians(r.degrees_per_second) * lt.dt);
            tr.rotation = math::normalize(tr.rotation * dq);
        });
    });
    sim.add_system("builtin.uv_scroll", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        s.world().each<MaterialOverridesComponent>([&](MaterialOverridesComponent& o) {
            if (o.uv_scroll.x == 0 && o.uv_scroll.y == 0) return;
            o.scrolled.x += o.uv_scroll.x * t.dt;
            o.scrolled.y += o.uv_scroll.y * t.dt;
            o.scrolled.x -= std::floor(o.scrolled.x); // stay in [0, 1) so precision never degrades
            o.scrolled.y -= std::floor(o.scrolled.y);
        });
    });
    sim.add_system("builtin.oscillator", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        VICTUS_PROFILE_SCOPE("gameplay.oscillator");
        const double now = t.time + t.dt;
        auto& w = s.world();
        w.each<OscillatorComponent, scene::Transform>([&](scene::Entity e, OscillatorComponent& o, scene::Transform& tr) {
            if (o.has_origin && !simulation_lod_tick(w.get<SimulationLodComponent>(e), t.dt).due) return;
            if (!o.has_origin) {
                o.origin = tr.position;
                o.has_origin = true;
            }
            const float k = static_cast<float>(std::sin(2.0 * 3.14159265358979 * o.frequency * now + o.phase));
            tr.position = o.origin + o.amplitude * k;
        });
    });
}

} // namespace victus::gameplay
