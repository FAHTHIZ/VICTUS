// SPDX-License-Identifier: Apache-2.0
// Gerstner water volumes (ADR-0095). Formulas follow Tessendorf ("Simulating Ocean Water") and
// GPU Gems ch. 1: P = (x + sum Q A Dx cos t, sum A sin t, z + sum Q A Dz cos t), t = k D.x - w t.
// engine/render/shaders/water.vert and water.frag evaluate exactly the same sums.
#include "victus/gameplay/water.hpp"

#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace victus::gameplay {

namespace {

struct WaterClock {
    double time{0};
};

// ADR-0261: spectra and their latest tiles, per water entity.
struct OceanCache {
    struct Entry {
        std::shared_ptr<const ocean::Spectrum> spectrum;
        std::shared_ptr<const ocean::Field> field;
    };
    std::unordered_map<std::uint64_t, Entry> entries;
};

constexpr float kGravity = 9.81f;
// Secondary waves relative to the primary: wavelength ratio and direction offset (degrees).
// Irrational-ish ratios avoid visible repetition; equal steepness keeps the crests coherent.
constexpr std::array<float, kWaterWaves> kLengthRatio{1.0f, 0.618f, 0.382f, 0.236f};
constexpr std::array<float, kWaterWaves> kAngleOffset{0.0f, 23.0f, -31.0f, 47.0f};

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

WaterWaves water_waves(const WaterComponent& w, double time) {
    WaterWaves out{};
    const float height = std::max(finite_or(w.wave_height, 0), 0.0f);
    const float length = std::max(finite_or(w.wavelength, 12), 0.05f);
    const float steep = std::clamp(finite_or(w.steepness, 0), 0.0f, 1.0f);
    const float speed = std::max(finite_or(w.wave_speed, 1), 0.0f);
    const float base = finite_or(w.wave_direction, 0) * std::numbers::pi_v<float> / 180.0f;
    for (std::size_t i = 0; i < kWaterWaves; ++i) {
        WaterWave& wave = out[i];
        const float l = length * kLengthRatio[i];
        const float a = base + kAngleOffset[i] * std::numbers::pi_v<float> / 180.0f;
        wave.direction = {std::cos(a), std::sin(a)};
        wave.k = 2.0f * std::numbers::pi_v<float> / l;
        wave.omega = std::sqrt(kGravity * wave.k) * speed;
        wave.amplitude = 0.5f * height * kLengthRatio[i];
        wave.q = wave.amplitude > 0 ? steep / (wave.k * wave.amplitude * static_cast<float>(kWaterWaves)) : 0.0f;
        wave.phase = static_cast<float>(std::fmod(static_cast<double>(wave.omega) * std::max(time, 0.0), 2.0 * std::numbers::pi));
    }
    return out;
}

WaveSample sample_waves(const WaterWaves& waves, math::Vec2 xz) {
    // Find the undisplaced point p0 whose horizontal displacement lands on xz.
    math::Vec2 p0 = xz;
    for (int it = 0; it < 4; ++it) {
        math::Vec2 d{0, 0};
        for (const WaterWave& w : waves) {
            const float c = std::cos(w.k * (w.direction.x * p0.x + w.direction.y * p0.y) - w.phase);
            d.x += w.q * w.amplitude * w.direction.x * c;
            d.y += w.q * w.amplitude * w.direction.y * c;
        }
        p0 = {xz.x - d.x, xz.y - d.y};
    }
    WaveSample s;
    math::Vec3 n{0, 1, 0};
    for (const WaterWave& w : waves) {
        const float t = w.k * (w.direction.x * p0.x + w.direction.y * p0.y) - w.phase;
        const float sn = std::sin(t), cs = std::cos(t);
        const float wa = w.k * w.amplitude;
        s.height += w.amplitude * sn;
        n.x -= w.direction.x * wa * cs;
        n.z -= w.direction.y * wa * cs;
        n.y -= w.q * wa * sn;
        s.jacobian -= w.q * wa * sn;
    }
    s.normal = math::normalize(n);
    return s;
}

ocean::SpectrumDesc spectrum_desc(const WaterComponent& w) {
    ocean::SpectrumDesc d;
    d.resolution = static_cast<std::uint32_t>(std::clamp(w.spectrum_resolution, 16, 256));
    d.tile_size = w.spectrum_tile;
    d.wave_height = w.wave_height;
    d.peak_wavelength = w.wavelength;
    d.direction = finite_or(w.wave_direction, 0) * std::numbers::pi_v<float> / 180.0f;
    d.choppiness = 2.0f * std::clamp(finite_or(w.steepness, 0), 0.0f, 1.0f);
    d.speed = w.wave_speed;
    d.seed = static_cast<std::uint32_t>(w.spectrum_seed);
    return ocean::sanitize(d);
}

std::shared_ptr<const ocean::Field> water_ocean(scene::Scene& scene, Entity entity, const WaterComponent& w, double time) {
    if (w.waves != WaveModel::Spectrum) return nullptr;
    auto* cache = scene.world().resource<OceanCache>();
    if (!cache) cache = &scene.world().set_resource<OceanCache>();
    OceanCache::Entry& e = cache->entries[entity.bits()];
    const ocean::SpectrumDesc desc = spectrum_desc(w);
    if (!e.spectrum || !(e.spectrum->desc() == desc)) {
        e.spectrum = std::make_shared<const ocean::Spectrum>(desc);
        e.field.reset();
    }
    time = std::isfinite(time) ? std::max(time, 0.0) : 0.0;
    if (!e.field || e.field->time != time) e.field = std::make_shared<const ocean::Field>(e.spectrum->evaluate(time));
    return e.field;
}

WaveSample sample_volume(const WaterVolume& v, math::Vec2 xz) {
    if (!v.ocean) return sample_waves(v.waves, xz);
    const ocean::Sample o = ocean::sample(*v.ocean, xz);
    return {o.height, o.normal, o.jacobian};
}

double water_time(scene::Scene& scene) {
    const auto* c = scene.world().resource<WaterClock>();
    return c ? c->time : 0.0;
}

void advance_water_time(scene::Scene& scene, float dt) {
    if (!(dt > 0) || !std::isfinite(dt)) return;
    auto* c = scene.world().resource<WaterClock>();
    if (!c) c = &scene.world().set_resource<WaterClock>();
    c->time += dt;
}

void set_water_time(scene::Scene& scene, double time) {
    auto* c = scene.world().resource<WaterClock>();
    if (!c) c = &scene.world().set_resource<WaterClock>();
    c->time = std::isfinite(time) ? std::max(time, 0.0) : 0.0;
}

std::vector<WaterVolume> water_volumes(scene::Scene& scene) {
    std::vector<WaterVolume> out;
    const double t = water_time(scene);
    scene.world().each<WaterComponent>([&](Entity e, WaterComponent& w) {
        WaterVolume v;
        v.entity = e;
        const math::Mat4 m = scene.world_matrix(e);
        v.center = {m[3].x, m[3].y, m[3].z};
        v.half_extent = {std::max(finite_or(w.size.x, 0), 0.0f) * 0.5f, std::max(finite_or(w.size.y, 0), 0.0f) * 0.5f};
        v.water = &w;
        if (w.waves == WaveModel::Spectrum) {
            v.ocean = water_ocean(scene, e, w, t);
            v.max_height = v.ocean->max_height;
        } else {
            v.waves = water_waves(w, t);
            for (const WaterWave& wave : v.waves) v.max_height += wave.amplitude;
        }
        out.push_back(v);
    });
    std::sort(out.begin(), out.end(), [](const WaterVolume& a, const WaterVolume& b) { return a.entity.index < b.entity.index; });
    // Forget spectra of water that was removed or switched back to Gerstner waves.
    if (auto* cache = scene.world().resource<OceanCache>()) {
        std::erase_if(cache->entries, [&](const auto& kv) {
            return std::none_of(out.begin(), out.end(), [&](const WaterVolume& v) { return v.ocean && v.entity.bits() == kv.first; });
        });
    }
    return out;
}

std::optional<WaterQuery> query_water(scene::Scene& scene, math::Vec3 p) {
    std::optional<WaterQuery> best;
    float best_dist = 0;
    for (const WaterVolume& v : water_volumes(scene)) {
        if (std::abs(p.x - v.center.x) > v.half_extent.x || std::abs(p.z - v.center.z) > v.half_extent.y) continue;
        const WaveSample s = sample_volume(v, {p.x, p.z});
        WaterQuery q;
        q.water = v.entity;
        q.surface = v.center.y + s.height;
        q.normal = s.normal;
        q.depth = q.surface - p.y;
        q.current = {v.water->current.x, 0, v.water->current.y};
        // Prefer the volume the point is inside (or the nearest surface above it).
        const float dist = q.depth >= 0 ? q.depth : 1e6f - q.depth;
        if (!best || dist < best_dist) {
            best = q;
            best_dist = dist;
        }
    }
    return best;
}

void register_water_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using W = WaterComponent;
        scene::Schema<W>("Water", "Environment")
            .field(&W::size, "size", {.min = 0.1f, .max = 100000, .speed = 0.5f, .tooltip = "Extent along X and Z in metres; the surface rests at the entity's height"})
            .field(&W::shallow_color, "shallow_color", {.tooltip = "Tint of light passing through clear water"})
            .field(&W::deep_color, "deep_color", {.tooltip = "Colour of deep water", .hint = "color_hdr"})
            .field(&W::clarity, "clarity", {.min = 0.05f, .max = 200, .speed = 0.05f, .tooltip = "Metres of water before the bottom fades into the deep colour"})
            .field(&W::wave_height, "wave_height", {.min = 0, .max = 30, .speed = 0.01f, .tooltip = "Crest-to-trough height of the primary wave (m)"})
            .field(&W::wavelength, "wavelength", {.min = 0.05f, .max = 1000, .speed = 0.1f, .tooltip = "Primary wavelength (m); three shorter waves are derived from it"})
            .field(&W::wave_direction, "wave_direction", {.min = -360, .max = 360, .speed = 1, .tooltip = "Degrees from +X towards +Z"})
            .field(&W::steepness, "steepness", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "0 = rolling sine waves, 1 = sharp crests"})
            .field(&W::wave_speed, "wave_speed", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Multiplier on physical deep-water wave speed"})
            .field(&W::foam, "foam", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Shoreline and crest foam"})
            .field(&W::refraction, "refraction", {.min = 0, .max = 0.2f, .speed = 0.001f, .tooltip = "Distortion of what lies below"})
            .field(&W::roughness, "roughness", {.min = 0.01f, .max = 1, .speed = 0.005f})
            .field(&W::resolution, "resolution", {.min = 16, .max = 512, .tooltip = "Rendered grid cells per side"})
            .field(&W::density, "density", {.min = 1, .max = 20000, .speed = 1, .tooltip = "kg/m^3; bodies lighter than this float"})
            .field(&W::linear_drag, "linear_drag", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&W::angular_drag, "angular_drag", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&W::damping, "damping", {.min = 0, .max = 20, .speed = 0.01f, .tooltip = "Settles bobbing (1/s while submerged)"})
            .field(&W::current, "current", {.min = -50, .max = 50, .speed = 0.05f, .tooltip = "Flow velocity on X/Z (m/s)"})
            .field(&W::visible, "visible")
            .enumeration(&W::waves, "waves", {"gerstner", "spectrum"})
            .field(&W::spectrum_tile, "spectrum_tile", {.min = 1, .max = 10000, .speed = 1, .tooltip = "Spectrum waves: metres before the sea repeats"})
            .field(&W::spectrum_resolution, "spectrum_resolution", {.min = 16, .max = 256, .tooltip = "Spectrum waves: waves per side (power of two; 64 is typical)"})
            .field(&W::spectrum_seed, "spectrum_seed", {.min = 0, .max = 1000000, .tooltip = "Spectrum waves: random seed of the sea state"})
            .commit();
    });
}

} // namespace victus::gameplay
