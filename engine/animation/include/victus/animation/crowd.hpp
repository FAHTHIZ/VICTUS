// SPDX-License-Identifier: Apache-2.0
// Crowd animation (ADR-0200, GAP-10): clips baked once into skin-matrix palettes, so thousands of
// characters animate without joint entities, per-agent pose evaluation or per-frame joint uploads.
//
// A bake samples every clip of a skinned model at a fixed rate and stores, per frame, the skin
// matrix of every joint (joint world * inverse bind, in the model's root space): exactly the
// palette GPU skinning reads. A CrowdAgent then only advances a time; the renderer draws it from
// the baked palette at frame `first_frame + frame index`. Trade-offs against Animator: no
// cross-fades, blend spaces, layers, IK or root motion, and poses snap to the bake rate.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::animation {

struct BakedClip {
    std::string name;
    float duration{0};           // seconds
    std::uint32_t first_frame{0}; // first frame in CrowdBake::matrices (in frames, not matrices)
    std::uint32_t frames{1};      // sampled frames (>= 1); frame k is at time min(k / fps, duration)
    math::Aabb bounds;            // model-space bounds over every frame (when joint bounds were given)
};

struct CrowdBake {
    float fps{30};
    std::uint32_t joints{0};
    std::vector<BakedClip> clips;
    std::vector<math::Mat4> matrices; // total frames * joints
    math::Aabb rest_bounds;           // bind-pose bounds (agents without a resolvable clip)

    [[nodiscard]] std::int32_t find_clip(std::string_view name) const noexcept;
    [[nodiscard]] std::uint32_t total_frames() const noexcept { return joints ? static_cast<std::uint32_t>(matrices.size() / joints) : 0; }
    // Absolute frame of `clip` at `time` (seconds): wrapped when `loop`, else clamped to the end.
    // Nearest sample, so a looping clip whose last key repeats its first never shows a hitch.
    [[nodiscard]] std::uint32_t frame_at(std::uint32_t clip, float time, bool loop) const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept { return matrices.size() * sizeof(math::Mat4); }
};

struct CrowdBakeInput {
    // Model node graph: parent before child (as in cooked models). Morph weight nodes are ignored.
    std::span<const AnimationSet::Node> nodes;
    std::span<const Clip> clips; // channel node indices refer to `nodes`
    std::span<const std::uint32_t> joints;     // skin joints (node indices)
    std::span<const math::Mat4> inverse_bind;  // one per joint
    // Optional per-joint bind-space bounds of the vertices each joint influences (union over the
    // skin's meshes); used for the per-clip culling bounds.
    std::span<const math::Aabb> joint_bounds;
    float fps{30};
    std::uint32_t max_frames_per_clip{1800}; // a minute at 30 fps; longer clips are sampled more sparsely
    std::size_t max_bytes{64u << 20};         // refuse bakes above this (InvalidArgument)
};

// Bakes every clip (in order). Fails on an empty skin, mismatched inverse binds, joint indices
// outside `nodes`, a non-positive fps or a bake larger than `max_bytes`.
[[nodiscard]] Result<CrowdBake> bake_crowd(const CrowdBakeInput& input);

// One member of a crowd: a whole skinned model drawn from its baked clips (ADR-0200). No skeleton
// entities, no MeshRenderer: the entity's transform places the model root. Every mesh of the
// model bound to `skin` is drawn with the model's materials, multiplied by `tint`.
struct CrowdAgentComponent {
    Uuid model;
    std::uint32_t skin{0};
    std::string clip;  // clip name; empty or unknown shows the bind pose
    float time{0};     // seconds into the clip
    float speed{1};
    bool playing{true};
    bool loop{true};
    math::Color tint{1, 1, 1, 1};
    bool cast_shadows{true};
};

struct CrowdStats {
    std::uint32_t agents{0}, advanced{0};
};

// Registers CrowdAgent (reflection: serialization, inspector, scripts). Idempotent; called by
// register_components().
void register_crowd_components();

// Advances the time of every playing CrowdAgent by dt * speed (AnimationSystem::update calls it).
CrowdStats update_crowd(ecs::World& world, float dt);

} // namespace victus::animation
