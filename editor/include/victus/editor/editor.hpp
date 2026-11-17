// SPDX-License-Identifier: Apache-2.0
// The VICTUS editor: project launcher, scene editing workspace (viewport, hierarchy, inspector,
// assets, console), undo/redo and isolated play mode. The class is UI-toolkit driven but every
// operation is also a plain method so tests and tools can drive the editor without input.
#pragma once
#include "victus/core/profiler.hpp"
#include "victus/animation/animation.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/gizmo.hpp"
#include "victus/editor/thumbnails.hpp"
#include "victus/editor/undo.hpp"
#include "victus/core/jobs.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/physics/physics.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/script/graph.hpp"
#include "victus/script/script.hpp"
#include "victus/ui/ui.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::platform {
class Platform;
class Window;
} // namespace victus::platform

namespace victus::editor {

struct EditorOptions {
    fs::Path recent_file = project::RecentProjects::default_file();
    fs::Path default_project_dir;  // launcher default location; empty = <home>/VICTUS Projects
    float autosave_seconds{30.0f}; // unsaved scene edits are autosaved this often; 0 disables
};

enum class PlayState : std::uint8_t { Editing, Playing, Paused };

// How Play runs the scene. Host: the editor is a listen server on UDP `port` and plays the
// host's character; others join with `victus run <project> --connect <ip>:<port>` (or the
// exported player). Settings live with the editor session, not in the project file.
struct NetPlaySettings {
    enum class Mode : std::uint8_t { Offline, Host };
    Mode mode{Mode::Offline};
    std::uint16_t port{net::kDefaultPort};
    std::uint16_t max_clients{8};
    std::string player_name{"host"};
    std::string password;        // required from clients when non-empty (needs encryption)
    bool insecure{false};        // plaintext session (no key, no password)
    std::string conditions;      // link emulation on the host's sends, e.g. "latency=80,loss=0.05"
    // Extra players simulated inside the editor (0-3): each is a full client (its own scene copy,
    // scripts and physics) joined over UDP loopback; View switches the viewport between them.
    std::uint16_t editor_clients{0};
    friend bool operator==(const NetPlaySettings&, const NetPlaySettings&) = default;
};

enum class Primitive : std::uint8_t { Empty, Cube, Sphere, Plane, Cylinder, Camera, DirectionalLight, PointLight, SpotLight, Sprite };

// Command palette entry (Ctrl+Shift+P): every menu action, window toggle, scene, asset and entity.
struct EditorCommand {
    std::string name;     // "View: Toggle Profiler", "Open Scene: scenes/main.vscene", "Select: Player"
    std::string shortcut; // display only
    bool enabled{true};
    std::function<void()> run;
};

// Fuzzy subsequence score of `query` in `text` (case-insensitive); -1 when it does not match.
// Higher is better: consecutive characters and word starts score more, shorter texts win ties.
[[nodiscard]] int fuzzy_match_score(std::string_view query, std::string_view text) noexcept;

// Multi-entity edits: applies the difference between `before` and `after` (component JSON) to
// `target`, field by field (vector elements individually); added/removed keys are added/removed.
[[nodiscard]] json::Value merge_edit(const json::Value& before, const json::Value& after, const json::Value& target);

struct EditorClient; // an in-editor player of a hosted session (editor_clients.cpp)

struct EditorCamera {
    math::Vec3 target{0, 0.5f, 0};
    float distance{10.0f};
    float yaw{0.6f};    // radians
    float pitch{-0.45f};
    // 2D: position/zoom/rotation; pixels_per_unit is derived from the viewport height so the
    // view always spans `view_height` world units at zoom 1 (matching orthographic cameras).
    render::Camera2D cam2d{{0, 0}, 1.0f, 0.0f, 1.0f};
    float view_height{10.0f};
    [[nodiscard]] render::Camera3D camera3d() const;
    [[nodiscard]] render::Camera2D camera2d(float viewport_height) const;
};

class Editor {
public:
    [[nodiscard]] static Result<std::unique_ptr<Editor>> create(render::GpuContext& gpu, ui::UiContext& ui,
                                                                platform::Platform* platform, platform::Window* window,
                                                                const EditorOptions& options = {});
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // ---- per-frame ----
    // Builds the UI (between UiContext::new_frame and prepare) and advances play mode.
    void frame(float dt);
    // Renders scene viewports into their textures. Call after frame(), outside render passes.
    [[nodiscard]] Status render(rhi::CommandBuffer& cmd);
    // Asks to close the editor (window close). Returns true when it may close now; otherwise a
    // save-changes prompt is shown and quit_confirmed() becomes true once resolved.
    bool request_quit();
    [[nodiscard]] bool quit_confirmed() const noexcept { return quit_confirmed_; }

