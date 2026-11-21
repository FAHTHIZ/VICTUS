// SPDX-License-Identifier: Apache-2.0
// Foot placement and foot locking (ADR-0163): plants animated feet on uneven ground and keeps a
// planted foot from sliding, on top of the two-bone IK of ik.hpp.
//
// Setup: `FootPlacement` on the character root, `FootContact` on every foot joint (two for a
// biped, four for a quadruped), each foot also carrying a TwoBone `IKConstraint` with no target
// entity. Every frame, after the animated pose and before IK, each foot probes the ground straight
// below its animated position. The foot keeps the height the animation gives it above the root
// (the root stands on the ground), measured from the ground it is over instead; the pelvis drops
// by the largest downward correction so the lower foot can reach; the IK target is set to the
// corrected position. A foot moving slower than `lock_speed` near the ground is locked to where
// it planted until the animation lifts it or it drifts `release_distance` away. After IK the foot
// is tilted to the ground normal. Corrections are smoothed so stairs and kerbs do not pop.
//
// The ground comes from a `GroundProbe` (AnimationSettings::ground); runtime::physics_ground_probe
// adapts a PhysicsWorld. Without a probe (edit-mode previews) the feet keep the animated pose.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/math.hpp"
#include "victus/ecs/world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace victus::scene {
class Scene;
}

namespace victus::animation {

struct GroundHit {
    math::Vec3 point;
    math::Vec3 normal{0, 1, 0};
};
// Casts from `origin` along `direction` (unit) up to `max_distance`; `ignore` is the character
// root, whose own bodies (capsule, ragdoll bones) must not count as ground.
using GroundProbe = std::function<std::optional<GroundHit>(math::Vec3 origin, math::Vec3 direction, float max_distance,
                                                          std::uint32_t mask, ecs::Entity ignore)>;

// On the character root (the entity standing on the ground, usually the one with the
// CharacterController).
struct FootPlacementComponent {
    bool enabled{true};
    Uuid pelvis;                  // bone lowered so the lower foot reaches; nil = never move the body
    float probe_up{0.5f};         // ray starts this far above the animated foot (steps it can climb)
    float probe_down{0.6f};       // ...and reaches this far below it (drops it can reach)
    float max_pelvis_drop{0.4f};  // metres
    float smoothing{14.0f};       // 1/s: how fast corrections follow the ground (0 = instant)
    bool align_to_ground{true};   // tilt feet to the ground normal
    float max_slope{40.0f};       // degrees of tilt at most
    bool lock_feet{true};         // stop planted feet from sliding
    float lock_speed{0.3f};       // m/s: a slower foot near the ground plants
    float lock_height{0.15f};     // a foot this close above its rest height counts as on the ground
    float release_distance{0.25f}; // a locked foot further than this from the animated one lets go
    std::uint32_t mask{0xFFFFFFFFu}; // physics layers that count as ground
};

// On each foot joint; the foot also needs a TwoBone IKConstraint without a target entity.
struct FootContactComponent {
    bool enabled{true};
    bool lock{true}; // may plant (off for a dragging foot or a tail tip)
};

struct FootPlacementStats {
    std::uint32_t characters{0}; // enabled FootPlacement roots evaluated
    std::uint32_t feet{0};       // feet evaluated
    std::uint32_t grounded{0};   // ...that found ground within the probe
    std::uint32_t locked{0};     // ...planted by foot locking this frame
    std::uint32_t invalid{0};    // FootContact without a usable TwoBone IKConstraint or root
};

// Registers FootPlacement and FootContact (reflection). Idempotent; also called by
// animation::register_components().
void register_foot_placement_components();

// Before IK: probes the ground, moves the pelvis and writes every foot's IK target_position.
// AnimationSystem calls both halves around apply_ik. An empty probe finds no ground: the IK
// targets follow the animated feet, so IK changes nothing.
FootPlacementStats prepare_foot_placement(scene::Scene& scene, const GroundProbe& probe, float dt);
// After IK: tilts the feet to the ground normals found by prepare_foot_placement.
void finish_foot_placement(scene::Scene& scene);

// One-step setup for a humanoid under `root`: the hips/pelvis bone becomes the pelvis and every
// joint named like a foot or ankle (not a toe) with two ancestors below `root` gets a FootContact
// and a TwoBone IKConstraint (an existing one is kept). Returns the feet configured; 0 adds nothing.
std::size_t make_humanoid_foot_placement(scene::Scene& scene, ecs::Entity root);
// Removes FootPlacement from `root`, and FootContact plus the IKConstraints that
// make_humanoid_foot_placement added (TwoBone, no target entity) from its feet. Returns the
// components removed.
std::size_t remove_foot_placement(scene::Scene& scene, ecs::Entity root);

// Debug lines: the probe ray, the ground point and normal, and a box around a locked foot.
void foot_placement_lines(scene::Scene& scene, const std::function<void(math::Vec3, math::Vec3)>& emit);

} // namespace victus::animation
