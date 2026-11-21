// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/sim_lod.hpp"

#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace victus::gameplay {

namespace {

math::Vec3 world_position(const scene::WorldTransform& w) noexcept {
    const math::Vec4& t = w.matrix[3];
    return {t.x, t.y, t.z};
}

// Spreads due frames of neighbouring entity indices across the interval (Knuth multiplicative hash).
std::uint32_t stagger_of(scene::Entity e) noexcept { return (e.index * 2654435761u) >> 8; }

struct Source {
    math::Vec3 position;
    float scale{1.0f};
};

} // namespace

void register_simulation_lod_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using C = SimulationLodComponent;
        scene::Schema<C>("SimulationLod", "Gameplay")
            .field(&C::bias, "bias",
                   {.min = -10000, .max = 10000, .speed = 0.5f, .label = "Distance Bias",
                    .tooltip = "Metres added to the distance to the nearest source; negative keeps the entity detailed further away"})
            .field(&C::always_full, "always_full",
                   {.label = "Always Full Rate", .tooltip = "Update every frame regardless of distance (player, bosses)"})
            .commit();
        using S = SimulationLodSource;
        scene::Schema<S>("SimulationLodSource", "Gameplay")
            .field(&S::radius_scale, "radius_scale",
                   {.min = 0.01f, .max = 100, .speed = 0.01f, .label = "Radius Scale",
                    .tooltip = "Multiplies every simulation LOD distance around this source"})
            .field(&S::enabled, "enabled")
            .commit();
    });
}

SimulationLodSettings& simulation_lod_settings(scene::Scene& scene) {
    auto& w = scene.world();
    if (auto* s = w.resource<SimulationLodSettings>()) return *s;
    return w.set_resource<SimulationLodSettings>();
}

const SimulationLodStats* simulation_lod_stats(const scene::Scene& scene) noexcept {
    return const_cast<scene::Scene&>(scene).world().resource<SimulationLodStats>();
}

std::uint8_t simulation_lod_tier(const SimulationLodSettings& s, float distance, std::uint8_t current,
                                 bool placed) noexcept {
    if (!std::isfinite(distance)) return static_cast<std::uint8_t>(kSimulationLodTiers - 1);
    std::uint8_t raw = 0;
    while (raw < kSimulationLodTiers - 1 && distance > s.distances[raw]) ++raw;
    if (!placed) return raw;
    const float h = std::clamp(s.hysteresis, 0.0f, 0.9f);
    std::uint8_t t = std::min<std::uint8_t>(current, kSimulationLodTiers - 1);
    while (t < kSimulationLodTiers - 1 && distance > s.distances[t] * (1.0f + h)) ++t;
    while (t > 0 && distance < s.distances[t - 1u] * (1.0f - h)) --t;
    return t;
}

void update_simulation_lod(scene::Scene& scene, const FrameTime& t) {
    VICTUS_PROFILE_SCOPE("simlod.assign");
    auto& w = scene.world();
    if (w.count<SimulationLodComponent>() == 0) return;
    if (game_paused(scene)) return;
    const SimulationLodSettings& settings = simulation_lod_settings(scene);
    SimulationLodStats* stats = w.resource<SimulationLodStats>();
    if (!stats) stats = &w.set_resource<SimulationLodStats>();
    *stats = {};
    stats->frame = t.frame;

    std::vector<Source> sources;
    w.each<SimulationLodSource, scene::WorldTransform>([&](const SimulationLodSource& s, const scene::WorldTransform& wt) {
        if (s.enabled) sources.push_back({world_position(wt), std::max(s.radius_scale, 0.01f)});
    });
    if (sources.empty() && settings.camera_fallback)
        w.each<scene::CameraComponent, scene::WorldTransform>([&](const scene::CameraComponent& c, const scene::WorldTransform& wt) {
            if (c.primary) sources.push_back({world_position(wt), 1.0f});
        });
    stats->sources = static_cast<std::uint32_t>(sources.size());

    const float dt = std::isfinite(t.dt) && t.dt > 0 ? t.dt : 0.0f;
    const bool lod = settings.enabled && !sources.empty();
    w.each<SimulationLodComponent>([&](scene::Entity e, SimulationLodComponent& c) {
        ++stats->entities;
        c.pending_dt += dt;
        const scene::WorldTransform* wt = w.get<scene::WorldTransform>(e);
        std::uint8_t tier = 0;
        if (lod && !c.always_full && wt) {
            const math::Vec3 p = world_position(*wt);
            // Distance in units of each source's scale, so a scaled-up source widens every tier.
            float best = std::numeric_limits<float>::infinity();
            for (const Source& s : sources) best = std::min(best, math::length(p - s.position) / s.scale);
            c.distance = std::max(best + c.bias, 0.0f);
            tier = simulation_lod_tier(settings, c.distance, c.tier, c.placed);
        } else {
            c.distance = 0;
        }
        if (c.placed && tier != c.tier) ++stats->tier_changes;
        if (!c.placed) c.stagger = stagger_of(e);
        c.placed = true;
        c.tier = tier;
        ++stats->tiers[tier];
        const std::uint32_t interval = std::max<std::uint32_t>(settings.intervals[tier], 1);
        c.due = interval == 1 || (t.frame + c.stagger) % interval == 0;
        if (c.due) {
            c.due_dt = c.pending_dt;
            c.pending_dt = 0;
            ++stats->due;
        } else {
            c.due_dt = 0;
        }
    });
}

LodTick simulation_lod_tick(scene::Scene& scene, scene::Entity e, float frame_dt) noexcept {
    return simulation_lod_tick(scene.world().get<SimulationLodComponent>(e), frame_dt);
}

} // namespace victus::gameplay