    // ---- projects ----
    [[nodiscard]] Status open_project(const fs::Path& path);
    [[nodiscard]] Status create_project(const fs::Path& directory, std::string_view name, std::string_view template_id);
    void close_project();
    [[nodiscard]] bool has_project() const noexcept { return project_.has_value(); }
    [[nodiscard]] project::Project* project() noexcept { return project_ ? &*project_ : nullptr; }
    [[nodiscard]] assets::AssetDatabase* assets() noexcept { return db_.get(); }
    [[nodiscard]] Status reimport_assets();
    // Hot reload: re-imports textures/models changed on disk and swaps them in the viewport and
    // a running play session, reloads changed audio clips, and notifies about files that need a
    // restart. Polled about once a second from frame(); returns the number of assets reloaded.
    std::size_t check_asset_changes();
    // Viewport/play rendering quality. Changing it updates the project's `rendering.quality` and
    // saves the project file, so `victus run` and exported games use the same tier.
    [[nodiscard]] render::QualityTier quality() const noexcept { return quality_; }
    [[nodiscard]] Status set_quality(render::QualityTier tier);
    [[nodiscard]] const render::SceneStats& render_stats() const;
    // Project input actions (Edit > Input Actions). set_input_map validates the map (syntax,
    // duplicate names, unknown keys), saves `victus.project` and applies it to a running play
    // session; on error nothing changes.
    [[nodiscard]] const gameplay::InputMap* input_map() const noexcept;
    [[nodiscard]] Status set_input_map(gameplay::InputMap map);
    void set_show_input_actions(bool show) noexcept { show_input_actions_ = show; }
    [[nodiscard]] bool show_input_actions() const noexcept { return show_input_actions_; }
    // "Press a control to bind": the next key (except Escape, which cancels), right/middle mouse
    // button, gamepad button or stick/trigger push is added to `action` (its negative side when
    // `negative`). The editor polls its own input each frame; capture_from() feeds a source directly.
    void begin_binding_capture(std::string action, bool negative = false);
    void cancel_binding_capture() noexcept { capture_action_.clear(); }
    [[nodiscard]] bool capturing_binding() const noexcept { return !capture_action_.empty(); }
    // Returns true when a control was captured (or the capture was cancelled with Escape).
    bool capture_from(const gameplay::InputSource& input);

    // ---- scenes ----
    [[nodiscard]] Status open_scene(std::string_view relative_path);
    [[nodiscard]] Status save_scene();
    [[nodiscard]] Status save_scene_as(std::string_view relative_path);
    void new_scene();
    [[nodiscard]] const std::string& scene_path() const noexcept { return scene_path_; }
    [[nodiscard]] bool dirty() const noexcept { return undo_.dirty(); }
    // Scene being edited, or the play copy while playing.
    // Script host of the current play session (null while editing).
    [[nodiscard]] const script::ScriptHost* play_scripts() const noexcept { return play_scripts_.get(); }
    [[nodiscard]] const physics::PhysicsWorld* play_physics() const noexcept { return play_physics_.get(); }
    [[nodiscard]] const navigation::NavigationSystem* play_navigation() const noexcept { return play_navigation_.get(); }
    // View > Navigation Mesh: draws the navmesh (built from static colliders) in the viewport.
    void set_show_navmesh(bool show) noexcept { show_navmesh_ = show; }
    [[nodiscard]] const navigation::NavigationSystem* navmesh_preview() const noexcept { return edit_navigation_.get(); }
    // Audio engine for play sessions and clip preview (null if no engine could be created).
    audio::AudioEngine* audio();
    // Shared worker threads (physics), created on first use.
    jobs::JobSystem& workers();
    // The scene panels show and edit: the authored scene, or while playing the viewed peer's world.
    [[nodiscard]] scene::Scene& active_scene() noexcept;
    [[nodiscard]] scene::Scene& edit_scene() noexcept { return *scene_; }
    // The host's (or offline) play-mode world; null while editing.
    [[nodiscard]] scene::Scene* play_scene() noexcept { return play_scene_.get(); }

