// SPDX-License-Identifier: Apache-2.0
// Procedural reference rigs (ADR-0207) for deformation tests, benchmarks, the Studio and the
// CLI: skeletons that are not humanoids, built in code so every vertex weight and every bind
// transform is known exactly.
//
// - Quadruped ("Hound"): 38 joints. Spine, neck and head with jaw and ears, four legs of five
//   joints (shoulder/thigh, upper, lower/hock, foot, toe), a seven-joint tail and a jiggle belly.
//   The skin is closed tubes around the body, tail and leg chains with smooth two-bone blends at
//   every joint, so LBS/DQS differences and volume loss are measurable. Clips: Walk (1 s cycle,
//   diagonal gait, tail wag, head bob, jaw), Sit (bent hind legs, curled tail) and Shake.
// - Mech ("Loader"): 13 joints. A turret, a two-piece hydraulic arm with a piston, a wrist that
//   rolls 360 degrees, a two-finger claw and four wheels; every part is a box weighted 100% to one joint,
//   the way hard-surface robots are rigged, so any pose must leave every part rigid. Clips: Work
//   and Spin (continuous wrist roll).
// Both are Y-up, metres, facing +Z, with a root helper node "Rig" and sockets.
#pragma once

#include "victus/assets/cooked.hpp"
#include "victus/assets/skeleton.hpp"

#include <string_view>

namespace victus::assets {

enum class ReferenceRig : std::uint8_t { Quadruped, Mech };

[[nodiscard]] CookedModel make_reference_rig(ReferenceRig rig);
// "quadruped" / "hound", "mech" / "loader"; false when unknown.
[[nodiscard]] bool parse_reference_rig(std::string_view name, ReferenceRig& out) noexcept;

// The "complex humanoid" reference: 19 bones added to a VICTUS humanoid (Orion, Nova or any
// skeleton with Head, Jaw, Toe.L/R, LowerArm.L/R, Shoulder.R and Hips) through the safe
// extension path: brows, upper lids, lip corners and upper lip, a tongue, second toe segments,
// forearm roll joints taking half of each forearm's weights toward the wrist, a two-joint
// secondary antenna, and non-deforming IK targets, elbow poles and a shoulder equipment socket.
[[nodiscard]] std::vector<BoneEdit> complex_humanoid_extension();

} // namespace victus::assets
