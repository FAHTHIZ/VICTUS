// SPDX-License-Identifier: Apache-2.0
// Gameplay AI: behaviour trees with a per-entity blackboard, and perception (sight and hearing).
//
// A behaviour tree is a JSON document (`.vbt`, see docs/AI.md):
//
//     { "blackboard": { "speed": 3 },
//       "root": { "type": "selector", "children": [
//           { "type": "guard", "key": "target", "op": "set",
//             "child": { "type": "move_to", "key": "target", "acceptance": 1.5 } },
//           { "type": "sequence", "children": [
//               { "type": "random_point", "key": "wander", "radius": 8 },
//               { "type": "move_to", "key": "wander" },
//               { "type": "wait", "seconds": 2, "deviation": 1 } ] } ] } }
//
// Trees are ticked from the root each AI tick. Composites remember their running child (so
// long actions resume) unless `"reactive": true`, in which case earlier children are re-checked
// every tick and pre-empt later ones; guards re-check their condition every tick. A running node
// that is no longer reached is aborted (move_to stops its agent, actions get an abort call).
//
// Perception: entities with a Perception component see Perceptible entities whose team bit is in
// `target_mask` inside the sight cone (range, field of view) with an unobstructed physics ray,
// and hear noises reported through AiSystem::report_noise. Results are written to the
// perceiver's blackboard (`<target_key>`, `<target_key>_position`, `<target_key>_visible`,
// `noise_position`, `heard`) so trees react to them with plain checks.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace victus::ai {

using scene::Entity;

// ---- blackboard -------------------------------------------------------------------------------

// Nil, bool, number, vector, entity or string.
using Value = std::variant<std::monostate, bool, double, math::Vec3, Entity, std::string>;

// Number/bool/string/[x, y, z] (null for anything else).
[[nodiscard]] Value value_from_json(const json::Value& v);
[[nodiscard]] std::string to_string(const Value& v);
[[nodiscard]] bool truthy(const Value& v) noexcept; // set and not false/0/empty string

// Per-entity key/value memory shared by its behaviour tree, perception, scripts and C++.
class Blackboard {
public:
    void set(std::string_view key, Value value);
    bool erase(std::string_view key);
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }
    [[nodiscard]] double number(std::string_view key, double fallback = 0) const noexcept;
    [[nodiscard]] bool boolean(std::string_view key, bool fallback = false) const noexcept;
    [[nodiscard]] std::optional<math::Vec3> vec3(std::string_view key) const noexcept;
    [[nodiscard]] Entity entity(std::string_view key) const noexcept;
    // Sorted by key (deterministic iteration for debug views and tests).
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& entries() const noexcept { return entries_; }
    // Incremented by every change.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    void clear();

private:
    std::vector<std::pair<std::string, Value>> entries_;
    std::uint64_t revision_{0};
};

// ---- behaviour trees --------------------------------------------------------------------------

enum class NodeStatus : std::uint8_t { Idle, Running, Success, Failure };
[[nodiscard]] std::string_view status_name(NodeStatus s) noexcept;

enum class NodeType : std::uint8_t {
    // composites
    Sequence, Selector, Parallel,
    // decorators (one child)
    Inverter, Succeeder, Failer, Repeat, Cooldown, Timeout, Guard,
    // leaves
    Wait, Check, Set, Clear, MoveTo, RandomPoint, Action, Succeed, Fail, Log,
};
[[nodiscard]] std::string_view node_type_name(NodeType t) noexcept;

enum class CompareOp : std::uint8_t { IsSet, NotSet, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, True, False };

struct Node {
    NodeType type{NodeType::Succeed};
    std::string name;                    // optional label (debug views)
    std::vector<std::uint32_t> children; // node indices
    std::uint32_t depth{0};
    std::string key;     // Check/Guard/Set/Clear/MoveTo/RandomPoint
    CompareOp op{CompareOp::IsSet};
    Value value;         // Check/Guard compare value, Set value
    float seconds{0};    // Wait duration, Cooldown, Timeout
    float deviation{0};  // Wait: +- random seconds
    std::uint32_t count{0};     // Repeat (0 = forever)
    std::uint32_t threshold{0}; // Parallel: successes needed (0 = all children)
    bool reactive{false};       // Sequence/Selector re-evaluate earlier children every tick
    float acceptance{0.5f};     // MoveTo arrival distance (m)
    float speed{3.5f};          // MoveTo speed for entities without a NavAgent (m/s)
    float radius{8.0f};         // RandomPoint search radius around the entity (m)
    std::string action;         // Action name / Log message
};