    // ---- editing (all undoable while editing; direct while playing) ----
    scene::Entity create_entity(Primitive kind, scene::Entity parent = scene::kNullEntity);
    void delete_selected();
    void duplicate_selected();
    bool reparent(scene::Entity e, scene::Entity new_parent, scene::Entity before = scene::kNullEntity);
    void rename(scene::Entity e, std::string name);
    // Adds a model asset (glTF) as a new entity tree.
    scene::Entity instantiate_asset(const Uuid& asset);
    // ---- prefabs (undoable while editing; the prefab files themselves are not part of undo) ----
    // Saves `e`'s subtree as a prefab asset (default "prefabs/<name>.vprefab", made unique) and
    // turns `e` into an instance of it. Returns the new asset's UUID.
    Result<Uuid> create_prefab(scene::Entity e, std::string_view relative_path = {});
    // Adds an instance of a prefab asset at the camera target (or under `parent`).
    scene::Entity instantiate_prefab(const Uuid& asset, scene::Entity parent = scene::kNullEntity);
    // ---- materials (.vmat assets; file edits are saved directly and are not scene undo) ----
    // Writes a new material (default "materials/Material.vmat", made unique). Returns its UUID.
    Result<Uuid> create_material(std::string_view relative_path = {}, const assets::MaterialAsset& material = {});
    // Overwrites a material file and refreshes every renderer that uses it.
    Status save_material(const Uuid& asset, const assets::MaterialAsset& material);
    // Sets (or clears, with a nil UUID) the material of `e`'s MeshRenderer. Undoable.
    bool assign_material(scene::Entity e, const Uuid& material);
    // Creates a material from the selected mesh renderer's look, assigns it and opens the editor.
    void new_material_for_selection();
    // Writes a material that inherits every field from `parent` (default "<parent>_instance.vmat"
    // next to it, made unique). Returns its UUID.
    Result<Uuid> create_material_instance(const Uuid& parent, std::string_view relative_path = {});
    // ---- sprite sheets (.vsheet) ----
    // Writes a sheet slicing `texture` into columns x rows frames with a looping "default" clip of
    // every frame at `fps` (default "<texture>.vsheet" next to it, made unique). Returns its UUID.
    Result<Uuid> create_sprite_sheet(const Uuid& texture, std::uint32_t columns, std::uint32_t rows, float fps = 10.0f,
                                     std::string_view relative_path = {});
    // Writes a texture's import settings (.meta), re-imports it and refreshes the GPU cache.
    Status set_texture_import_settings(const Uuid& texture, const assets::TextureImportSettings& settings);
    // Gives `e` a SpriteAnimator playing `sheet`'s first clip (adding a Sprite when missing). Undoable.
    bool assign_sprite_sheet(scene::Entity e, const Uuid& sheet);
    // ---- tilemaps (.vtileset; painting is undoable, one entry per stroke) ----
    // Writes a tile set slicing `texture` into columns x rows tiles, all with `collision` (default
    // "<texture>.vtileset" next to it, made unique). Returns its UUID.
    Result<Uuid> create_tileset(const Uuid& texture, std::uint32_t columns, std::uint32_t rows,
                                tilemap::TileCollision collision = tilemap::TileCollision::Solid,
                                std::string_view relative_path = {});
    // Gives `e` a Tilemap using `tileset` (or sets its tile set) and selects the tile tool. Undoable.
    bool assign_tileset(scene::Entity e, const Uuid& tileset);
    enum class TileMode : std::uint8_t { Paint, Erase, Rectangle, Terrain, Pick };
    struct TileBrush {
        TileMode mode{TileMode::Paint};
        std::int32_t tile{0};
        std::int32_t terrain{0};
        bool flip_x{false}, flip_y{false};
    };
    [[nodiscard]] TileBrush& tile_brush() noexcept { return tile_brush_; }
    // While on (and the selected entity has a Tilemap in a 2D scene) left-dragging in the viewport
    // paints with the brush instead of selecting.
    void set_tile_tool(bool on) noexcept { tile_tool_ = on; }
    [[nodiscard]] bool tile_tool() const noexcept { return tile_tool_; }
    // The cell of `map` under a viewport pixel (2D view), or nullopt.
    [[nodiscard]] std::optional<std::array<std::int32_t, 2>> tile_under(scene::Entity map, math::Vec2 pixel,
                                                                        math::Vec2 viewport_size);
    // A brush stroke on `map`: begin, then continue over cells (gaps are filled with a line; the
    // rectangle mode fills from the first to the last cell on end), end records one undo entry.
    bool begin_tile_stroke(scene::Entity map, std::int32_t x, std::int32_t y);
    void continue_tile_stroke(std::int32_t x, std::int32_t y);
    void end_tile_stroke();
    [[nodiscard]] bool tile_stroke_active() const noexcept { return tile_stroke_.active; }
    // Writes a new generated heightfield (default "terrain/Terrain.vterrain", made unique; 129
    // samples per side) and creates an entity with a Terrain component using it (undoable).
    Result<scene::Entity> create_terrain(std::string_view relative_path = {}, std::uint32_t resolution = 129);
    // Writes a new surface shader from the template (default "shaders/Surface.vshader", made
    // unique). Returns its UUID.
    Result<Uuid> create_surface_shader(std::string_view relative_path = {});
    // Sets (or clears) the surface shader of the material open in the Material Editor and saves it.
    bool set_editing_material_shader(const Uuid& shader);
    void open_material_editor(const Uuid& asset);
    [[nodiscard]] bool material_editor_open() const noexcept { return show_material_editor_; }
    [[nodiscard]] const Uuid& editing_material() const noexcept { return material_editing_; }

