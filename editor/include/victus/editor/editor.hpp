// SPDX-License-Identifier: Apache-2.0
// The VICTUS editor: project launcher, scene editing workspace (viewport, hierarchy, inspector,
// assets, console), undo/redo and isolated play mode. The class is UI-toolkit driven but every
// operation is also a plain method so tests and tools can drive the editor without input.
#pragma once
#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/undo.hpp"
#include "victus/core/jobs.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/physics/physics.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/scene_view.hpp"
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
    std::unique_ptr<runtime::SceneView> view_;
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<scene::Scene> play_scene_;
    std::string scene_path_;
    UndoStack undo_;
    std::unique_ptr<jobs::JobSystem> jobs_; // created on first use; outlives play_physics_
    gameplay::Simulation sim_;
    std::unique_ptr<script::ScriptHost> play_scripts_; // fresh per play session, hot reloads files
    std::unique_ptr<physics::PhysicsWorld> play_physics_; // fresh per play session
    std::unique_ptr<audio::AudioEngine> audio_;           // opened on first use, kept across sessions
    bool audio_failed_{false};
    std::unique_ptr<script::ScriptHost> edit_scripts_; // inspector property discovery
    std::unique_ptr<player::PlatformInput> input_;     // game input, enabled while the viewport has focus
    float script_poll_{0};
    bool show_colliders_{true};
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
    std::string rename_buffer_;
    Uuid renaming_;

    std::vector<std::string> status_messages_;
    std::string status_line_;
    double status_time_{0};
    float fps_{0};
    runtime::SceneViewStats last_view_stats_;
};

} // namespace victus::editor
