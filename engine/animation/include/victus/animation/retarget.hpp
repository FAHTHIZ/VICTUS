// SPDX-License-Identifier: Apache-2.0
// Animation retargeting (ADR-0075): plays clips authored for one skeleton on another whose bones
// have matching names, correcting for different rest orientations and proportions.
//
// Bones map by name (exact, then normalized: namespace prefixes such as "mixamorig:" or
// "Armature|" dropped, case and separators ignored), plus an optional explicit map. Each
// retargeted clip is baked once: at every source key time the source pose is evaluated, each
// mapped bone's world-space rotation change from its rest pose is applied to the target bone's
// world rest rotation, and the result is converted back to local rotations. Only the root bone
// (the highest mapped bone the clip translates, usually the hips) keeps translation, scaled by
// the ratio of the two rigs' root heights; other bones keep the target's rest translations so
// its proportions are preserved.
#pragma once
#include "victus/animation/animation.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace victus::animation {

struct RetargetOptions {
    // Explicit target-bone -> source-bone names, checked before name matching.
    std::vector<std::pair<std::string, std::string>> bone_map;
    bool root_translation{true}; // keep the root bone's travel (scaled by root height)
    float max_sample_rate{60.0f}; // keys per second cap when a clip has very many key times
};

// "mixamorig:LeftUpLeg" -> "leftupleg", "Armature|Left_Arm" -> "leftarm".
[[nodiscard]] std::string normalize_bone_name(std::string_view name);

// target node index -> source node index (-1 = unmapped).
[[nodiscard]] std::vector<std::int32_t> map_bones(const AnimationSet& source, const AnimationSet& target,
                                                  const RetargetOptions& options = {});

// Bakes `clip` (of `source`) onto `target`. `map` comes from map_bones.
[[nodiscard]] Clip retarget_clip(const AnimationSet& source, const Clip& clip, const AnimationSet& target,
                                 std::span<const std::int32_t> map, const RetargetOptions& options = {});

struct RetargetReport {
    std::uint32_t mapped_bones{0}, target_bones{0}, clips{0}, skipped_clips{0};
    std::vector<std::string> unmapped; // target bones without a source bone (first 32)
};

// Target nodes and clips plus every source clip whose name the target lacks, retargeted.
// Returns null when no bone maps.
[[nodiscard]] std::shared_ptr<AnimationSet> retarget_set(const AnimationSet& source, const AnimationSet& target,
                                                         const RetargetOptions& options = {},
                                                         RetargetReport* report = nullptr);

} // namespace victus::animation