    // ---- visual script graphs (.vgraph assets, ADR-0058/ADR-0059; graph_panel.cpp) ----
    // The Graph Editor edits one graph file. Graph edits have their own undo history (the file is a
    // project asset, not scene data) and are saved explicitly; play mode hot-reloads saved graphs.
    // Writes a starter graph (On Start -> Log; default "scripts/Graph.vgraph", made unique).
    Result<Uuid> create_graph(std::string_view relative_path = {});
    // Writes a starter material graph (ADR-0062; default "shaders/MaterialGraph.vmatgraph"). A
    // .vmatgraph is a surface shader asset: materials use it like a .vshader. It opens in the
    // Graph Editor with the material node library.
    Result<Uuid> create_material_graph(std::string_view relative_path = {});
    // Loads a graph asset into the Graph Editor and shows it (unsaved edits of the previous graph
    // are discarded).
    Status open_graph_editor(const Uuid& asset);
    [[nodiscard]] bool graph_editor_open() const noexcept { return show_graph_editor_; }
    [[nodiscard]] const Uuid& editing_graph_asset() const noexcept { return graph_asset_; }
    [[nodiscard]] const script::ScriptGraph& editing_graph() const noexcept { return graph_; }
    // Validation of the open graph, refreshed after every edit.
    [[nodiscard]] const std::vector<script::GraphDiagnostic>& graph_diagnostics() const noexcept { return graph_diags_; }
    [[nodiscard]] bool graph_dirty() const noexcept { return graph_dirty_; }
    // Edits of the open graph; each is one undo step. 0 / false when rejected (unknown node type,
    // missing node or pin, incompatible pin types, duplicate variable name).
    std::uint32_t graph_add_node(std::string_view type, float x = 0.0f, float y = 0.0f);
    bool graph_remove_node(std::uint32_t id);
    bool graph_move_node(std::uint32_t id, float x, float y);
    bool graph_connect(std::uint32_t from, std::string_view from_pin, std::uint32_t to, std::string_view to_pin);
    bool graph_disconnect(std::uint32_t node, std::string_view input_pin);
    bool graph_set_literal(std::uint32_t node, std::string_view input_pin, json::Value value);
    bool graph_set_config(std::uint32_t node, std::string_view key, json::Value value);
    bool graph_add_variable(script::GraphVariable variable);
    // Replaces a variable; a rename also retargets every Get/Set node that used the old name.
    bool graph_update_variable(std::string_view name, script::GraphVariable variable);
    bool graph_remove_variable(std::string_view name);
    bool graph_undo();
    bool graph_redo();
    [[nodiscard]] bool graph_can_undo() const noexcept { return !graph_undo_.empty(); }
    [[nodiscard]] bool graph_can_redo() const noexcept { return !graph_redo_.empty(); }
    // Writes the open graph to its file and hot-reloads it (edit-time inspector and play mode).
    Status save_graph();
    // Points `e`'s Script component (added when missing) at the open graph. Undoable scene edit.
    bool assign_graph_to(scene::Entity e);
    // Graph debugging while playing (play mode traces graphs, ADR-0060): a node's highlight
    // (1 when it just ran, fading to 0), its run count this session, and the open graph's last
    // runtime error (kept after Stop until the next Play).
    [[nodiscard]] float graph_node_heat(std::uint32_t node) const;
    [[nodiscard]] std::uint64_t graph_node_runs(std::uint32_t node) const;
    [[nodiscard]] const std::optional<script::GraphRuntimeError>& graph_runtime_error() const noexcept { return graph_error_; }
    // Writes the instance's current content to its prefab and updates every instance in the scene.
    Status apply_prefab(scene::Entity instance);
    // Discards the instance's overrides and brings it back in line with its prefab.
    Status revert_prefab(scene::Entity instance);
    // Turns the instance into ordinary entities (no link to the prefab any more).
    void unpack_prefab(scene::Entity instance);
    // Updates every instance of `asset` in the edited scene from the prefab file; returns how many.
    Result<std::size_t> update_prefab_instances(const Uuid& asset);
    // The prefab file of an asset UUID.
    [[nodiscard]] Result<scene::Prefab> load_prefab(const Uuid& asset) const;
    // Records an edit made directly to `entity` (before = snapshot captured earlier).
    void record_edit(std::string label, std::vector<EntityState> before);
    [[nodiscard]] UndoStack& undo_stack() noexcept { return undo_; }
    void undo();
    void redo();

    // ---- selection ----
    void select(scene::Entity e, bool additive = false);
    void clear_selection() { selection_.clear(); }
    [[nodiscard]] scene::Entity selected() noexcept;
    [[nodiscard]] const std::vector<Uuid>& selection() const noexcept { return selection_; }
    [[nodiscard]] bool is_selected(scene::Entity e) const;
    void frame_selected();
    // Selects from the primary selection to `to` in hierarchy order (Shift+click); `to` becomes primary.
    void select_range(scene::Entity to);
    // Selects pickable entities whose bounds centre projects inside the viewport-pixel rectangle
    // a..b (prefab parts select their instance). Returns how many were added.
    std::size_t box_select(math::Vec2 a, math::Vec2 b, math::Vec2 viewport_size, bool additive = false);
    // Applies the change made to `primary` since snapshot `before` (Scene::save_entity) to the
    // other selected entities, field by field (the inspector calls this while several are selected).
    void apply_multi_edit(scene::Entity primary, const json::Value& before);
    // Every entity in hierarchy (depth-first) order.
    [[nodiscard]] std::vector<scene::Entity> hierarchy_order();

    // ---- play mode ----
    void play();
    void pause();
    void stop();
    [[nodiscard]] PlayState play_state() const noexcept { return play_state_; }

