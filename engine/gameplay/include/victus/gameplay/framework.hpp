// SPDX-License-Identifier: Apache-2.0
// Gameplay framework primitives (ADR-0094): hierarchical tags, a scene event bus, saved timers,
// attributes with timed/periodic effects (a compact take on Unreal's Gameplay Ability System:
// attributes, gameplay effects that grant tags, abilities with costs and cooldowns) and item
// inventories. Everything lives in components or scene resources, is saved with the scene (save
// games keep health, cooldowns, running timers and inventories) and is exposed to Luau.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace victus::gameplay {

using scene::Entity;

// ---- tags ----------------------------------------------------------------------------------------

// Authored tags, separated by spaces or commas: "Enemy Status.Burning". Names use letters,
// digits, '_', '-' and '.', where dots form a hierarchy: a query "Status" matches the tag
// "Status.Burning" (and "Status" itself), but not "StatusBar".
struct TagsComponent {
    std::string tags;
};
[[nodiscard]] bool valid_tag(std::string_view tag) noexcept;
[[nodiscard]] bool tag_matches(std::string_view tag, std::string_view query) noexcept;
[[nodiscard]] std::vector<std::string> split_tags(std::string_view text);
// Authored tags plus the tags granted by the entity's active effects.
[[nodiscard]] bool has_tag(scene::Scene& scene, Entity e, std::string_view query);
[[nodiscard]] std::vector<std::string> tags_of(scene::Scene& scene, Entity e);
// Adds/removes an authored tag (exact name). False when invalid, already present or missing.
bool add_tag(scene::Scene& scene, Entity e, std::string_view tag);
bool remove_tag(scene::Scene& scene, Entity e, std::string_view tag);
// Entities whose tags match `query`, in entity-index order.
[[nodiscard]] std::vector<Entity> find_tagged(scene::Scene& scene, std::string_view query);

// ---- events --------------------------------------------------------------------------------------

struct GameplayEvent {
    std::uint64_t id{0}; // increasing per scene
    std::string name;
    json::Value payload;
    Entity sender;
    Entity target; // null = broadcast
};
using EventListener = std::function<void(scene::Scene&, const GameplayEvent&)>;

// Queues an event. C++ listeners run immediately; scripts receive it as
// `on_event(name, payload, sender)` after their next update (the target's scripts, or every
// script for a broadcast). Events stay readable through `recent_events` for two ticks.
std::uint64_t emit_event(scene::Scene& scene, std::string_view name, json::Value payload = {}, Entity sender = {},
                         Entity target = {});
// `name` empty = every event. Returns a handle for unlisten.
std::uint64_t listen(scene::Scene& scene, std::string name, EventListener fn);
void unlisten(scene::Scene& scene, std::uint64_t handle);
[[nodiscard]] const std::vector<GameplayEvent>& recent_events(scene::Scene& scene);
inline constexpr std::size_t kMaxQueuedEvents = 4096;
// Starts a tick: forgets events older than the previous tick (add_builtin_systems runs it in
// Phase::PreUpdate).
void begin_event_tick(scene::Scene& scene);

// ---- timers --------------------------------------------------------------------------------------

struct Timer {
    std::string name;
    float remaining{0};
    float interval{0}; // repeating timers restart with this period
    bool repeat{false};
    bool paused{false};
};
// Saved timers. A timer firing emits the event `name` (payload {timer = true}) from and to its
// entity. Ticked in Phase::FixedUpdate, so they are frame-rate independent and replayable.
struct TimersComponent {
    std::vector<Timer> timers;
};
// Starts (or restarts) a timer; `seconds` <= 0 fires on the next tick.
void start_timer(scene::Scene& scene, Entity e, std::string_view name, float seconds, bool repeat = false);
bool stop_timer(scene::Scene& scene, Entity e, std::string_view name);
bool pause_timer(scene::Scene& scene, Entity e, std::string_view name, bool paused);
[[nodiscard]] std::optional<float> timer_remaining(scene::Scene& scene, Entity e, std::string_view name);
void update_timers(scene::Scene& scene, float dt);

// ---- attributes and effects ------------------------------------------------------------------------

