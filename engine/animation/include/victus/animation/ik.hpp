// SPDX-License-Identifier: Apache-2.0
// Inverse kinematics (ADR-0073): analytic two-bone, FABRIK chains and look-at/aim constraints
// that post-process the animated pose. Solvers work on world-space joint positions; the result
// is written back as local rotations of the bound entities, blended with the animated pose by
// `weight`. Bone lengths and translations are never changed, so skinning stays rigid.
//
// The `IKConstraint` component sits on the end of a chain (a hand, a foot, a head) and names
// how many ancestors the solver may rotate. Targets are entities or a world position, so
// scripts drive foot placement by raycasting and writing `target_position`.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/hash.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace victus::scene {
class Scene;
}

namespace victus::animation {

// ---- pure solvers (world space, no scene) ---------------------------------------------------

// Shortest rotation taking direction `from` onto `to` (both non-zero). Opposite vectors turn
// half a revolution about an axis perpendicular to `from`.
[[nodiscard]] math::Quat rotation_between(math::Vec3 from, math::Vec3 to) noexcept;

struct TwoBoneResult {
    math::Vec3 mid, tip; // solved joint positions (root does not move)
    bool reached{true};  // false when the target was out of reach or too close (clamped)
};
// Analytic two-bone solve by the law of cosines. The bend plane contains root, target and
// `bend_hint` (the pole position, or the current middle joint to keep the animated knee/elbow
// direction). `max_extension` (0..1] keeps the chain from locking straight (1 = may straighten).
[[nodiscard]] TwoBoneResult solve_two_bone(math::Vec3 root, math::Vec3 mid, math::Vec3 tip, math::Vec3 target,
                                           math::Vec3 bend_hint, float max_extension = 1.0f) noexcept;

struct FabrikResult {
    std::uint32_t iterations{0};
    float error{0};     // distance from the solved end to the target
    bool reached{false}; // error <= tolerance
};
// FABRIK (Aristidou & Lasenby 2011) on `points` (root first, end last) in place. The root stays
// fixed and bone lengths are kept exactly. An out-of-reach target straightens the chain toward it.
FabrikResult solve_fabrik(std::span<math::Vec3> points, math::Vec3 target, std::uint32_t max_iterations = 10,
                          float tolerance = 1e-3f) noexcept;

// World rotations that carry a chain from `before` to `after` joint positions (same count):
// joint i turns by the shortest rotation taking its old bone direction (as already rotated by
// its ancestors) onto the new one. `rotations` holds world rotations and is updated in place;
// the last joint (the end) inherits its parent's turn.
void rotations_from_positions(std::span<const math::Vec3> before, std::span<const math::Vec3> after,
                              std::span<math::Quat> rotations) noexcept;

// Rotation about `pivot` that turns `aim` (a world direction) toward `target`, with the angle
// clamped to `max_angle` radians. Identity when the target coincides with the pivot.
[[nodiscard]] math::Quat aim_rotation(math::Vec3 pivot, math::Vec3 aim, math::Vec3 target, float max_angle) noexcept;

// ---- component -------------------------------------------------------------------------------

enum class IkSolver : std::uint8_t { TwoBone, Chain, LookAt };

// On the end joint of a chain. TwoBone moves the parent and grandparent (thigh-shin-foot,
// upper arm-forearm-hand); Chain moves `chain_length` ancestors with FABRIK (tails, tentacles,
// spines); LookAt turns this joint and `chain_length` ancestors so `aim_axis` points at the
// target, sharing the turn evenly (head + neck + chest).
struct IkConstraintComponent {
    IkSolver solver{IkSolver::TwoBone};
    bool enabled{true};
    float weight{1.0f};            // 0 = animated pose, 1 = fully solved
    Uuid target;                   // entity to reach or look at; nil = target_position
    math::Vec3 target_position{};  // world-space target used when `target` is nil
    Uuid pole;                     // TwoBone: entity the middle joint bends toward; nil = keep the animated bend
    bool match_rotation{false};    // TwoBone/Chain: the end joint takes the target entity's world rotation
    std::uint32_t chain_length{2}; // joints above this one the solver may rotate (TwoBone always uses 2)
    std::uint32_t iterations{10};  // Chain: FABRIK iteration cap
    float tolerance{0.001f};       // Chain: stop when the end is this close (metres)
    float max_extension{0.999f};   // TwoBone: fraction of full reach (avoids a locked, popping knee)
    math::Vec3 aim_axis{0, 0, 1};  // LookAt: local axis of this joint that should point at the target
    float max_angle{90.0f};        // LookAt: largest turn away from the animated direction, degrees
};

struct IkStats {
    std::uint32_t constraints{0}; // enabled constraints evaluated
    std::uint32_t solved{0};      // ...that changed the pose
    std::uint32_t unreachable{0}; // TwoBone/Chain targets out of reach (clamped)
    std::uint32_t invalid{0};     // missing target, chain longer than the hierarchy, zero-length bones
};

// Registers IKConstraint (reflection: serialization, inspector, scripts). Idempotent; also
// called by animation::register_components().
void register_ik_components();

// Solves every enabled IKConstraint in the scene, parents' chains before children's, and writes
// local rotations. Call after the animated pose is written (AnimationSystem::update does).
// A chain whose joints nothing re-animates (procedural rigs) solves from the pose it had before
// IK first touched it, so weights below 1 do not drift toward the solution frame after frame.
IkStats apply_ik(scene::Scene& scene);

// Debug lines for every enabled constraint: chain bones and a cross at the target.
void ik_lines(scene::Scene& scene, const std::function<void(math::Vec3, math::Vec3)>& emit);

} // namespace victus::animation