    // ---- network play (Play > Play as Host, Window > Network) ----
    // Validated (link conditions parse, a password needs encryption); applies to the next Play.
    [[nodiscard]] Status set_net_settings(NetPlaySettings settings);
    [[nodiscard]] const NetPlaySettings& net_settings() const noexcept { return net_settings_; }
    // Starts play mode as a listen server (the settings' mode becomes Host).
    void play_host();
    // The running session's server (null when offline or editing).
    [[nodiscard]] const net::NetServer* play_server() const noexcept { return play_server_.get(); }
    // Public key players pin with --server-key (hex; empty for insecure or before hosting).
    [[nodiscard]] const std::string& server_key() const noexcept { return server_key_; }
    // Join command for other machines/terminals, e.g. "victus run <project> --connect 127.0.0.1:27960 ...".
    [[nodiscard]] std::string join_command() const;
    // Window > Profiler (ADR-0061): rolling frame-time graph with p50/p95/p99, per-scope CPU
    // timings (systems, scripts, render passes), GPU wait, memory and GPU resource counters.
    void set_show_profiler(bool show) noexcept {
        show_profiler_ = show;
        profiler_focus_ = show;
    }
    [[nodiscard]] bool show_profiler() const noexcept { return show_profiler_; }
    [[nodiscard]] profiler::FrameReport profiler_report() const;
    void set_profiler_paused(bool paused) noexcept;
    [[nodiscard]] bool profiler_paused() const noexcept;
    // Keeps every zone of the next `frames` editor frames and writes them as a Chrome/Perfetto
    // trace to <project>/.victus/profiles/trace-<time>.json; last_profile_trace() is its path.
    void record_profile_trace(std::size_t frames = 300);
    [[nodiscard]] bool recording_profile_trace() const noexcept;
    [[nodiscard]] const std::filesystem::path& last_profile_trace() const noexcept { return last_profile_trace_; }
    // ---- autosave & crash recovery ----
    // Unsaved edits of the open scene are written every autosave_interval() seconds to
    // <project>/.victus/autosave/; saving or discarding removes the copy. Opening a scene whose
    // autosave is newer than the file offers recovery (a crash or kill left it behind).
    void set_autosave_interval(float seconds) noexcept { autosave_seconds_ = std::max(seconds, 0.0f); }
    [[nodiscard]] float autosave_interval() const noexcept { return autosave_seconds_; }
    [[nodiscard]] Status autosave_now();
    [[nodiscard]] fs::Path autosave_path(std::string_view scene_path) const;
    [[nodiscard]] bool recovery_available() const noexcept { return recovery_.has_value(); }
    [[nodiscard]] std::string recovery_scene() const { return recovery_ ? recovery_->scene : std::string(); }
    // Replaces the open scene with the autosaved copy (left unsaved so the user can review it).
    [[nodiscard]] Status recover_autosave();
    void discard_autosave();

    // ---- command palette ----
    [[nodiscard]] std::vector<EditorCommand> commands();
    // Enabled commands matching `query`, best first (all of them, alphabetically, for an empty query).
    [[nodiscard]] std::vector<EditorCommand> find_commands(std::string_view query, std::size_t limit = 50);
    // Runs the best-matching enabled command whose name equals `name` (case-insensitive).
    bool run_command(std::string_view name);
    void open_command_palette() noexcept {
        palette_open_ = true;
        palette_focus_ = true;
        palette_query_.clear();
        palette_index_ = 0;
    }
    [[nodiscard]] bool command_palette_open() const noexcept { return palette_open_; }
    void set_show_network(bool show) noexcept { show_network_ = show; }
    [[nodiscard]] bool show_network() const noexcept { return show_network_; }
    // In-editor clients of the hosted session (NetPlaySettings::editor_clients), 1-based.
    [[nodiscard]] std::size_t editor_client_count() const noexcept;
    [[nodiscard]] const net::NetClient* editor_client(std::size_t index) const noexcept;
    [[nodiscard]] scene::Scene* editor_client_scene(std::size_t index) noexcept;
    // Which peer the viewport, hierarchy and inspector show and the keyboard drives: 0 = host,
    // 1..n = in-editor client. Out-of-range values are ignored.
    void set_view_peer(std::size_t peer);
    [[nodiscard]] std::size_t view_peer() const noexcept { return view_peer_; }
    [[nodiscard]] std::string peer_label(std::size_t peer) const;
    // Game input for automation and tests: replaces the platform keyboard/gamepad as the input
    // of the viewed peer (null restores the platform input). Not owned.
    void set_game_input_override(gameplay::InputSource* input);

    // ---- regression captures (Play > Record / Replay) ----
    // Starts play mode while recording input, frame times and state digests with a fresh script
    // seed. Stop saves the capture to <project>/.victus/captures/<scene>-<time>.vcapture.
    void record();
    [[nodiscard]] bool recording() const noexcept { return recording_ != nullptr; }
    // Starts play mode driven by a capture: its seed, frame times and input replace live ones.
    // The session pauses when the capture ends or the first digest differs; replay_result()
    // then reports "N digests match" or the frame and entities that diverged.
    [[nodiscard]] Status replay(const std::filesystem::path& capture);
    [[nodiscard]] bool replaying() const noexcept { return replay_ != nullptr && !replay_->done() && !replay_->diverged(); }
    [[nodiscard]] const std::string& replay_result() const noexcept { return replay_result_; }
    [[nodiscard]] bool replay_diverged() const noexcept { return replay_diverged_; }
    // Captures saved for the open project, newest first.
    [[nodiscard]] std::vector<std::filesystem::path> captures() const;
    [[nodiscard]] const std::filesystem::path& last_capture() const noexcept { return last_capture_; }

    // ---- viewport ----
    [[nodiscard]] EditorCamera& camera() noexcept { return camera_; }
    // Ray pick in viewport pixel coordinates (origin top-left). Returns the closest entity.
    [[nodiscard]] scene::Entity pick(math::Vec2 pixel, math::Vec2 viewport_size);
    [[nodiscard]] rhi::TextureHandle viewport_texture() const noexcept { return viewport_tex_; }
    // Forces the viewport size (headless tests); 0 = follow the panel.
    void set_viewport_size_override(std::uint32_t w, std::uint32_t h) { forced_w_ = w; forced_h_ = h; }

