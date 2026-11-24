// SPDX-License-Identifier: Apache-2.0
// Luau gameplay scripting.
//
// A script is a Luau module (a `.luau` file in the project's assets folder) that returns a table.
// Entities opt in with a ScriptComponent naming the file. Each scripted entity gets its own
// instance table whose metatable is the module, so module fields act as defaults and the
// component's `properties` override them per entity:
//
//     local Mover = {}
//     Mover.speed = 4                      -- editable default, overridable per entity
//     function Mover:start() end           -- once, before the first update
//     function Mover:update(dt)            -- every frame
//         local p = self.entity.position
//         self.entity.position = p + vector.create(input.axis("A", "D"), 0, 0) * self.speed * dt
//     end
//     function Mover:fixed_update(dt) end  -- at the simulation's fixed rate
//     function Mover:reload() end          -- after hot reload replaced the module
//     -- Physics callbacks (dispatched after update, once per event of the frame's fixed steps):
//     function Mover:on_collision_enter(other, info) end  -- info.point, info.normal (self -> other)
//     function Mover:on_collision_exit(other) end
//     function Mover:on_trigger_enter(other) end
//     function Mover:on_trigger_exit(other) end
//     return Mover
//
// Scene API: `scene.find(name)`, `scene.spawn(name?, parent?)`, `scene.destroy(e)`,
// `scene.query(component)`, `scene.count()`, `scene.instantiate(prefab_path, {position, parent,
// name}?)` creates an instance of a `.vprefab` asset (its scripts start on the next update).
// Sprite animation: `entity:play_sprite(clip, restart?)`, `entity:sprite_animation()`
// ({valid, clip, frame, clip_frame, time, duration, playing, looping, finished}) and
// `entity:sprite_clips()` drive a SpriteAnimator's sheet clips.
// Scene transitions: `scene.load(path, params?)` validates a project scene (e.g.
// "scenes/level2.vscene") and switches to it at the end of this update (true | nil, error);
// `scene.reload(params?)` restarts the current scene; `scene.current()` is its path and
// `scene.params()` the table passed to the load that started it (nil when none). Every script
// instance is dropped and starts again in the new scene; module-level Luau state persists.
//
// Input API: raw keys/mouse `input.down/pressed/released(key)`, `input.axis(neg, pos, ...)`,
// `input.mouse_down/mouse_pressed(b)`, `input.mouse_position/mouse_delta()`; actions from the
// project's input map (keyboard, mouse and gamepads together) `input.action(name)`,
// `input.action_pressed/action_released(name)`, `input.value(name)` (-1..1),
// `input.vector(x_action, y_action)`, `input.actions()`, `input.bindings(name)`,
// `input.rebind(name, {"Space", "Pad.South", "-Pad.LeftX"})`; gamepads `input.gamepad_connected(pad?)`,
// `input.gamepad_button(name, pad?)`, `input.gamepad_axis(name, pad?)`.
//
// Physics API (when a PhysicsWorld is bound to the scene): `physics.raycast(origin, dir, max,
// mask, ignore)`, `physics.raycast2d`, `physics.overlap_sphere/overlap_box/overlap_circle/
// overlap_box2d`, `physics.gravity()/set_gravity(v)`, `physics.gravity2d()/set_gravity2d(v)`,
// entity members `velocity`, `angular_velocity` and methods `add_force`, `add_impulse`,
// `add_torque`, `add_angular_impulse`.
//
// Audio API (no-ops returning false when no AudioEngine is bound): `audio.play(clip, {volume,
// pitch, bus, position})` for one-shots, `audio.set_volume(bus, v)/volume(bus)`,
// `audio.set_muted(bus, m)/muted(bus)`, `audio.available()`; entity methods `play_audio()`,
// `stop_audio()`, `pause_audio(paused)`, `audio_playing()` for the entity's AudioSource;
// acoustics (ADR-0096): `audio.acoustics() -> {zone, zone_weight, underwater, decay, rays,
// occluded}` and `entity:audio_occlusion() -> {blocked, gain, cutoff}` (nil when not traced).
//
// Navigation API (when a NavigationSystem is bound; the navmesh is built on first use from the
// scene's static colliders): `nav.available()`, `nav.find_path(from, to) -> {points}, partial`,
// `nav.nearest(p, extent?)`, `nav.raycast(from, to) -> hit, point, normal`,
// `nav.random_point(seed?)`, `nav.rebuild()`; entity methods for NavAgent entities
// `set_destination(p)`, `stop()` and `nav_state()`.
//
// Terrain API (Terrain entities resolved by a TerrainSystem): `terrain.height(x, z, entity?)`,
// `terrain.sample(x, z, entity?) -> {entity, height, normal, weights}` and
// `terrain.raycast(origin, dir, max?) -> {entity, point, normal, distance}`; nil off the terrain.
// AI API (docs/AI.md; needs an ai::AiSystem bound to the scene): `entity:bb_get(key)`,
// `entity:bb_set(key, value|nil)`, `entity:blackboard() -> {key = value}`,
// `entity:perception() -> {target, visible, last_position, seen, heard, noise_position, ...}`,
// `ai.available()`, `ai.report_noise(position, loudness?, source?)`. Behaviour tree `action`
// leaves without a C++ action call `self:<action>(dt, first)` -> "running"|"success"|"failure"
// and `self:on_task_abort(action)` when interrupted.
//
// Sequencer API (docs/SEQUENCER.md; needs a sequencer::SequencerSystem bound to the scene):
// `entity:play_sequence(path?) -> bool`, `entity:stop_sequence()`, `entity:pause_sequence(paused?)`,
// `entity:seek_sequence(t) -> bool`, `entity:sequence_state() -> {valid, sequence, error, time,
// duration, playing, paused, finished, loops, camera}`, `sequencer.available()`. Event keys call
// `on_sequence_event(name, payload, player)` on the track target's script and on the player's.
//
// Networking API (meaningful when a NetServer/NetClient session is bound; offline every entity
// is local and has authority): `net.role()` ("offline"/"server"/"client"), `net.is_server()`,
// `net.is_client()`, `net.authority()`, `net.id()`, `net.connected()`, `net.local_player()`,
// `net.send(name, value?, to?)`, `net.clients()`, `net.stats()`; entity methods `set_net(key, v)`
// (authority only, replicated), `get_net(key)`, `has_authority()`, `is_local()`, `owner()`.
// Messages arrive as `function M:on_net_message(name, value, sender)` after update. In a session,
// `input.*` inside a script reads the input of the client that owns the entity (none on proxies).
//
// Rendering API (entity methods): `set_material(path|nil)` / `material()` swap a MeshRenderer's
// `.vmat` material asset; `set_mesh("cube"|"sphere"|"plane"|"cylinder"|model_path, index?)`
// / `mesh()`; `set_color(vector, alpha?)` / `color() -> vector, alpha` (MeshRenderer tint or
// Sprite tint); `set_sprite(texture_path|nil)`; `set_visible(bool)` / `visible()` also cover
// MeshRenderers; `set_cast_shadows(bool)`; `set_uv_offset(v)`, `set_uv_scroll(v)` (UV units per
// second) and `set_emissive(v|nil, strength?)` edit the entity's MaterialOverrides.
// Tilemaps: `set_tile(x, y, tile|nil, flip_x?, flip_y?)`, `tile(x, y) -> tile|nil, flip_x, flip_y`,
// `fill_tiles(x0, y0, x1, y1, tile|nil) -> changed`, `clear_tiles()`, `paint_terrain(x, y,
// name|nil)` (autotiling; needs the tile set loaded), `world_to_tile(point) -> x, y`,
// `tile_to_world(x, y) -> vector` (cell centre) and `tile_bounds() -> x0, y0, x1, y1 | nil`.
//
// Save API (when HostOptions::save_directory is set; otherwise `save.available()` is false and the
// other calls return nil plus an error): `save.write(slot, data, label?)`, `save.read(slot) ->
// data, info` (info: version, saved_at, label, recovered, migrated), `save.exists(slot)`,
// `save.delete(slot)`, `save.list()`, `save.version()`, `save.migrate(from, fn)` (fn(data) returns
// the data upgraded to version from + 1; they run in order when an older save is read).
// Checkpoints capture the whole scene: `save.checkpoint(slot, label?)` writes every entity plus
// what each script's `function M:save_state()` returns; `save.restore(slot)` validates the slot
// and, at the end of the frame's update, reloads the scene from it, restarts every script and
// passes its state to `function M:load_state(state)` right after `start`. Entities inside saved
// data are stored by id and come back as entities (nil if gone). `scene.find_id(id)` finds an
// entity by its `entity.id`. Checkpoints are refused in multiplayer sessions.
//
// Safety: every VM is sandboxed (read-only standard libraries and globals, per-script global
// tables, no io/os/package/debug access), each call has a wall-clock budget enforced through the
// interrupt callback, and the heap has a byte limit. A failing instance is disabled and its
// error logged (channel "script") until the module is reloaded.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/process.hpp"
#include "victus/core/result.hpp"
#include "victus/script/api.hpp"
#include "victus/script/debugger.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/input_map.hpp"
#include "victus/gameplay/save.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

