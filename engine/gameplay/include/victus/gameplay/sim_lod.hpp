// SPDX-License-Identifier: Apache-2.0
// Simulation level of detail (ADR-0165, GAP-09). Entities with a SimulationLod component are put
// into one of four tiers by their distance to the nearest significance source (entities with
// SimulationLodSource, otherwise the primary camera). A tier has a tick interval: tier 0 updates
// every frame, farther tiers every 2nd, 4th, 8th frame by default. Systems that honour LOD (AI
// behaviour trees, animators, built-in movers, scripts that ask) skip an entity on frames when it
// is not due and give it the summed delta of the skipped frames when it is, so off-screen agents
// keep living at a lower rate and their total simulated time stays exact. Due frames are
// staggered by entity so a tier's work is spread evenly over its interval.
//
// Entities without the component are always due with the frame delta, so nothing changes for
// scenes that do not opt in.
#pragma once
#include "victus/gameplay/gameplay.hpp"

#include <array>
#include <cstdint>

namespace victus::gameplay {

inline constexpr int kSimulationLodTiers = 4;

// Opt-in per entity. Serialized as "SimulationLod".
struct SimulationLodComponent {
    float bias{0.0f};       // metres added to the measured distance (negative = more important)
    bool always_full{false}; // stays in tier 0 (the player, bosses, quest givers)
    // Runtime state (not serialized).
    std::uint8_t tier{0};
    bool due{true};
    float due_dt{0.0f};       // delta to simulate this frame when due
    float pending_dt{0.0f};   // delta accumulated since the last due frame
    float distance{0.0f};     // distance used for the tier this frame
    std::uint32_t stagger{0}; // per-entity phase within the tier interval
    bool placed{false};       // tier assigned at least once (no hysteresis on the first frame)
};

// A point of interest that keeps nearby agents at full rate (player, camera rig, a cinematic
// focus). Serialized as "SimulationLodSource". `radius_scale` multiplies every tier distance.
struct SimulationLodSource {
    float radius_scale{1.0f};
    bool enabled{true};
};

// Scene resource (created on demand with these defaults).
struct SimulationLodSettings {
    bool enabled{true};
    // Upper distance (metres) of tiers 0, 1 and 2; beyond the last is tier 3.
    std::array<float, kSimulationLodTiers - 1> distances{30.0f, 80.0f, 200.0f};
    // Frames between updates per tier (each at least 1).
    std::array<std::uint32_t, kSimulationLodTiers> intervals{1, 2, 4, 8};
    // Fraction of a boundary an entity must cross beyond it before it changes tier.
    float hysteresis{0.1f};
    // Use primary cameras as sources when no SimulationLodSource is enabled.
    bool camera_fallback{true};
};

// Scene resource written by update_simulation_lod.
struct SimulationLodStats {
    std::uint32_t entities{0};
    std::uint32_t sources{0};
    std::uint32_t due{0};
    std::uint32_t tier_changes{0};
    std::array<std::uint32_t, kSimulationLodTiers> tiers{};
    std::uint64_t frame{0};
};

// What a LOD-aware system should do with one entity this frame.
struct LodTick {
    bool due{true};
    float dt{0.0f};
};

void register_simulation_lod_components();
// The scene's settings (created with defaults if missing).
SimulationLodSettings& simulation_lod_settings(scene::Scene& scene);
[[nodiscard]] const SimulationLodStats* simulation_lod_stats(const scene::Scene& scene) noexcept;

// Assigns tiers and due flags for this frame. Runs as the "simlod.assign" PreUpdate system
// (added by add_builtin_systems); uses world transforms from the previous frame. Does nothing
// while the game is paused.
void update_simulation_lod(scene::Scene& scene, const FrameTime& t);

// For LOD-aware systems: {true, frame_dt} for entities without the component (or with LOD
// disabled); otherwise the entity's due flag and accumulated delta.
[[nodiscard]] LodTick simulation_lod_tick(scene::Scene& scene, scene::Entity e, float frame_dt) noexcept;
[[nodiscard]] inline LodTick simulation_lod_tick(const SimulationLodComponent* c, float frame_dt) noexcept {
    if (!c) return {true, frame_dt};
    return {c->due, c->due ? c->due_dt : 0.0f};
}

// Tier for `distance` given the current tier (hysteresis applies once `placed`). Exposed for tests.
[[nodiscard]] std::uint8_t simulation_lod_tier(const SimulationLodSettings& s, float distance, std::uint8_t current,
                                               bool placed) noexcept;

} // namespace victus::gameplay