    // ---- transform gizmo (viewport; W/E/R pick move/rotate/scale, T toggles world/local) ----
    // The gizmo sits on the active (last selected) entity and moves every selected entity whose
    // ancestors are not selected too; rotate and scale pivot about the active entity. A drag is
    // one undo entry ("Move", "Rotate" or "Scale") and marks prefab overrides. Snapping rounds
    // the drag to the steps; holding Ctrl while dragging inverts the snap toggle.
    struct GizmoSettings {
        gizmo::Mode mode{gizmo::Mode::Translate};
        gizmo::Space space{gizmo::Space::World};
        bool snap{false};
        gizmo::Snap steps;
        bool visible{true};
    };
    [[nodiscard]] GizmoSettings& gizmo_settings() noexcept { return gizmo_; }

    // ---- asset thumbnails (Assets panel) ----
    // nullptr while no project is open (or when the preview renderer could not be created).
    [[nodiscard]] ThumbnailCache* thumbnails() noexcept { return thumbnails_.get(); }
    // Thumbnail of an asset for UI code; queues a preview render when needed.
    [[nodiscard]] std::optional<Thumbnail> asset_thumbnail(const Uuid& asset);
    // Assets panel layout: list rows with small thumbnails, or a grid of `size`-pixel tiles.
    void set_asset_view(bool grid, float size) noexcept;
    [[nodiscard]] bool asset_grid() const noexcept { return asset_grid_; }
    [[nodiscard]] float asset_tile_size() const noexcept { return asset_tile_size_; }
    // Preview renders allowed per editor frame (0 pauses thumbnail rendering).
    void set_thumbnail_budget(std::uint32_t per_frame) noexcept { thumbnail_budget_ = per_frame; }
    // Frame of the gizmo for the current selection (nullopt: nothing selected or not editing).
    [[nodiscard]] std::optional<gizmo::Frame> gizmo_frame();
    // The camera of the viewport as the gizmo sees it (editor camera, 2D or 3D).
    [[nodiscard]] gizmo::View gizmo_view(math::Vec2 viewport_size);
    // Pointer API in viewport pixels (origin top-left). press() starts a drag when a handle is
    // under the pointer and returns true; drag() applies the transform live; release() records
    // the undo entry; cancel() restores the transforms from before the drag.
    [[nodiscard]] gizmo::Handle gizmo_hover(math::Vec2 pixel, math::Vec2 viewport_size);
    bool gizmo_press(math::Vec2 pixel, math::Vec2 viewport_size);
    void gizmo_drag(math::Vec2 pixel, math::Vec2 viewport_size, bool invert_snap = false);
    void gizmo_release();
    void gizmo_cancel();
    [[nodiscard]] bool gizmo_dragging() const noexcept { return gizmo_active_.has_value(); }

    [[nodiscard]] const std::vector<std::string>& messages() const noexcept { return status_messages_; }
    void notify(std::string message, bool error = false);

private:
    Editor(render::GpuContext& gpu, ui::UiContext& ui, platform::Platform* platform, platform::Window* window);

    // UI (panels.cpp / launcher.cpp)
    void draw_launcher();
    void draw_workspace(float dt);
    void draw_menu_bar();
    void draw_toolbar();
    void draw_hierarchy();
    void draw_hierarchy_node(scene::Entity e);
    void draw_inspector();
    void draw_assets();
    void draw_console();
    void draw_viewport();
    void draw_status_bar();
    void draw_modals();
    void handle_shortcuts();
    void viewport_input(math::Vec2 size, bool hovered);
    void build_default_layout(unsigned int dockspace_id);
    // Inspector helpers
    bool inspect_components(scene::Entity e);
    void begin_inspector_frame(scene::Entity e);
    void end_inspector_frame(scene::Entity e, bool changed);

    void set_scene(std::unique_ptr<scene::Scene> scene, std::string path);
    void update_title();
    [[nodiscard]] Status ensure_viewport(std::uint32_t w, std::uint32_t h);
    [[nodiscard]] math::Aabb entity_bounds(scene::Entity e, bool* has_mesh);
    void draw_overlays_3d(render::SceneRenderer& r);
    void draw_overlays_2d(render::SpriteBatch& b);
    void prune_selection();
    void reset_simulation();
    bool inspect_script(script::ScriptComponent& c);

    render::GpuContext& gpu_;
    ui::UiContext& ui_;
    platform::Platform* platform_;
    platform::Window* window_;
    EditorOptions options_;

