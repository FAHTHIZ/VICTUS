// SPDX-License-Identifier: Apache-2.0
// Skeletal/transform animation: keyframed clips (glTF 2.0 channel model: translation, rotation,
// scale with step, linear or cubic-spline interpolation), sampling and blending into node poses,
// and the Animator component/system that plays clips on an entity hierarchy with cross-fades.
//
// The module is headless (no GPU, no asset pipeline): clips arrive through a provider callback,
// so servers and `victus simulate` animate exactly like the player. Animators write local
// Transforms of the bound entities; GPU skinning (runtime) then reads their world transforms.
#pragma once
#include "victus/animation/sprite.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::animation {

enum class Path : std::uint8_t { Translation, Rotation, Scale };
enum class Interpolation : std::uint8_t { Step, Linear, CubicSpline };

// One animated property of one node. `values` holds `components()` floats per key (3 for
// translation/scale, 4 for rotation xyzw); cubic-spline keys store in-tangent, value,
// out-tangent (3 * components floats per key), as in glTF.
struct Channel {
    std::uint32_t node{0};
    Path path{Path::Translation};
    Interpolation interpolation{Interpolation::Linear};
    std::vector<float> times; // seconds, non-decreasing
    std::vector<float> values;
    [[nodiscard]] std::uint32_t components() const noexcept { return path == Path::Rotation ? 4u : 3u; }
};

struct Clip {
    std::string name;
    float duration{0}; // seconds (last key time)
    std::vector<Channel> channels;
};

struct NodePose {
    math::Vec3 translation;
    math::Quat rotation;
    math::Vec3 scale{1, 1, 1};
};

// Rest pose and names of the animated node graph (from the model) plus its clips.
struct AnimationSet {
    struct Node {
        std::string name;
        std::int32_t parent{-1};
        NodePose rest;
    };
    std::vector<Node> nodes;
    std::vector<Clip> clips;
    [[nodiscard]] std::int32_t find_clip(std::string_view name) const noexcept;
};

// Validates node indices, key ordering, finite values and value counts.
[[nodiscard]] Status validate(const Clip& clip, std::size_t node_count);

[[nodiscard]] math::Vec3 sample_vec3(const Channel& channel, float time) noexcept;
[[nodiscard]] math::Quat sample_quat(const Channel& channel, float time) noexcept;
// Writes every channel of `clip` at `time` (clamped to the clip) into `pose` (indexed by node).
void sample(const Clip& clip, float time, std::span<NodePose> pose) noexcept;
// Linear blend of two poses (shortest-path normalized quaternion lerp), weight 0 = a, 1 = b.
[[nodiscard]] NodePose blend(const NodePose& a, const NodePose& b, float weight) noexcept;
// Wraps (loop) or clamps a playback time into [0, duration].
[[nodiscard]] float wrap_time(float time, float duration, bool loop) noexcept;

// Binds nodes to entities under `root` by name: root nodes (parent < 0) are looked up among
// root's children, other nodes among their bound parent's children, falling back to a
// breadth-first search of root's whole subtree. Unmatched nodes are null. Nodes must be ordered
// parent-before-child (as in cooked models).
[[nodiscard]] std::vector<scene::Entity> bind_nodes(scene::Scene& scene, scene::Entity root,
                                                    std::span<const AnimationSet::Node> nodes);

// ---- components ------------------------------------------------------------------------------

// Plays clips of a model's AnimationSet on this entity's descendants (bound by node name).
struct AnimatorComponent {
    Uuid model;              // model asset providing the clips and rest pose
    std::string clip;        // clip to play; changing it cross-fades from the current clip
    bool playing{true};
    bool loop{true};
    float speed{1.0f};       // playback rate (negative plays backwards)
    float crossfade{0.2f};   // seconds used when `clip` changes
    float time{0.0f};        // current playback time in seconds
};

// Marks a MeshRenderer as GPU-skinned by `skin` of its model, with joints bound by name under
// the `skeleton` entity (the model's instantiated root).
struct SkinnedMeshComponent {
    Uuid skeleton;
    std::uint32_t skin{0};
};

