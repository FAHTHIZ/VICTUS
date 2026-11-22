// SPDX-License-Identifier: Apache-2.0
// Skeletal/transform animation: keyframed clips (glTF 2.0 channel model: translation, rotation,
// scale with step, linear or cubic-spline interpolation), sampling and blending into node poses,
// and the Animator component/system that plays clips on an entity hierarchy with cross-fades.
//
// The module is headless (no GPU, no asset pipeline): clips arrive through a provider callback,
// so servers and `victus simulate` animate exactly like the player. Animators write local
// Transforms of the bound entities; GPU skinning (runtime) then reads their world transforms.
#pragma once
#include "victus/animation/controller.hpp"
#include "victus/animation/foot_placement.hpp"
#include "victus/animation/ik.hpp"
#include "victus/animation/sprite.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::animation {

// Weights: glTF morph target weights of the node's mesh (`Channel::width` per key). Sets expand
// them into per-target weight nodes (expand_morph_weights), so poses only carry transforms.
enum class Path : std::uint8_t { Translation, Rotation, Scale, Weights };
enum class Interpolation : std::uint8_t { Step, Linear, CubicSpline };

// One animated property of one node. `values` holds `components()` floats per key (3 for
// translation/scale, 4 for rotation xyzw, `width` morph weights); cubic-spline keys store
// in-tangent, value, out-tangent (3 * components floats per key), as in glTF.
struct Channel {
    std::uint32_t node{0};
    Path path{Path::Translation};
    Interpolation interpolation{Interpolation::Linear};
    std::vector<float> times; // seconds, non-decreasing
    std::vector<float> values;
    std::uint32_t width{0}; // Path::Weights: morph targets per key
    [[nodiscard]] std::uint32_t components() const noexcept {
        return path == Path::Rotation ? 4u : path == Path::Weights ? width : 3u;
    }
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
        // Weight node (expand_morph_weights): carries morph target `morph_index` of node
        // `morph_node` (also its parent) in translation.x. Never bound to an entity.
        std::int32_t morph_node{-1};
        std::uint32_t morph_index{0};
        [[nodiscard]] bool is_morph() const noexcept { return morph_node >= 0; }
    };
    // Morph target names and default weights of one model mesh (empty for meshes without).
    struct MeshMorphs {
        std::vector<std::string> names;
        std::vector<float> defaults;
    };
    std::vector<Node> nodes;
    std::vector<Clip> clips;
    std::vector<MeshMorphs> mesh_morphs; // by model mesh index
    [[nodiscard]] std::int32_t find_clip(std::string_view name) const noexcept;
};

// A node whose mesh has morph targets (names and default weights, one per target).
struct MorphNode {
    std::uint32_t node{0};
    std::vector<std::string> names;
    std::vector<float> defaults;
};
// Appends one weight node per target of each MorphNode (named "<node>/<target>", rest weight in
// translation.x) and rewrites Path::Weights channels into translation channels of those nodes, so
// cross-fades, blend spaces, layers and masks (through the parent) blend weights like transforms.
// Weights channels of nodes without targets, or wider than the node's target count, are dropped.
void expand_morph_weights(AnimationSet& set, std::span<const MorphNode> morphs);

// Validates node indices, key ordering, finite values and value counts.
[[nodiscard]] Status validate(const Clip& clip, std::size_t node_count);

// Keyframe reduction (as Unity's import option): removes keys that interpolation between the kept
// neighbours reproduces within the given errors. The first and last keys always stay, so clip
// duration is unchanged. Linear channels drop keys within tolerance; Step channels drop keys equal
// to the previous one; cubic-spline channels are left alone.
struct KeyframeReduction {
    float rotation_degrees{0.05f}; // angle between the original and the interpolated rotation
    // Translation, scale and morph weight error: this fraction of the channel's largest absolute
    // value (at least `min_error`), per component.
    float relative{0.001f};
    float min_error{1e-5f};
};
// Returns the number of keys removed.
std::size_t reduce_keyframes(Channel& channel, const KeyframeReduction& settings = {});
std::size_t reduce_keyframes(Clip& clip, const KeyframeReduction& settings = {});

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
// With a `controller` (.vanim state machine, controller.hpp) the controller picks and blends the
// clips; `clip` then mirrors the current state's name and writing it enters that state.
struct AnimatorComponent {
    Uuid model;              // model asset providing the clips and rest pose
    // Optional model whose clips are retargeted onto `model` by bone name (retarget.hpp,
    // ADR-0075); `model`'s own clips win on name clashes. Nil: only `model`'s clips.
    Uuid clip_source;
    std::string clip;        // clip to play; changing it cross-fades from the current clip
    std::string controller;  // optional .vanim asset path (relative to the assets folder)
    bool playing{true};
    bool loop{true};
    float speed{1.0f};       // playback rate (negative plays backwards)
    float crossfade{0.2f};   // seconds used when `clip` changes
    float time{0.0f};        // current playback time in seconds
    // Controller states with "root_motion" move this entity by the root node's travel. Off: the
    // motion is only reported (AnimationSystem::root_motion, Luau entity:root_motion()) for a
    // script to apply, e.g. through a CharacterController.
    bool apply_root_motion{true};
};