class BehaviorTree {
public:
    static constexpr std::size_t kMaxNodes = 4096;
    [[nodiscard]] static Result<std::shared_ptr<const BehaviorTree>> parse(std::string_view text, std::string name = {});
    [[nodiscard]] static Result<std::shared_ptr<const BehaviorTree>> from_json(const json::Value& doc, std::string name = {});
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const Blackboard& defaults() const noexcept { return defaults_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    std::vector<Node> nodes_; // pre-order; node 0 is the root
    Blackboard defaults_;
    std::string name_;
};

// Passed to registered actions. `aborted` is set on the final call when a running action is
// pre-empted (its return value is then ignored).
struct ActionContext {
    scene::Scene& scene;
    Entity entity;
    Blackboard& blackboard;
    const Node& node;
    float dt{0};
    bool first{false};   // first tick since the node started
    bool aborted{false};
};
using ActionFn = std::function<NodeStatus(ActionContext&)>;

// ---- components -------------------------------------------------------------------------------

// Runs a behaviour tree asset on this entity.
struct BehaviorTreeComponent {
    std::string tree;          // path relative to the assets folder, e.g. "ai/guard.vbt"
    float tick_interval{0.0f}; // seconds between ticks (0 = every frame)
    bool enabled{true};
    friend bool operator==(const BehaviorTreeComponent&, const BehaviorTreeComponent&) = default;
};

struct PerceptionComponent {
    float sight_range{15.0f};
    float fov{120.0f};          // degrees, full cone angle around the entity's +Z axis
    float eye_height{1.6f};     // eye above the entity origin (world up)
    float hearing_range{10.0f}; // 0 = deaf
    float memory{3.0f};         // seconds a lost target (or a noise) is remembered
    std::uint32_t target_mask{0xFFFFFFFFu}; // Perceptible team bits that are noticed
    std::uint32_t occlusion_mask{0xFFFFFFFFu}; // collider layers that block sight
    bool line_of_sight{true};
    std::string target_key{"target"};
    friend bool operator==(const PerceptionComponent&, const PerceptionComponent&) = default;
};

// Something perception can notice.
struct PerceptibleComponent {
    std::uint32_t team{1u};  // bit mask
    float height{1.0f};      // aim point above the origin used for sight rays
    bool enabled{true};
    friend bool operator==(const PerceptibleComponent&, const PerceptibleComponent&) = default;
};

// What a perceiver currently notices (AiSystem::perception).
struct PerceptionState {
    Entity target;             // current target (kept for `memory` seconds after losing sight)
    bool visible{false};
    math::Vec3 last_position;  // last seen position of the target
    float since_seen{0};
    std::vector<Entity> seen;  // every visible perceptible this tick, nearest first
    bool heard{false};
    math::Vec3 noise_position;
    float since_heard{0};
};

struct NodeDebug {
    NodeType type;
    std::string label; // name or type plus key/action
    std::uint32_t depth{0};
    NodeStatus status{NodeStatus::Idle}; // last result (Running for the active path)
    bool active{false};          // reached on the last tick
};

struct TreeDebug {
    std::string tree;  // asset path
    std::string error; // load/parse error, if any
    std::vector<NodeDebug> nodes;
    std::uint64_t ticks{0};
    std::uint32_t completions{0}; // root finished (the tree restarts on the next tick)
    NodeStatus last_root{NodeStatus::Idle};
};

struct DebugLine {
    math::Vec3 a, b;
    math::Color color;
};

struct AiStats {
    std::uint32_t trees{0};
    std::uint32_t perceivers{0};
    std::uint64_t ticks{0};
    std::uint32_t load_errors{0};
};

// Reads tree text by asset path (e.g. from the project's assets folder or a package mount).
using TreeLoader = std::function<Result<std::string>(std::string_view path)>;
// Reads sanitized relative paths under `assets_root` (mounted packs included).
[[nodiscard]] TreeLoader directory_loader(std::filesystem::path assets_root);

class AiSystem {
public:
    explicit AiSystem(TreeLoader loader = {});
    ~AiSystem();
    AiSystem(const AiSystem&) = delete;
    AiSystem& operator=(const AiSystem&) = delete;

    // Named leaf behaviour for `{ "type": "action", "action": name }`.
    void register_action(std::string name, ActionFn fn);
    [[nodiscard]] bool has_action(std::string_view name) const noexcept;
    // Runs action leaves whose name has no registered function (the script host installs one so
    // Luau methods on the entity's script act as tasks). Returning Idle means "not handled": the
    // leaf fails and an unknown-action warning is logged once. Also called with
    // ActionContext::aborted when such a running leaf is interrupted.
    void set_fallback_action(ActionFn fn);
    [[nodiscard]] bool has_fallback_action() const noexcept;
    // Uses `tree` for the entity instead of loading BehaviorTreeComponent::tree (nullptr: back
    // to the asset). Restarts the tree.
    void set_tree(scene::Scene& scene, Entity e, std::shared_ptr<const BehaviorTree> tree);
    // Drops cached trees so changed files load again (all when `path` is empty).
    void reload(std::string_view path = {});

    // The entity's blackboard (created on demand and owned by the system, so references stay
    // valid while components are added; nullptr for dead entities).
    Blackboard* blackboard(scene::Scene& scene, Entity e);
    [[nodiscard]] const PerceptionState* perception(scene::Scene& scene, Entity e) const;
    // A sound at `position` heard by perceivers within their hearing range of it (plus `loudness`
    // metres). `source` is ignored by its own perception.
    void report_noise(math::Vec3 position, float loudness = 0.0f, Entity source = {});

    // Perception, then behaviour trees.
    void update(scene::Scene& scene, float dt);

    [[nodiscard]] std::optional<TreeDebug> debug(scene::Scene& scene, Entity e) const;
    // Sight cones and lines to targets.
    void debug_lines(scene::Scene& scene, std::vector<DebugLine>& out) const;
    [[nodiscard]] const AiStats& stats() const noexcept { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AiStats stats_;
};

// Registers "BehaviorTree", "Perception" and "Perceptible" (inspector category "AI"). Idempotent.
void register_components();
// The AI system bound to a scene (used by scripts); nullptr when none.
[[nodiscard]] AiSystem* ai_of(scene::Scene& scene);
void bind(scene::Scene& scene, AiSystem* ai);
// Binds and updates the system in Phase::Update (after navigation was added).
void add_ai_systems(gameplay::Simulation& sim, AiSystem& ai);

} // namespace victus::ai
