// SPDX-License-Identifier: Apache-2.0
// The VICTUS editor: project launcher, scene editing workspace (viewport, hierarchy, inspector,
// assets, console), undo/redo and isolated play mode. The class is UI-toolkit driven but every
// operation is also a plain method so tests and tools can drive the editor without input.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/undo.hpp"
#include "victus/core/jobs.hpp"
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

enum class Primitive : std::uint8_t { Empty, Cube, Sphere, Plane, Cylinder, Camera, DirectionalLight, PointLight, Sprite };

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
    [[nodiscard]] scene::Scene& active_scene() noexcept { return play_scene_ ? *play_scene_ : *scene_; }
    [[nodiscard]] scene::Scene& edit_scene() noexcept { return *scene_; }

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
    Editor(render::GpuContext& gpu, ui::UiContext& ui, platform::Platform* platform, platform::Window* window)
        : gpu_(gpu), ui_(ui), platform_(platform), window_(window) {}

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
    std::string rename_buffer_;
    Uuid renaming_;

    std::vector<std::string> status_messages_;
    std::string status_line_;
    double status_time_{0};
    float fps_{0};
    runtime::SceneViewStats last_view_stats_;
};

} // namespace victus::editor