// Root motion extracted by the latest update of one Animator.
struct RootMotion {
    bool active{false};    // a root-motion state contributed this update
    math::Vec3 position{}; // travel in the Animator entity's parent space (world space for root entities)
    float yaw{0};          // turn about +Y in radians
    bool applied{false};   // moved the entity (AnimatorComponent::apply_root_motion)
};

// Current morph target weights of a mesh entity, one per target of its mesh (the node entity
// above a multi-primitive mesh also drives its primitive children). Written by Animators whose
// clips animate weights and by scripts (AnimationSystem::set_morph_weight); the renderer deforms
// the mesh with them. Runtime state, not serialized: without it the mesh's default weights apply.
struct MorphWeightsComponent {
    std::vector<float> weights;
};

// How joint transforms are blended per vertex (ADR-0205, render/skinning.hpp). Linear is linear
// blend skinning (the cheapest; candy-wraps under twist and thins bent joints); DualQuaternion
// blends rigid transforms as dual quaternions, keeping volume at elbows, knees and twisting
// forearms for a few extra ALU per vertex.
enum class SkinningMode : std::uint8_t { Linear, DualQuaternion };

// Marks a MeshRenderer as GPU-skinned by `skin` of its model, with joints bound by name under
// the `skeleton` entity (the model's instantiated root).
struct SkinnedMeshComponent {
    Uuid skeleton;
    std::uint32_t skin{0};
    SkinningMode method{SkinningMode::Linear};
};

// Registers Animator, SkinnedMesh and SpriteAnimator (reflection: serialization, inspector,
// scripts). Idempotent.
void register_components();

// ---- system ----------------------------------------------------------------------------------

// Returns the AnimationSet of a model asset, or null when it does not exist or has no nodes.
using SetProvider = std::function<std::shared_ptr<const AnimationSet>(const Uuid& model)>;

struct AnimatorStatus {
    bool valid{false};       // entity has an Animator whose model resolved
    std::string clip;        // clip name, or the current state in controller mode
    bool controlled{false};  // a controller drives the Animator
    std::string state;       // current controller state ("" without a controller)
    float normalized_time{0}; // controller mode: cycles of the current state (1 = one cycle)
    float time{0}, duration{0};
    bool playing{false}, looping{false}, finished{false};
    float blend{1};          // cross-fade progress (1 = settled on the current clip)
    std::uint32_t bound_nodes{0}, animated_nodes{0};
};

struct AnimationStats {
    std::uint32_t animators{0}, channels_sampled{0}, nodes_written{0}, missing_models{0};
    std::uint32_t controlled{0}, transitions{0}, controller_errors{0};
    std::uint32_t lod_skipped{0}; // animators not due this frame under simulation LOD (ADR-0165)
    IkStats ik; // inverse kinematics applied after the animated pose (ik.hpp)
    FootPlacementStats feet; // foot placement before IK (foot_placement.hpp; needs a ground probe)
};

// Reads a .vanim asset's text by its path (relative to the assets folder).
using ControllerLoader = std::function<Result<std::string>(std::string_view path)>;
// Loads controllers from `assets_root` (paths are sanitized; no escaping the folder).
[[nodiscard]] ControllerLoader directory_loader(std::filesystem::path assets_root);

// Live view of an Animator's controller (inspector, preview panel, `victus` tooling).
struct ControllerStatus {
    bool valid{false};          // the Animator has a controller that loaded
    std::string controller;     // asset path
    std::string error;          // load/parse error when !valid and a path is set
    std::string state, previous_state; // previous_state is set while a cross-fade runs
    float normalized_time{0};
    float blend{1};             // cross-fade progress (1 = settled)
    struct Param {
        std::string name;
        ParamType type{ParamType::Float};
        float value{0};
    };
    std::vector<Param> parameters;
    struct Weight {
        std::string clip;
        float weight{0};
        bool missing{false}; // the model has no clip of this name (rest pose is used)
    };
    std::vector<Weight> weights; // blend weights of the current state's clips
    std::vector<std::string> states;
    std::vector<std::uint8_t> root_motion_states; // per state: 1 = extracts root motion
    std::string root_node;      // node root motion is taken from ("" when unresolved)
    struct Layer {
        std::string name;
        LayerBlend blend{LayerBlend::Override};
        float weight{1};           // runtime weight (set_layer_weight)
        float effective_weight{1}; // times the weight parameter, if any
        std::string state, previous_state;
        float normalized_time{0};
        float blend_progress{1};   // cross-fade progress inside the layer
        std::uint32_t affected_nodes{0}; // nodes inside the mask that a layer clip animates
        std::vector<std::string> states;
    };
    std::vector<Layer> layers;  // controller layers above the base, in application order
};

