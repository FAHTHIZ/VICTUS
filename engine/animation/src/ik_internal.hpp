// SPDX-License-Identifier: Apache-2.0
// Shared between ik.cpp and foot_placement.cpp; not installed.
#pragma once
#include "victus/core/math.hpp"
#include "victus/scene/scene.hpp"

#include <vector>

namespace victus::animation::detail {

// Runtime (not serialized): the joint list and the pose IK last read and wrote, so procedural
// chains that nothing re-animates solve from their own pose instead of IK's previous output.
struct IkRuntime {
    std::vector<scene::Entity> joints;
    std::vector<math::Quat> input, output;
};

// Puts back the pose IK read last frame on joints still holding IK's output (nothing re-animated
// them), so code running before apply_ik sees the animated pose, as apply_ik itself will.
void restore_ik_input(scene::Scene& scene, scene::Entity end);
// Records `local` as IK's output for `joint` of the chain ending at `end` (a post-IK adjustment
// such as a foot tilt), so the next frame's restore still recognises it as IK-written.
void set_ik_output_rotation(scene::Scene& scene, scene::Entity end, scene::Entity joint, math::Quat local);

} // namespace victus::animation::detail