// Authored defaults, one attribute per ';' or newline: `name = base [min..max]`, where min and
// max are numbers or other attribute names: "health = 100 [0..max_health]; max_health = 100".
// The runtime Attributes component is created from it on first use and follows later edits
// (new attributes are added and bounds updated; current base values are kept).
struct AttributeSetComponent {
    std::string definition;
};
struct Attribute {
    std::string name;
    float base{0};
    float min{-std::numeric_limits<float>::infinity()};
    float max{std::numeric_limits<float>::infinity()};
    std::string min_attribute, max_attribute; // bound by another attribute's current value
    float value{0}; // current value after effects, as of the last refresh
};
enum class ModifierOp : std::uint8_t { Add, Multiply, Override };
enum class Stacking : std::uint8_t { Refresh, Stack, Independent };
struct EffectSpec {
    std::string name;      // identifies the effect for stacking and removal
    std::string attribute; // empty: the effect only grants its tag (statuses, cooldowns)
    ModifierOp op{ModifierOp::Add};
    float magnitude{0};
    float duration{0};   // 0 = instant (changes the base value once); < 0 = until removed
    float period{0};     // > 0: applies `magnitude` to the base value every period while active (DoT, regeneration)
    std::string grant_tag; // tag the entity has while the effect is active
    Stacking stacking{Stacking::Refresh};
    std::uint32_t max_stacks{1};
};
struct ActiveEffect {
    std::uint64_t id{0};
    EffectSpec spec;
    float remaining{0};
    float next_tick{0};
    std::uint32_t stacks{1};
    Uuid source;
};
struct AttributesComponent {
    std::vector<Attribute> attributes;
    std::vector<ActiveEffect> effects;
    std::uint64_t next_effect{1};
    std::uint64_t definition_hash{0};
};
// Parses an AttributeSet definition; fails with the offending entry.
[[nodiscard]] Result<std::vector<Attribute>> parse_attributes(std::string_view definition);

[[nodiscard]] bool has_attribute(scene::Scene& scene, Entity e, std::string_view name);
// Current value: (base + adds) * multipliers, or the latest override, clamped to the bounds.
[[nodiscard]] std::optional<float> attribute(scene::Scene& scene, Entity e, std::string_view name);
[[nodiscard]] std::optional<float> attribute_base(scene::Scene& scene, Entity e, std::string_view name);
// Sets the base value (creating the attribute when missing), clamped to its bounds.
void set_attribute_base(scene::Scene& scene, Entity e, std::string_view name, float value);
// Instant change of the base value (damage, healing). Returns the new current value.
std::optional<float> modify_attribute(scene::Scene& scene, Entity e, std::string_view name, float delta);
// Instant effects change the base value and return 0; others become active and return their id
// (a refreshed or stacked effect returns the existing id).
std::uint64_t apply_effect(scene::Scene& scene, Entity e, const EffectSpec& spec, Entity source = {});
bool remove_effect(scene::Scene& scene, Entity e, std::uint64_t id);
std::size_t remove_effects(scene::Scene& scene, Entity e, std::string_view name);
[[nodiscard]] const ActiveEffect* find_effect(scene::Scene& scene, Entity e, std::string_view name);
// Ticks effects (periodic application, expiry) and emits `attribute_changed`
// {attribute, old, value} and `attribute_depleted` {attribute} (current value reached a finite
// minimum) to the entity.
void update_attributes(scene::Scene& scene, float dt);

// ---- abilities -----------------------------------------------------------------------------------

struct AbilitySpec {
    std::string name;
    float cooldown{0};                                 // s; tracked as the effect "cooldown.<name>"
    std::vector<std::pair<std::string, float>> costs;  // attribute, amount (all must be affordable)
    std::string required_tags;                         // all needed
    std::string blocked_tags;                          // any blocks
};
enum class AbilityResult : std::uint8_t { Activated, OnCooldown, Blocked, MissingTags, CannotAfford };
[[nodiscard]] std::string_view ability_result_name(AbilityResult r) noexcept;
// Checks cooldown, tags and costs; on success pays the costs, starts the cooldown (granting the
// tag "Cooldown.<name>") and emits `ability_activated` {ability} to the entity.
AbilityResult activate_ability(scene::Scene& scene, Entity e, const AbilitySpec& spec);
[[nodiscard]] float ability_cooldown(scene::Scene& scene, Entity e, std::string_view name);

// ---- inventory -----------------------------------------------------------------------------------

struct ItemStack {
    std::string item;
    std::uint32_t count{0};
};
struct InventoryComponent {
    std::uint32_t capacity{20};  // slots
    std::uint32_t max_stack{99}; // default stack size
    std::vector<ItemStack> slots;
};
// Fills existing stacks of the item, then free slots. Returns the amount added (the rest did not
// fit). Emits `inventory_changed` {item, delta, count} to the entity when something changed.
std::uint32_t add_item(scene::Scene& scene, Entity e, std::string_view item, std::uint32_t count, std::uint32_t max_stack = 0);
// Takes from the last stacks first. Returns the amount removed.
std::uint32_t remove_item(scene::Scene& scene, Entity e, std::string_view item, std::uint32_t count);
[[nodiscard]] std::uint32_t item_count(scene::Scene& scene, Entity e, std::string_view item);
// Moves up to `count` items; only what fits in `to` leaves `from`. Returns the amount moved.
std::uint32_t transfer_item(scene::Scene& scene, Entity from, Entity to, std::string_view item, std::uint32_t count);

// Registers the components above (called by register_components).
void register_framework_components();

} // namespace victus::gameplay