struct AnimationSettings {
    SetProvider provider;
    ControllerLoader controllers; // resolves AnimatorComponent::controller paths
    SheetProvider sheets; // sprite sheets for SpriteAnimator (assets::sprite_sheet_provider)
    jobs::JobSystem* jobs{nullptr}; // evaluates animators in parallel when set
    std::uint32_t parallel_threshold{32};
    GroundProbe ground; // FootPlacement raycasts (runtime::physics_ground_probe); none = feet keep the animated pose
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
    // Replaces the ground probe used by FootPlacement (e.g. once the physics world exists).
    void set_ground_probe(GroundProbe probe);

    // Advances and applies every Animator by dt seconds.
    void update(scene::Scene& scene, float dt);
    // Advances and applies only `e`'s Animator (editor previews of one character). False when the
    // entity has no Animator.
    bool update_entity(scene::Scene& scene, scene::Entity e, float dt);
    // Root motion of the entity's latest update (zero when none).
    [[nodiscard]] RootMotion root_motion(scene::Scene& scene, scene::Entity e);
    // Starts `clip` on the entity's Animator. fade < 0 uses the component's crossfade.
    // Returns false when the entity has no Animator or the clip does not exist.
    bool play(scene::Scene& scene, scene::Entity e, std::string_view clip, float fade = -1.0f, bool restart = true);
    // In controller mode `clip` names a state (entered with the fade; restart re-enters it).
    [[nodiscard]] AnimatorStatus status(scene::Scene& scene, scene::Entity e);

    // ---- controllers ----
    // Registers a controller under `path` directly (tests, editor preview); precedes the loader.
    void add_controller(std::string path, std::shared_ptr<const Controller> controller);
    // Forgets loaded controllers (after a .vanim changed). Animators using a changed controller
    // restart in its default state, keeping parameter values whose names still exist.
    void reload_controllers();
    // The controller at `path` (cached), or null with `error` set.
    [[nodiscard]] std::shared_ptr<const Controller> controller(std::string_view path, std::string* error = nullptr);
    // Sets a parameter (bool: value != 0, int: rounded, trigger: value != 0 sets it). False when
    // the entity has no controller or no such parameter.
    bool set_parameter(scene::Scene& scene, scene::Entity e, std::string_view name, float value);
    bool set_trigger(scene::Scene& scene, scene::Entity e, std::string_view name);
    bool reset_trigger(scene::Scene& scene, scene::Entity e, std::string_view name);
    [[nodiscard]] std::optional<float> parameter(scene::Scene& scene, scene::Entity e, std::string_view name);
    // Runtime weight (0..1) of a controller layer by name; false/nullopt without such a layer.
    // Starts at the authored weight; kept across controller hot reloads.
    bool set_layer_weight(scene::Scene& scene, scene::Entity e, std::string_view layer, float weight);
    [[nodiscard]] std::optional<float> layer_weight(scene::Scene& scene, scene::Entity e, std::string_view layer);
    [[nodiscard]] ControllerStatus controller_status(scene::Scene& scene, scene::Entity e);
    [[nodiscard]] std::vector<std::string> clips(scene::Scene& scene, scene::Entity e);
    // Morph targets of the mesh on `e` (its MeshRenderer, else the first child MeshRenderer whose
    // mesh has targets): names in target order, empty when there are none or the model is not
    // resolved. set_morph_weight adds a MorphWeightsComponent to `e` (mesh defaults) on first use;
    // `target` is a name or a 0-based index. Weights are not clamped.
    [[nodiscard]] std::vector<std::string> morph_targets(scene::Scene& scene, scene::Entity e);
    bool set_morph_weight(scene::Scene& scene, scene::Entity e, std::string_view target, float weight);
    bool set_morph_weight(scene::Scene& scene, scene::Entity e, std::size_t target, float weight);
    [[nodiscard]] std::optional<float> morph_weight(scene::Scene& scene, scene::Entity e, std::string_view target);
    [[nodiscard]] std::optional<float> morph_weight(scene::Scene& scene, scene::Entity e, std::size_t target);
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
