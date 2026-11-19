// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/framework.hpp"
#include "victus/gameplay/water.hpp"

#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <mutex>
#include <cmath>

namespace victus::gameplay {

using json::Value;

void Simulation::add_system(std::string name, Phase phase, SystemFn fn) {
    const char* zone = profiler::intern("system." + name);
    systems_.push_back({std::move(name), phase, std::move(fn), true, zone});
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
}

void Simulation::run_phase(scene::Scene& scene, Phase phase, const FrameTime& t) {
    for (auto& s : systems_) {
        if (!s.enabled || s.phase != phase || !s.fn) continue;
        profiler::ScopedZone zone(s.zone ? s.zone : "system");
        s.fn(scene, t);
    }
}

void Simulation::tick(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("simulation.tick");
    dt = std::clamp(dt, 0.0f, max_frame_dt);
    FrameTime t = time_;
    t.dt = dt;
    run_phase(scene, Phase::PreUpdate, t);
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
            .field(&E::bloom_intensity, "bloom_intensity", {.min = 0, .max = 4, .speed = 0.005f})
            .field(&E::bloom_threshold, "bloom_threshold", {.min = 0, .max = 64, .speed = 0.01f})
            .field(&E::ao_intensity, "ao_intensity", {.min = 0, .max = 4, .speed = 0.01f})
            .field(&E::ao_radius, "ao_radius", {.min = 0.01f, .max = 10, .speed = 0.01f})
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
            .field(&E::environment_map, "environment_map", {.label = "Environment Map", .tooltip = "HDR (.hdr) equirectangular image for image-based lighting, reflections and the sky; empty = analytic sky", .hint = "texture"})
            .field(&E::environment_intensity, "environment_intensity", {.min = 0, .max = 64, .speed = 0.01f, .tooltip = "Scales environment lighting, reflections and background"})
            .field(&E::environment_rotation, "environment_rotation", {.min = -360, .max = 360, .speed = 0.5f, .tooltip = "Degrees about the up axis"})
            .field(&E::sky_blur, "sky_blur", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Blurs the environment background (0 sharp .. 1 rough)"})
            .field(&E::physical_sky, "physical_sky", {.label = "Physical Sky", .tooltip = "Atmospheric scattering sky lit by the directional light; also lights the scene (used when no environment map is set)"})
            .field(&E::atmosphere_density, "atmosphere_density", {.min = 0, .max = 20, .speed = 0.01f, .tooltip = "Air density (1 = Earth): bluer sky, redder sunsets"})
            .field(&E::atmosphere_haze, "atmosphere_haze", {.min = 0, .max = 50, .speed = 0.05f, .tooltip = "Aerosol density (1 = clear): whiter sky, larger sun halo"})
            .field(&E::atmosphere_intensity, "atmosphere_intensity", {.min = 0, .max = 64, .speed = 0.01f, .tooltip = "Sky and ambient brightness relative to the sun"})
            .field(&E::atmosphere_tints_sun, "atmosphere_tints_sun", {.tooltip = "Colour the sun light by the atmosphere (warm, dim sunsets)"})
            .commit();
    });
}

void add_builtin_systems(Simulation& sim) {
    sim.add_system("gameplay.events", Phase::PreUpdate, [](scene::Scene& s, const FrameTime&) { begin_event_tick(s); });
    sim.add_system("gameplay.water", Phase::FixedUpdate, [](scene::Scene& s, const FrameTime& t) { advance_water_time(s, t.dt); });
    sim.add_system("gameplay.timers", Phase::FixedUpdate, [](scene::Scene& s, const FrameTime& t) { update_timers(s, static_cast<float>(t.dt)); });
    sim.add_system("gameplay.attributes", Phase::FixedUpdate,
                   [](scene::Scene& s, const FrameTime& t) { update_attributes(s, static_cast<float>(t.dt)); });
    sim.add_system("builtin.rotator", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        VICTUS_PROFILE_SCOPE("gameplay.rotator");
        s.world().each<RotatorComponent, scene::Transform>([&](const RotatorComponent& r, scene::Transform& tr) {
            if (math::length(r.axis) <= 0) return;
            const math::Quat dq = math::Quat::axis_angle(r.axis, math::radians(r.degrees_per_second) * t.dt);
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
        s.world().each<OscillatorComponent, scene::Transform>([&](OscillatorComponent& o, scene::Transform& tr) {
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