    std::optional<project::Project> project_;
    std::unique_ptr<assets::AssetDatabase> db_;
    std::unique_ptr<runtime::GpuAssetCache> cache_;
    std::unique_ptr<ThumbnailCache> thumbnails_; // destroyed before cache_ (borrows its textures)
    bool asset_grid_{false};
    float asset_tile_size_{72.0f};
    std::uint32_t thumbnail_budget_{2};
    void draw_asset_item(const assets::AssetRecord& r); // context menu, drag source, double-click, tooltip
    std::unique_ptr<runtime::AssetHotReloader> hot_reload_; // watches assets/ while a project is open
    std::unique_ptr<runtime::SceneView> view_;
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<scene::Scene> play_scene_;
    std::string scene_path_;
    UndoStack undo_;
    std::unique_ptr<jobs::JobSystem> jobs_; // created on first use; outlives play_physics_
    gameplay::Simulation sim_;
    std::unique_ptr<script::ScriptHost> play_scripts_; // fresh per play session, hot reloads files
    std::unique_ptr<physics::PhysicsWorld> play_physics_; // fresh per play session
    std::unique_ptr<animation::AnimationSystem> play_animation_; // fresh per play session
    std::unique_ptr<animation::AnimationSystem> edit_sprites_;   // previews SpriteAnimator frames while editing
    std::unique_ptr<tilemap::TilemapSystem> tilemaps_;           // tile sets + tile colliders (edit and play)
    std::unique_ptr<terrain::TerrainSystem> terrains_;           // terrain chunks + colliders (edit and play)
    // "Create Sprite Sheet" popup of the asset table.
    Uuid sheet_texture_;
    Uuid import_texture_; // "Texture Import Settings" popup target
    assets::TextureImportSettings import_edit_;
    int sheet_columns_{4}, sheet_rows_{1};
    float sheet_fps_{10.0f};
    std::unique_ptr<particles::ParticleSystem> play_particles_;  // fresh per play session
    std::unique_ptr<navigation::NavigationSystem> play_navigation_; // fresh per play session
    std::unique_ptr<audio::AudioEngine> audio_;           // opened on first use, kept across sessions
    bool audio_failed_{false};
    std::unique_ptr<script::ScriptHost> edit_scripts_; // inspector property discovery
    std::unique_ptr<player::PlatformInput> input_;     // game input, enabled while the viewport has focus
    float script_poll_{0};
    bool show_colliders_{true};
    bool show_navmesh_{false};
    std::unique_ptr<navigation::NavigationSystem> edit_navigation_; // navmesh preview while editing
    std::vector<navigation::DebugLine> nav_scratch_;
    render::QualityTier quality_{render::QualityTier::High};
    void apply_quality();
    std::vector<physics::DebugLine> collider_scratch_;
    bool focus_viewport_{false};
    PlayState play_state_{PlayState::Editing};
    void start_play(std::uint64_t seed, bool networked = false);
    [[nodiscard]] Status start_hosting();
    void draw_network();
    void draw_profiler();
    void draw_command_palette();
    void update_autosave(float dt);
    void check_recovery();
    void remove_autosave(std::string_view scene_path);
    void draw_recovery_modal();
    void update_profiler();
    void draw_network_section(scene::Entity e);
    NetPlaySettings net_settings_;
    std::unique_ptr<net::NetServer> play_server_; // listen server of a hosted play session
    std::vector<std::unique_ptr<EditorClient>> play_clients_; // in-editor players (editor_clients.cpp)
    std::size_t view_peer_{0};
    gameplay::InputSource* game_input_override_{nullptr};
    gameplay::ManualInput host_idle_input_; // the host's input while a client is viewed
    void start_editor_clients();
    void tick_editor_clients(float dt);
    void stop_editor_clients();
    void route_input();
    std::string server_key_;
    bool show_network_{false};
    bool show_profiler_{false};
    struct Recovery {
        std::string scene; // "" = untitled
        fs::Path file;
        std::int64_t age_s{0};
    };
    std::optional<Recovery> recovery_;
    float autosave_seconds_{30.0f};
    float autosave_timer_{0.0f};
    std::uint64_t autosaved_revision_{0};
    bool autosave_written_{false}; // this session wrote the autosave of the open scene
    std::string autosave_scene_;   // the scene path that autosave was written for
    bool box_selecting_{false};
    bool palette_open_{false};
    bool palette_focus_{false};
    std::string palette_query_;
    int palette_index_{0};
    bool profiler_focus_{false};
    std::unique_ptr<profiler::FrameProfiler> frame_profiler_;
    std::filesystem::path last_profile_trace_;
    std::string net_filter_;
    std::string net_error_;
    float net_sample_{0};
    std::vector<float> net_send_history_, net_receive_history_; // kbps, 4 samples per second
    void tick_play(float dt);
    std::unique_ptr<gameplay::Capture> recording_;        // while recording
    std::unique_ptr<gameplay::Capture> replay_capture_;   // while replaying
    std::unique_ptr<gameplay::CaptureReplay> replay_;
    gameplay::ReplayInput captured_input_;
    std::string replay_result_;
    bool replay_diverged_{false};
    std::filesystem::path last_capture_;
    std::vector<Uuid> selection_;
    EditorCamera camera_;
    render::Camera2D last_cam2d_;
    std::unique_ptr<project::RecentProjects> recent_;
    std::shared_ptr<log::RingSink> log_sink_;

    // viewport
    rhi::TextureHandle viewport_tex_;
    std::uint32_t viewport_w_{0}, viewport_h_{0}, forced_w_{0}, forced_h_{0};
    bool viewport_focused_{false};
    math::Vec2 drag_origin_;
    bool lmb_dragged_{false};

    // inspector edit tracking
    std::vector<EntityState> pending_before_;
    Uuid pending_entity_;
    bool pending_dirty_{false};
    struct EulerCache {
        Uuid entity;
        math::Quat rotation;
        math::Vec3 degrees;
    } euler_cache_;

    // launcher state
    std::string new_name_{"My Game"};
    std::string new_location_;
    std::string new_template_{"3d"};
    std::string open_path_;
    std::string launcher_error_;