namespace victus::script {

struct ScriptComponent {
    std::string script;                         // path relative to the assets folder, e.g. "scripts/mover.luau"
    json::Value properties = json::Value::object(); // per-entity overrides of module fields
    bool enabled{true};
};

// Registers the "Script" component serializer. Idempotent.
void register_components();

// A module field that the editor can show and override (number, bool, string or vector).
struct PropertyInfo {
    std::string name;
    json::Value default_value; // number, bool, string or [x, y, z]
};

// A prefab and its asset UUID, as returned by HostOptions::prefab_loader.
struct PrefabSource {
    Uuid asset;
    scene::Prefab prefab;
};

struct HostOptions {
    std::filesystem::path assets_root;   // scripts are resolved under this folder
    double call_budget_seconds{0.25};    // per start/update call; exceeded calls raise an error
    std::size_t memory_limit_bytes{256u << 20};
    bool watch_files{false};             // poll script files for changes and hot reload them
    double watch_interval_seconds{0.5};
    // Actions behind input.action/value/vector (the project's "input" section; scripts may
    // rebind them for the session with input.rebind).
    gameplay::InputMap input_map = gameplay::default_input_map();
    // Resolves `scene.instantiate(path)`. Empty: reads `<assets_root>/<path>` and the UUID from
    // its `.meta` sidecar (nil when there is none). Players pass one backed by the asset database.
    std::function<Result<PrefabSource>(std::string_view path)> prefab_loader;
    // Resolves asset paths used by `entity:set_material/set_mesh/set_sprite` to asset UUIDs.
    // Empty: reads the UUID from `<assets_root>/<path>.meta`. Players pass one backed by the
    // asset database (exported games have no .meta files).
    std::function<Result<Uuid>(std::string_view path)> asset_resolver;
    // Folder behind the `save` API (empty: saves unavailable) and the game's save data version
    // (the project's "save_version").
    std::filesystem::path save_directory;
    std::uint32_t save_version{1};
    // Fallback locale of the `loc` API and keyed UiText (the project's "default_locale"); string
    // tables are read from <assets_root>/localization/*.vstrings.
    std::string default_locale{"en"};
    // Nonzero: math.random starts from this seed, so runs with the same input repeat exactly
    // (regression captures). Zero: Luau's default per-VM seed.
    std::uint64_t random_seed{0};
    // Scene transitions (`scene.load(path)`): scene paths are relative to this folder (the
    // project root). Empty: the parent of assets_root.
    std::filesystem::path project_root;
    // Reads a scene document for `scene.load`. Empty: reads `<project_root>/<path>` (through
    // any mounted pack, so exported games work unchanged).
    std::function<Result<json::Value>(std::string_view path)> scene_loader;
    // Called right after a transition replaced the scene (players reset physics, audio, ...).
    std::function<void(scene::Scene& scene, std::string_view path)> on_scene_loaded;
    // Resolves `bridge.open(name)` to a helper process (players read the project's "bridges"
    // section, see project::bridge_launch). Empty: bridges are unavailable to scripts.
    std::function<Result<process::LaunchOptions>(std::string_view name)> bridge_launcher;
    // Native plugin functions for `plugins.call(name, args)` / `plugins.has(name)` (ADR-0078):
    // JSON text in, JSON text out. Players wire these to plugin::PluginManager. Empty: scripts
    // see no plugin functions.
    std::function<Result<std::string>(std::string_view name, std::string_view args_json)> plugin_call;
    std::function<bool(std::string_view name)> plugin_has;
    // Compiles visual script graphs with trace instrumentation and records which nodes run
    // (graph_node_hits). The editor turns it on for play mode (ADR-0060).
    bool trace_graphs{false};
    // Compiles scripts with full debug information (local names) and enables the source
    // debugger (set_debug_handler, ADR-0099). Off: breakpoints are never hit.
    bool debugger{false};
    // Script API the game's scripts were written for (victus.project "script_api", ADR-0155).
    // create() fails when this build cannot run them (different major, or a newer minor).
    ApiVersion script_api{kScriptApiVersion};
};

// The most recent runtime error of a visual script graph.
struct GraphRuntimeError {
    std::uint32_t node{0}; // 0 when the failing line does not belong to a node
    std::string message;   // annotated message ("... [node 3 Log]")
    std::uint64_t serial{0}; // increases with every error of the host
};

struct HostStats {
    std::size_t modules{0};
    std::size_t instances{0};
    std::size_t faulted{0};
    std::uint64_t calls{0};
    std::uint64_t errors{0};
    std::uint64_t reloads{0};
    std::size_t heap_bytes{0};
    std::uint64_t scene_loads{0}; // completed scene transitions
};

// Owns one sandboxed Luau VM and runs the scripts of one scene.
class ScriptHost {
public:
    static Result<std::unique_ptr<ScriptHost>> create(HostOptions options);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Runs start/update for every enabled ScriptComponent (Phase::Update) and fixed_update
    // (Phase::FixedUpdate). Called by the systems that add_script_systems() installs.
    void update(scene::Scene& scene, const gameplay::FrameTime& time, const gameplay::InputSource* input);
    void fixed_update(scene::Scene& scene, const gameplay::FrameTime& time, const gameplay::InputSource* input);
    // Phase::PreUpdate: Game Rules' pause control, which must work while the game is paused.
    void pre_update(scene::Scene& scene, const gameplay::InputSource* input);