// Registers Animator, SkinnedMesh and SpriteAnimator (reflection: serialization, inspector,
// scripts). Idempotent.
void register_components();

// ---- system ----------------------------------------------------------------------------------

// Returns the AnimationSet of a model asset, or null when it does not exist or has no nodes.
using SetProvider = std::function<std::shared_ptr<const AnimationSet>(const Uuid& model)>;

struct AnimatorStatus {
    bool valid{false};       // entity has an Animator whose model resolved
    std::string clip;
    float time{0}, duration{0};
    bool playing{false}, looping{false}, finished{false};
    float blend{1};          // cross-fade progress (1 = settled on the current clip)
    std::uint32_t bound_nodes{0}, animated_nodes{0};
};

struct AnimationStats {
    std::uint32_t animators{0}, channels_sampled{0}, nodes_written{0}, missing_models{0};
};

struct AnimationSettings {
    SetProvider provider;
    SheetProvider sheets; // sprite sheets for SpriteAnimator (assets::sprite_sheet_provider)
    jobs::JobSystem* jobs{nullptr}; // evaluates animators in parallel when set
    std::uint32_t parallel_threshold{32};
};

class AnimationSystem {
public:
    explicit AnimationSystem(AnimationSettings settings = {});
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    // Registers a set directly (tests, procedural content); takes precedence over the provider.
    void add_set(const Uuid& model, std::shared_ptr<const AnimationSet> set);
    // Forgets cached sets (e.g. after re-import) so the provider is asked again.
    void invalidate(const Uuid& model);
    void clear_cache();

    // Advances and applies every Animator by dt seconds.
    void update(scene::Scene& scene, float dt);
    // Starts `clip` on the entity's Animator. fade < 0 uses the component's crossfade.
    // Returns false when the entity has no Animator or the clip does not exist.
    bool play(scene::Scene& scene, scene::Entity e, std::string_view clip, float fade = -1.0f, bool restart = true);
    [[nodiscard]] AnimatorStatus status(scene::Scene& scene, scene::Entity e);
    [[nodiscard]] std::vector<std::string> clips(scene::Scene& scene, scene::Entity e);
    [[nodiscard]] std::shared_ptr<const AnimationSet> set(const Uuid& model);
    [[nodiscard]] const AnimationStats& stats() const noexcept { return stats_; }

    // ---- sprite animation ----
    // Registers a sheet directly (tests, procedural content); takes precedence over the provider.
    void add_sheet(const Uuid& sheet, std::shared_ptr<const SpriteSheet> data);
    // Forgets cached sheets (after a .vsheet changed) so the provider is asked again.
    void clear_sheets();
    [[nodiscard]] std::shared_ptr<const SpriteSheet> sheet(const Uuid& sheet);
    // Advances every SpriteAnimator by dt and writes its Sprite's texture and uv. update() calls
    // it; editors call it with dt 0 to preview the authored frame.
    void update_sprites(scene::Scene& scene, float dt);
    // Starts `clip` (restart: from its first frame even when already playing). False when the
    // entity has no SpriteAnimator, its sheet is missing or has no such clip.
    bool play_sprite(scene::Scene& scene, scene::Entity e, std::string_view clip, bool restart = true);
    [[nodiscard]] SpriteAnimatorStatus sprite_status(scene::Scene& scene, scene::Entity e);
    [[nodiscard]] std::vector<std::string> sprite_clips(scene::Scene& scene, scene::Entity e);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AnimationStats stats_;
};

// Installs "animation.update" (Phase::Update; register after scripts so script clip changes
// apply the same frame). Also binds the system to the scene for scripts (system_of).
void add_animation_systems(gameplay::Simulation& sim, AnimationSystem& system);
[[nodiscard]] AnimationSystem* system_of(scene::Scene& scene);
void bind(scene::Scene& scene, AnimationSystem* system);

// Debug skeleton: one segment from each bound node to its bound parent for every Animator.
void skeleton_lines(scene::Scene& scene, const std::function<void(math::Vec3, math::Vec3)>& emit);

} // namespace victus::animation
