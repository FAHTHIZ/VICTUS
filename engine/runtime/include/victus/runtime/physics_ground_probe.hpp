// SPDX-License-Identifier: Apache-2.0
// Adapts a PhysicsWorld to the animation ground probe used by FootPlacement (ADR-0163).
// Animation and physics are peers (layer 4), so the bridge lives here.
#pragma once
#include "victus/animation/foot_placement.hpp"

namespace victus::physics {
class PhysicsWorld;
}

namespace victus::runtime {

// Raycasts against `world` (which must outlive the probe), ignoring the character root's own
// bodies (its CharacterController capsule, rigid body and ragdoll bones) and triggers.
[[nodiscard]] animation::GroundProbe physics_ground_probe(const physics::PhysicsWorld& world);

} // namespace victus::runtime