    // Re-reads changed script files now (also done periodically when watch_files is set).
    // Returns the number of modules that were reloaded successfully.
    std::size_t reload_changed();

    // Compiles and runs a module (cached) and lists its editable fields. Used by the editor.
    [[nodiscard]] Result<std::vector<PropertyInfo>> describe(std::string_view script_path);

    // Runs a chunk of Luau source in the sandbox (tests, console). Returns its first result
    // converted to JSON (null when it returns nothing).
    [[nodiscard]] Result<json::Value> eval(std::string_view source, scene::Scene* scene = nullptr,
                                           std::string_view chunk_name = "eval");

    // Re-reads the string tables (hot reload of .vstrings files); the current locale is kept when
    // it still has a table. Returns the number of locales loaded.
    std::size_t reload_localization();

    // Drops all instances (start runs again on the next update). Modules stay cached.
    void reset_instances();

    [[nodiscard]] HostStats stats() const;
    [[nodiscard]] const std::vector<std::string>& recent_errors() const noexcept;
    [[nodiscard]] const HostOptions& options() const noexcept;
    // Visual script debugging: how many times each event/statement node of the graph at `path`
    // has run (needs HostOptions::trace_graphs; empty otherwise).
    [[nodiscard]] std::unordered_map<std::uint32_t, std::uint64_t> graph_node_hits(std::string_view path) const;
    // The last runtime error raised by the graph at `path` (cleared when it reloads).
    [[nodiscard]] std::optional<GraphRuntimeError> last_graph_error(std::string_view path) const;
    // Drops cached prefabs so the next scene.instantiate re-reads them (hot reload).
    void forget_prefabs();
    // Writes a checkpoint of `scene` (entities + every started script's save_state()) to `slot`.
    [[nodiscard]] Status checkpoint(scene::Scene& scene, std::string_view slot, std::string_view label = {});
    // Loads a checkpoint into `scene` now (not from inside update): reloads the entities, drops
    // every script instance and hands each script its state after its next start.
    [[nodiscard]] Status restore(scene::Scene& scene, std::string_view slot);
    // The save store, or nullptr when HostOptions::save_directory is empty.
    [[nodiscard]] gameplay::SaveStore* saves();
    // Replaces the input actions scripts see (e.g. edited in the editor while playing).
    void set_input_map(gameplay::InputMap map);
    // The project-relative path of the running scene (`scene.current()`); players set it at
    // startup and transitions update it.
    void set_current_scene(std::string path);
    [[nodiscard]] const std::string& current_scene() const noexcept;
    // The params of the load that started the running scene (`scene.params()`; null when none).
    [[nodiscard]] const json::Value& scene_params() const noexcept;
    // Switches to the scene at `path` now (not from inside update), as `scene.load` would at the
    // end of an update: validates it first and leaves `scene` untouched on failure.
    [[nodiscard]] Status load_scene(scene::Scene& scene, std::string_view path, json::Value params = {});

