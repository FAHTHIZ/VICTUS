// SPDX-License-Identifier: Apache-2.0
// The VICTUS editor: project launcher, scene editing workspace (viewport, hierarchy, inspector,
// assets, console), undo/redo and isolated play mode. The class is UI-toolkit driven but every
// operation is also a plain method so tests and tools can drive the editor without input.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
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
#include "victus/script/script.hpp"
#include "victus/ui/ui.hpp"

#include <memory>
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

enum class Primitive : std::uint8_t { Empty, Cube, Sphere, Plane, Cylinder, Camera, DirectionalLight, PointLight, Sprite };

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
    // Writes a new surface shader from the template (default "shaders/Surface.vshader", made
    // unique). Returns its UUID.
    Result<Uuid> create_surface_shader(std::string_view relative_path = {});
    // Sets (or clears) the surface shader of the material open in the Material Editor and saves it.
    bool set_editing_material_shader(const Uuid& shader);
    void open_material_editor(const Uuid& asset);
    [[nodiscard]] bool material_editor_open() const noexcept { return show_material_editor_; }
    [[nodiscard]] const Uuid& editing_material() const noexcept { return material_editing_; }
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
    bool show_material_editor_{false};
    bool material_dirty_{false};
    Uuid material_editing_;
    assets::MaterialAsset material_edit_;
    std::unordered_map<std::string, std::uint64_t> written_materials_; // path -> hash of our last write
    std::string rename_buffer_;
    Uuid renaming_;

    std::vector<std::string> status_messages_;
    std::string status_line_;
    double status_time_{0};
    float fps_{0};
    runtime::SceneViewStats last_view_stats_;
};

} // namespace victus::editor