    // modals
    enum class Pending : std::uint8_t { None, Quit, CloseProject, OpenRecent };
    Pending pending_action_{Pending::None};
    fs::Path pending_path_;
    bool quit_confirmed_{false};
    bool show_save_as_{false};
    std::string save_as_path_;
    bool layout_built_{false};
    bool show_demo_{false};
    bool show_input_actions_{false};
    std::string capture_action_;
    bool capture_negative_{false};
    std::string input_error_;
    std::unordered_map<std::string, std::string> binding_text_; // typed bindings per action side
    void draw_input_actions();
    void draw_prefab_section(scene::Entity e);
    // Marks components of `e` that differ from `before` as prefab overrides (instance parts only).
    void mark_prefab_overrides(scene::Entity e, const std::vector<EntityState>& before);
    std::unordered_map<std::string, std::uint64_t> written_prefabs_; // path -> hash of our last write
    void draw_material_editor();
    void draw_tile_palette();
    void draw_tile_overlay(render::SpriteBatch& b, float px);
    void tile_viewport_input(math::Vec2 origin, math::Vec2 size, bool hovered);
    [[nodiscard]] scene::Entity tile_target();
    void apply_tile_brush(std::int32_t x, std::int32_t y);
    struct TileStroke {
        bool active{false};
        Uuid map;
        std::int32_t x0{0}, y0{0}, x{0}, y{0};
        std::vector<EntityState> before;
        std::uint64_t revision{0};
    };
    TileBrush tile_brush_;
    TileStroke tile_stroke_;
    bool tile_tool_{false};
    bool show_tile_palette_{true};
    std::optional<std::array<std::int32_t, 2>> tile_hover_;
    Uuid tileset_texture_; // "Create Tileset" popup target
    int tileset_columns_{8}, tileset_rows_{8}, tileset_collision_{1};
    bool show_material_editor_{false};
    bool material_dirty_{false};
    Uuid material_editing_;
    assets::MaterialAsset material_edit_;
    std::unordered_map<std::string, std::uint64_t> written_materials_; // path -> hash of our last write
    // visual script graph editor (graph_panel.cpp)
    void draw_graph_editor();
    void draw_graph_side_panel();
    void draw_graph_canvas();
    // Reloads the open graph when its file changed on disk and there are no unsaved edits.
    void sync_graph_from_disk();
    // Follows the play session's graph trace: highlights and the last runtime error.
    void update_graph_debug(float dt);
    std::unordered_map<std::uint32_t, float> graph_heat_;
    std::unordered_map<std::uint32_t, std::uint64_t> graph_hits_seen_;
    std::optional<script::GraphRuntimeError> graph_error_;
    bool graph_live_{false};
    bool graph_show_code_{false}; // material graphs: show the generated GLSL
    Result<Uuid> create_graph_file(std::string_view relative_path, script::GraphDomain domain);
    // Pushes an undo snapshot; consecutive edits with the same non-empty key share one step.
    void graph_begin_edit(std::string_view coalesce_key = {});
    void graph_end_edit();
    bool show_graph_editor_{false};
    bool graph_dirty_{false};
    bool graph_focused_{false};
    Uuid graph_asset_;
    std::string graph_path_;
    std::uint64_t graph_disk_hash_{0};
    script::ScriptGraph graph_;
    std::vector<script::GraphDiagnostic> graph_diags_;
    std::vector<std::string> graph_undo_, graph_redo_;
    std::string graph_edit_key_;
    std::uint32_t graph_selected_{0};
    math::Vec2 graph_scroll_{40.0f, 40.0f};
    struct GraphPinDrag {
        std::uint32_t node{0};
        std::string pin;
        bool output{false};
    };
    std::optional<GraphPinDrag> graph_link_drag_;
    std::optional<GraphPinDrag> graph_palette_link_; // link to complete with the node picked from the palette
    bool graph_node_drag_{false};
    bool graph_node_moved_{false};
    math::Vec2 graph_palette_pos_{};
    std::string graph_palette_search_;
    std::string rename_buffer_;
    Uuid renaming_;

    // transform gizmo (gizmo_tool.cpp)
    struct GizmoTarget {
        Uuid id;
        scene::Transform local;  // local transform when the drag began
        math::Vec3 position;     // world TRS when the drag began
        math::Quat rotation;
        math::Vec3 scale;
    };
    GizmoSettings gizmo_;
    std::optional<gizmo::Drag> gizmo_active_;
    std::vector<GizmoTarget> gizmo_targets_;
    std::vector<EntityState> gizmo_before_;
    gizmo::Handle gizmo_hot_{gizmo::Handle::None};
    bool gizmo_consumed_click_{false}; // the current left click started a gizmo drag
    gizmo::DrawList gizmo_draw_;
    void apply_gizmo_delta(const gizmo::Delta& d);
    void gizmo_viewport(math::Vec2 origin, math::Vec2 size, bool hovered, bool painting);
    void draw_gizmo_toolbar(math::Vec2 origin, math::Vec2 size);
    void gizmo_shortcuts();

    std::vector<std::string> status_messages_;
    std::string status_line_;
    double status_time_{0};
    float fps_{0};
    runtime::SceneViewStats last_view_stats_;
};

} // namespace victus::editor