    // ---- source debugger (needs HostOptions::debugger; ADR-0099) ----
    // Installs the stop handler (empty: detach; breakpoints stay but are not hit) and the
    // logpoint output (empty: the engine log).
    void set_debug_handler(DebugHandler handler, DebugOutput output = {});
    // Replaces the breakpoints of the script at `path` (assets-relative, as scripts are named).
    std::vector<BreakpointState> set_breakpoints(std::string_view path, std::vector<SourceBreakpoint> breakpoints);
    // The current state of the breakpoints of `path` (lines move once the script loads).
    [[nodiscard]] std::vector<BreakpointState> breakpoints(std::string_view path) const;
    // Stops when an error escapes a script call (before the error is reported).
    void set_break_on_error(bool enabled);
    // Stops at the next line any script runs. Safe to call from another thread.
    void request_pause();
    [[nodiscard]] DebugStats debug_stats() const;

    struct Impl;

private:
    friend Result<json::Value> describe_api();
    explicit ScriptHost(std::unique_ptr<Impl> impl);
    void dispatch_physics_events(scene::Scene& scene);
    void dispatch_ui_events(scene::Scene& scene);
    void dispatch_sequence_events(scene::Scene& scene);
    void dispatch_gameplay_events(scene::Scene& scene);
    void dispatch_net_messages(scene::Scene& scene);
    std::unique_ptr<Impl> impl_;
};

// Installs "script.update" (Update) and "script.fixed_update" (FixedUpdate) systems that drive
// `host` with the simulation's time and input. The host must outlive the simulation's use.
void add_script_systems(gameplay::Simulation& sim, ScriptHost& host);

// Reads the prefab `<assets_root>/<path>` and its asset UUID from the `.meta` sidecar (nil when
// there is none). The default HostOptions::prefab_loader.
[[nodiscard]] Result<PrefabSource> read_prefab(const std::filesystem::path& assets_root, std::string_view path);

// Compiles source to Luau bytecode without running it; returns the compile error message on
// failure (used by import validation and tests).
[[nodiscard]] Status check_syntax(std::string_view source, std::string_view chunk_name);

// The Luau source behind a script asset: `text` itself for `.luau`, the compiled graph for
// `.vgraph` visual scripts (ADR-0058; graph errors become the error message).
[[nodiscard]] Result<std::string> luau_source(std::string_view path, std::string text);
// Scene paths a script passes as string literals to `scene.load("...")` (export and check
// verify they exist). Computed paths are not found; they are checked when the call runs.
[[nodiscard]] std::vector<std::string> scene_references(std::string_view source);

} // namespace victus::script
