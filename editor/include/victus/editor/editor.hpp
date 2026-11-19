// SPDX-License-Identifier: Apache-2.0
// The VICTUS editor: project launcher, scene editing workspace (viewport, hierarchy, inspector,
// assets, console), undo/redo and isolated play mode. The class is UI-toolkit driven but every
// operation is also a plain method so tests and tools can drive the editor without input.
#pragma once
#include "victus/core/profiler.hpp"
#include "victus/animation/animation.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/gizmo.hpp"
#include "victus/editor/thumbnails.hpp"
#include "victus/editor/undo.hpp"
#include "victus/player/export.hpp"
#include "victus/player/validate.hpp"
#include "victus/player/diagnostics.hpp"
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
#include "victus/graph/graph.hpp"
#include "victus/script/script.hpp"
#include "victus/ui/ui.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <array>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    fs::Path samples_dir;          // sample projects for the launcher's Samples page; empty = none
    fs::Path player_executable;    // what Export Game ships; empty = victus_player next to the editor
    fs::Path diagnostics_dir;      // crash reports and session logs; empty = crash::default_directory()
    bool previous_session_crashed{false}; // show "closed unexpectedly last time" (crash::Session)
    // Studio preferences (interface size, Getting Started progress). Empty: studio_preferences.json
    // next to recent_file.
    fs::Path preferences_file;
    // Interface size used until the user picks one (View > Interface Size). The editor app passes
    // a larger size than the 100 % the tests use, so text and buttons are easy to read and hit.
    float default_ui_scale{1.0f};
    // Safe mode (ADR-0101): projects open without their startup scene, edit-time scripts, asset
    // import, thumbnails, the background check, autosave restore and play mode.
    bool safe_mode{false};             // for every project this run (victus_editor --safe-mode)
    std::uint32_t safe_mode_after{2};  // unfinished openings of a project that switch it on; 0 = never
    std::uint32_t startup_frames{120}; // frames after opening a project that count as a finished start
};

// <project>/.victus/editor_startup.json: how many times in a row the Studio started opening the
// project without reaching startup_frames (a crash, hang or kill). Missing or unreadable = 0.
[[nodiscard]] fs::Path startup_marker_path(const fs::Path& project_root);
[[nodiscard]] std::uint32_t unfinished_startups(const fs::Path& project_root);
// What safe mode skips, one plain sentence each (shown in the safe mode card and the guide).
[[nodiscard]] const std::vector<std::string>& safe_mode_skipped();

// Interface sizes offered in View > Interface Size (the scale is clamped to this range).
inline constexpr float kMinUiScale = 0.8f;
inline constexpr float kMaxUiScale = 2.0f;
struct UiScaleChoice {
    const char* name;
    float scale;
};
inline constexpr UiScaleChoice kUiScales[] = {{"Compact", 0.9f},     {"Normal", 1.0f}, {"Large", 1.15f},
                                              {"Extra Large", 1.3f}, {"Huge", 1.5f},   {"Presentation", 1.75f}};

// Getting Started checklist steps (Help > Getting Started). Each completes from real editor
// actions, never from opening the panel.
enum class GuideStep : std::uint8_t { AddObject, MoveObject, EditSetting, Play, Save, Export, Count };
[[nodiscard]] const char* guide_step_title(GuideStep step) noexcept;

// One built-in Help topic (Help > Help Topics): a plain-language how-to.
struct HelpTopic {
    const char* category;
    const char* title;
    const char* body;     // paragraphs separated by blank lines; lines starting with "- " are bullets
    const char* keywords; // extra search words
};
[[nodiscard]] std::span<const HelpTopic> help_topics() noexcept;
// Topics whose title, body or keywords contain every word of `query` (any case), title matches first.
[[nodiscard]] std::vector<const HelpTopic*> find_help_topics(std::string_view query);

// A sample project the launcher offers (a folder under EditorOptions::samples_dir).
struct SampleInfo {
    fs::Path dir;
    std::string name;    // from victus.project
    std::string summary; // first paragraph of its README.md
};

// One entry of Inspector > Add Component.
struct ComponentChoice {
    std::string id;          // serialized name, e.g. "RigidBody"
    std::string name;        // shown, e.g. "Rigid Body"
    std::string category;    // e.g. "Physics 3D"
    std::string description; // one plain-language line; may be empty
    bool present{false};     // the entity already has it
};

enum class LauncherPage : std::uint8_t { New, Recent, Samples, Learn };

// Every dockable Studio panel. The Window menu lists them all, so a closed panel is never lost.
enum class Panel : std::uint8_t {
    Viewport,
    Hierarchy,
    Inspector,
    Assets,
    Console,
    Problems,
    Animation,
    Sequencer,
    GraphEditor,
    MaterialEditor,
    TilePalette,
    TerrainTools,
    AiDebugger,
    Network,
    Profiler,
    InputActions,
    GettingStarted,
    Help,
    ProjectSettings,
    History,
    Count
};
[[nodiscard]] const char* panel_title(Panel panel) noexcept;

// Workspace presets: which panels are open and where they dock.
enum class Layout : std::uint8_t { Default, LevelDesign, Animation, Scripting, Debugging, Count };
[[nodiscard]] const char* layout_name(Layout layout) noexcept;

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

enum class Primitive : std::uint8_t { Empty, Cube, Sphere, Plane, Cylinder, Camera, DirectionalLight, PointLight, SpotLight, Sprite, Decal, Light2D, FogVolume };

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
    [[nodiscard]] Status create_project(const fs::Path& directory, std::string_view name, std::string_view template_id,
                                        const project::TemplateOptions& options = {});
    void close_project();
    [[nodiscard]] bool has_project() const noexcept { return project_.has_value(); }
    [[nodiscard]] project::Project* project() noexcept { return project_ ? &*project_ : nullptr; }
    [[nodiscard]] assets::AssetDatabase* assets() noexcept { return db_.get(); }
    [[nodiscard]] Status reimport_assets();
    // Copies files (or whole folders) from anywhere on disk into the project's assets and imports
    // them: what dropping files on the Studio window and Assets > Import do. Files go to the folder
    // selected in the Assets panel, else to a folder for their type (images -> textures/, models ->
    // models/, sounds -> audio/, scripts -> scripts/ ...); a glTF's .bin and textures come along;
    // names that exist get " 2", " 3"... Unknown types are skipped with a reason.
    struct ImportedFiles {
        std::vector<std::string> added;   // asset paths, e.g. "models/robot.glb"
        std::vector<std::string> skipped; // plain-language reasons, one per skipped item
    };
    ImportedFiles import_files(const std::vector<std::filesystem::path>& sources);
    void open_import_dialog(); // native multi-file picker, then import_files
    // Hot reload: re-imports textures/models changed on disk and swaps them in the viewport and
    // a running play session, reloads changed audio clips, and notifies about files that need a
    // restart. Polled about once a second from frame(); returns the number of assets reloaded.
    std::size_t check_asset_changes();
    // Viewport/play rendering quality. Changing it updates the project's `rendering.quality` and
    // saves the project file, so `victus run` and exported games use the same tier.
    [[nodiscard]] render::QualityTier quality() const noexcept { return quality_; }
    [[nodiscard]] Status set_quality(render::QualityTier tier);
    // Viewport debug view (View > Overdraw): editor-only, never saved to the project.
    [[nodiscard]] render::DebugView debug_view() const noexcept { return debug_view_; }
    void set_debug_view(render::DebugView view);
    // Layer counts of the last viewport frame drawn with the overdraw view (waits for the GPU).
    [[nodiscard]] Result<render::OverdrawStats> read_overdraw();
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
    [[nodiscard]] ai::AiSystem* play_ai() noexcept { return play_ai_.get(); }
    [[nodiscard]] sequencer::SequencerSystem* play_sequencer() noexcept { return play_sequencer_.get(); }
    // View > AI Debug: perception cones and targets in the viewport and the AI Debugger window
    // (behaviour tree state and blackboard of the selected entity) during play.
    void set_show_ai(bool show) noexcept { show_ai_ = show; }
    [[nodiscard]] bool show_ai() const noexcept { return show_ai_; }
    // Text form of the AI Debugger for an entity of the play scene (tree header, one line per node
    // with its last status, then "bb key = value" lines); empty while editing.
    [[nodiscard]] std::vector<std::string> ai_debug_lines(scene::Entity e);
    // View > Animation: the selected Animator's controller — state, cross-fade, clip weights and
    // parameter controls. In play it reads and drives the live session; while editing, a preview
    // runs the controller on one character in place and restores its pose when it ends (also on
    // save, play, autosave skip and scene change).
    void set_show_animation(bool show) noexcept { show_animation_ = show; }
    [[nodiscard]] bool show_animation() const noexcept { return show_animation_; }
    bool begin_animation_preview(scene::Entity e); // false without an Animator or while playing
    void end_animation_preview();
    [[nodiscard]] bool animation_previewing() const noexcept { return anim_backup_.has_value(); }
    [[nodiscard]] scene::Entity animation_preview_target();
    void set_animation_preview_paused(bool paused) noexcept { anim_preview_paused_ = paused; }
    // The system the Animation window reads: the play session's, else the edit preview's (or null).
    [[nodiscard]] animation::AnimationSystem* animation_inspected();
    bool set_animation_parameter(scene::Entity e, std::string_view name, float value);
    bool fire_animation_trigger(scene::Entity e, std::string_view name);
    bool enter_animation_state(scene::Entity e, std::string_view state);
    // Text form of the Animation window: "controller <path> state <S> time <t>", "fade from ...",
    // "weight <clip> <w>" and "param <name> <type> = <value>" lines; empty when nothing is inspected.
    [[nodiscard]] std::vector<std::string> animation_debug_lines(scene::Entity e);
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
    // Adds a "Foliage" child to `terrain` with a Foliage scatter on `layer` (-1: everywhere) and a
    // green sphere mesh renderer (undoable); swap the mesh for real grass/tree models.
    Result<scene::Entity> add_foliage(scene::Entity terrain, std::int32_t layer = 0);
    // Terrain sculpting and painting. While the tool is on (and the selected entity has a Terrain
    // in a 3D scene) left-dragging in the viewport applies the brush instead of selecting; Shift
    // inverts Raise/Lower, Ctrl-click in Flatten picks the target height. Each stroke is one undo
    // entry; edited heightfields are written back to their .vterrain on save_scene().
    void set_terrain_tool(bool on) noexcept { terrain_tool_ = on; }
    [[nodiscard]] bool terrain_tool() const noexcept { return terrain_tool_; }
    [[nodiscard]] terrain::Brush& terrain_brush() noexcept { return terrain_brush_; }
    // The terrain surface under a viewport pixel (3D view), or nullopt.
    [[nodiscard]] std::optional<terrain::TerrainHit> terrain_under(math::Vec2 pixel, math::Vec2 viewport_size);
    // A stroke on `terrain` at world points: begin, continue with `amount` (seconds of brushing;
    // long moves are split into dabs), end records the undo entry when anything changed.
    bool begin_terrain_stroke(scene::Entity terrain, math::Vec3 world_point, bool invert = false);
    void continue_terrain_stroke(math::Vec3 world_point, float amount);
    void end_terrain_stroke();
    [[nodiscard]] bool terrain_stroke_active() const noexcept { return terrain_stroke_.active; }
    // Whole-terrain operations (undoable): regenerate the heights, repaint the splat map.
    Status generate_terrain(scene::Entity terrain, const terrain::GenerateSettings& settings);
    Status auto_paint_terrain(scene::Entity terrain, const terrain::AutoPaintSettings& settings = {});
    // Heightfields edited since the last save.
    [[nodiscard]] std::size_t unsaved_terrains() const noexcept { return dirty_terrains_.size(); }
    // Writes a new surface shader from the template (default "shaders/Surface.vshader", made
    // unique). Returns its UUID.
    Result<Uuid> create_surface_shader(std::string_view relative_path = {});
    // Sets (or clears) the surface shader of the material open in the Material Editor and saves it.
    bool set_editing_material_shader(const Uuid& shader);
    void open_material_editor(const Uuid& asset);
    [[nodiscard]] bool material_editor_open() const noexcept { return show_material_editor_; }
    [[nodiscard]] const Uuid& editing_material() const noexcept { return material_editing_; }

    // ---- sequencer (.vseq timelines, docs/SEQUENCER.md; sequencer_panel.cpp) ----
    // The Sequencer panel edits one sequence file with its own undo history; edits are saved
    // explicitly. Scrubbing previews the sequence on a SequencePlayer entity of the edit scene; the
    // previewed transforms, camera flags and animators are put back when the preview ends (before
    // saving the scene, playing, switching scenes, or with end_sequence_preview()).
    enum class SeqChannel : std::uint8_t { Position, Rotation, Scale, Keys };
    struct SeqKey {
        int track{-1};
        SeqChannel channel{SeqChannel::Keys};
        int index{-1};
        [[nodiscard]] bool valid() const noexcept { return track >= 0 && index >= 0; }
        friend bool operator==(const SeqKey&, const SeqKey&) = default;
    };
    // Writes an empty sequence (default "sequences/Sequence.vseq", made unique) and returns its path.
    Result<std::string> create_sequence(std::string_view relative_path = {}, float duration = 5.0f);
    // Opens a sequence in the panel. `player` previews it (default: the selected entity when it
    // has a SequencePlayer, else the first SequencePlayer using `path`).
    Status open_sequence(std::string_view path, scene::Entity player = scene::kNullEntity);
    // Adds a SequencePlayer to `e` (or retargets its own) that plays the open sequence. Undoable.
    bool assign_sequence(scene::Entity e);
    void close_sequence();
    Status save_sequence();
    [[nodiscard]] const sequencer::Sequence* editing_sequence() const noexcept { return seq_doc_ ? &*seq_doc_ : nullptr; }
    [[nodiscard]] const std::string& editing_sequence_path() const noexcept { return seq_path_; }
    [[nodiscard]] bool sequence_dirty() const noexcept { return seq_dirty_; }
    [[nodiscard]] bool sequencer_open() const noexcept { return show_sequencer_; }
    void set_show_sequencer(bool show) noexcept { show_sequencer_ = show; }
    // Applies `edit` to the open sequence as one undo step (keys are re-sorted, the duration grows
    // to the last key) and refreshes the preview. False without an open sequence.
    bool edit_sequence(const std::function<void(sequencer::Sequence&)>& edit);
    // Appends a track; returns its index (-1 without an open sequence or at the track limit).
    int sequence_add_track(sequencer::TrackType type, std::string target = {}, std::string name = {});
    bool sequence_remove_track(int track);
    // Keys position, rotation and scale of the transform track's target at `time` from its
    // current local transform (relative tracks store the offset from the preview start).
    SeqKey sequence_record_transform(int track, float time);
    // Adds a key to a non-transform track: `value` is the clip (animation/audio), event name or
    // camera target. A key at the same time is replaced.
    SeqKey sequence_add_key(int track, float time, std::string value = {});
    // Moves a key to a new time (clamped to >= 0); returns its new position after re-sorting.
    SeqKey sequence_move_key(SeqKey key, float time);
    bool sequence_delete_key(SeqKey key);
    [[nodiscard]] std::optional<float> sequence_key_time(SeqKey key) const;
    bool sequence_undo();
    bool sequence_redo();
    [[nodiscard]] bool sequence_can_undo() const noexcept { return !seq_undo_.empty(); }
    [[nodiscard]] bool sequence_can_redo() const noexcept { return !seq_redo_.empty(); }
    // Preview: moves the playhead and applies the sequence to the edit scene (no events or audio).
    void sequence_seek(float time);
    [[nodiscard]] float sequence_time() const noexcept { return seq_time_; }
    // Plays the preview in real time from frame() (loops at the end).
    void sequence_preview_play(bool playing);
    [[nodiscard]] bool sequence_previewing() const noexcept { return seq_backup_.has_value(); }
    [[nodiscard]] scene::Entity sequence_preview_player();
    // Restores the entities changed by the preview.
    void end_sequence_preview();

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
    [[nodiscard]] const graph::ScriptGraph& editing_graph() const noexcept { return graph_; }
    // Validation of the open graph, refreshed after every edit.
    [[nodiscard]] const std::vector<graph::GraphDiagnostic>& graph_diagnostics() const noexcept { return graph_diags_; }
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
    bool graph_add_variable(graph::GraphVariable variable);
    // Replaces a variable; a rename also retargets every Get/Set node that used the old name.
    bool graph_update_variable(std::string_view name, graph::GraphVariable variable);
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
    // Inspector > Add Component: every component in display order, and adding one by id (one
    // undo step). False if the entity already has it or the id is unknown.
    [[nodiscard]] std::vector<ComponentChoice> component_choices(scene::Entity e) const;
    bool add_component(scene::Entity e, std::string_view id);
    void open_add_component() noexcept { add_component_open_ = 4; } // after the workspace settles
    // Hierarchy search (case-insensitive, by name): matches stay visible with their parents.
    void set_hierarchy_filter(std::string text) { hierarchy_filter_ = std::move(text); }
    // Opens the Hierarchy's "Add" menu once the workspace has settled (a few frames).
    void open_add_menu() noexcept { hierarchy_open_add_ = 4; }
    [[nodiscard]] const std::string& hierarchy_filter() const noexcept { return hierarchy_filter_; }
    // Rows the Hierarchy drew last frame and, while filtering, how many entities matched.
    [[nodiscard]] std::size_t hierarchy_rows() const noexcept { return hierarchy_rows_; }
    [[nodiscard]] std::size_t hierarchy_matches() const noexcept { return hierarchy_matches_; }
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
    // Fatal error (e.g. the GPU device was lost): writes unsaved scene edits to the autosave slot
    // so the next session offers to recover them, logs `reason`, and returns a one-line summary
    // of what was kept for the error message.
    [[nodiscard]] std::string save_for_fatal_error(std::string_view reason);

    // ---- Studio preferences, interface size and guidance (guide.cpp) ----
    // Interface size as a multiple of 100 % (on top of the display scale). Applied from the next
    // frame and saved to the preferences file. Ctrl+= / Ctrl+- / Ctrl+0 step through kUiScales.
    void set_ui_scale(float scale);
    // Ragdolls (Inspector card): one undoable step each. make_ragdoll returns the body parts
    // configured (0 when no humanoid skeleton is found under `root`).
    [[nodiscard]] bool can_make_ragdoll(scene::Entity root) const;
    [[nodiscard]] std::size_t ragdoll_bone_count(scene::Entity root) const;
    std::size_t make_ragdoll(scene::Entity root);
    bool set_ragdoll_mode(scene::Entity root, physics::RagdollComponent::Mode mode);
    std::size_t remove_ragdoll(scene::Entity root);
    // Starter characters (Vanguard V-01 "v01", V-02 "v02"): copies the model into the project
    // when missing and adds it, optionally as a ragdoll in `mode`. One undo step when `record`.
    Result<Uuid> ensure_starter_character(std::string_view id);
    scene::Entity add_character(std::string_view id, scene::Entity parent, math::Vec3 position, bool ragdoll = false,
                                physics::RagdollComponent::Mode mode = physics::RagdollComponent::Mode::Passive,
                                bool record = true);
    // Both Vanguards on a floor at the camera focus: V-01 floppy, V-02 powered. One undo step.
    scene::Entity add_ragdoll_lab();

    [[nodiscard]] float ui_scale() const noexcept { return ui_scale_; }
    void step_ui_scale(int direction); // +1 larger, -1 smaller, 0 back to the default size
    [[nodiscard]] fs::Path preferences_file() const;
    // Getting Started: which steps are done (kept across sessions and projects).
    [[nodiscard]] bool guide_done(GuideStep step) const noexcept {
        return (guide_done_ >> static_cast<unsigned>(step)) & 1u;
    }
    [[nodiscard]] std::size_t guide_done_count() const noexcept;
    void complete_guide_step(GuideStep step);
    // Runs a step's "Do it for me" action (adds a cube, nudges it, plays, saves, opens Export...).
    void run_guide_step(GuideStep step);
    void reset_guide(); // Help > Restart Getting Started: clears progress and shows the checklist
    [[nodiscard]] bool guide_dismissed() const noexcept { return guide_dismissed_; }
    void dismiss_guide();
    // Help panel: shows `topic` (by title; empty: the topic list) with the search text `query`.
    void open_help(std::string_view topic = {}, std::string_view query = {});
    [[nodiscard]] const std::string& help_topic() const noexcept { return help_topic_; }
    // Undo History panel: jumps the scene to just after command `index` (0-based; -1 = before
    // every recorded edit) by undoing or redoing. False while playing or out of range.
    bool undo_to(std::ptrdiff_t index);

    // ---- launcher ----
    // Why a project named `name` cannot be created in `location` (empty when it can): empty or
    // unsafe names, names reserved on Windows, a target folder that is not empty or is already a project.
    [[nodiscard]] static std::string new_project_problem(const fs::Path& location, std::string_view name);
    // Sample projects in EditorOptions::samples_dir, sorted by name (read once).
    [[nodiscard]] const std::vector<SampleInfo>& samples() const;
    // Copies a sample into `location` (folder named after it, made unique) and opens the copy,
    // so the original stays untouched.
    Status open_sample_copy(const fs::Path& sample, const fs::Path& location);
    // Which launcher page shows: Recent when there are recent projects, else New, until set.
    [[nodiscard]] LauncherPage launcher_page() const;
    void set_launcher_page(LauncherPage page) noexcept {
        launcher_page_ = page;
        launcher_page_set_ = true;
    }

    // ---- workspace ----
    [[nodiscard]] bool panel_open(Panel panel) const noexcept;
    void set_panel_open(Panel panel, bool open) noexcept;
    // Opens the panel and brings it to the front.
    void show_panel(Panel panel) noexcept;
    // Opens the preset's panels and re-docks every panel on the next frame.
    void apply_layout(Layout layout) noexcept;
    [[nodiscard]] Layout layout() const noexcept { return layout_; }

    // ---- problems (project validation, the same checks as `victus check`) ----
    // Starts checking the saved project files on a worker thread (again after the current check
    // if one is running). Runs by itself when a project opens and after each scene save.
    void check_project();
    [[nodiscard]] bool checking_project() const noexcept { return check_future_.valid(); }
    // Blocks until a running check is done and its result is applied (tests, tools).
    void wait_for_project_check();
    // The latest finished check; empty before the first one.
    [[nodiscard]] const std::optional<player::ValidationReport>& problems() const noexcept { return problems_; }
    // Why the latest check could not run (the project failed to open), else empty.
    [[nodiscard]] const std::string& problems_error() const noexcept { return problems_error_; }
    // Jumps to what a problem is about: selects the entity (opening its scene if the current
    // one is saved), opens a material, graph or sequence, or copies the path. False if nothing
    // could be shown.
    bool go_to_problem(const player::Diagnostic& d);

    // ---- export (File > Export Game, the same as `victus export`) ----
    void open_export_dialog();
    // <project>/build/<GameName>: where Export Game writes unless the user picks a folder.
    [[nodiscard]] fs::Path default_export_dir() const;
    // Starts exporting on a worker thread; fails at once on an obviously wrong folder or while
    // playing. The result lands in last_export()/export_error().
    Status start_export(const fs::Path& output, std::string executable_name);
    [[nodiscard]] bool exporting() const noexcept { return export_future_.valid(); }
    void wait_for_export();
    [[nodiscard]] const std::optional<player::ExportReport>& last_export() const noexcept { return export_report_; }
    [[nodiscard]] const std::string& export_error() const noexcept { return export_error_; }

    // ---- diagnostics (Help > Create Diagnostics Bundle, the same as `victus diagnose`) ----
    void open_diagnostics_dialog();
    // A new .zip on the Desktop (else the home folder) named victus-diagnostics-<time>.zip.
    [[nodiscard]] fs::Path default_bundle_path() const;
    [[nodiscard]] fs::Path diagnostics_dir() const;
    // Builds the bundle on a worker thread: crash reports, recent logs, system facts, the open
    // project's settings and check, plus Studio and GPU facts. Paths are redacted unless
    // `keep_paths`. The result lands in last_bundle()/bundle_error().
    Status start_diagnostics_bundle(const fs::Path& output, bool keep_paths = false);
    [[nodiscard]] bool creating_bundle() const noexcept { return bundle_future_.valid(); }
    void wait_for_bundle();
    [[nodiscard]] const std::optional<player::BundleResult>& last_bundle() const noexcept { return bundle_result_; }
    [[nodiscard]] const std::string& bundle_error() const noexcept { return bundle_error_; }
    // The "closed unexpectedly last time" card; dismissed once per run.
    [[nodiscard]] bool crash_notice_visible() const noexcept { return crash_notice_; }
    void set_crash_notice(bool visible) noexcept { crash_notice_ = visible; }
    void open_about_dialog() noexcept { show_about_ = true; }

    // ---- safe mode (ADR-0101) ----
    // On after EditorOptions::safe_mode_after unfinished openings of the open project, with
    // EditorOptions::safe_mode, or through open_project_safe(). Ends when the project closes.
    [[nodiscard]] bool safe_mode() const noexcept { return safe_mode_; }
    [[nodiscard]] const std::string& safe_mode_reason() const noexcept { return safe_mode_reason_; }
    Status open_project_safe(const fs::Path& path);
    // Leaves safe mode: closes the project and opens it again with everything on.
    Status reopen_normally();
    // True until startup_frames frames have run since the project opened; the marker is then removed.
    [[nodiscard]] bool startup_pending() const noexcept { return startup_frames_left_ > 0; }
    [[nodiscard]] bool safe_mode_card_visible() const noexcept { return safe_mode_ && !safe_mode_card_hidden_; }

    // ---- console ----
    // Records currently held by the Console (the newest 4096), by severity.
    struct ConsoleCounts {
        std::size_t info{0}, warnings{0}, errors{0};
    };
    [[nodiscard]] ConsoleCounts console_counts() const;
    void clear_console();
    // Brings the Console panel to the front (the status bar's error/warning counts call this).
    void focus_console() noexcept { focus_console_ = 3; }
    // What the Console shows: severity toggles (messages = info and below), a case-insensitive
    // search over category and message, and repeats of the same line merged into one row.
    void set_console_filter(std::string text) { console_search_ = std::move(text); }
    void set_console_levels(bool messages, bool warnings, bool errors) noexcept {
        console_show_[0] = messages;
        console_show_[1] = warnings;
        console_show_[2] = errors;
    }
    void set_console_collapse(bool on) noexcept { console_collapse_ = on; }
    // Rows the Console draws with the current filters (after merging repeats).
    [[nodiscard]] std::size_t console_rows() const;
    // The visible rows as plain text, one per line (what Copy puts on the clipboard).
    [[nodiscard]] std::string console_text() const;

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
    // Assets panel filters: search text (file name or folder, any case), one asset kind (nullopt: all)
    // and a folder ("" = every folder, "models" = models/ and below).
    void set_asset_filter(std::string text) { asset_search_ = std::move(text); }
    void set_asset_kind_filter(std::optional<assets::AssetKind> kind) noexcept { asset_kind_filter_ = kind; }
    void set_asset_folder(std::string folder) { asset_folder_ = std::move(folder); }
    [[nodiscard]] std::size_t asset_rows() const;              // assets shown with the current filters
    [[nodiscard]] std::vector<std::string> asset_folders() const; // every folder holding assets, sorted
    // Plain name for an asset kind: Texture -> "Image", Model -> "3D Model", SpriteSheet -> "Sprite Sheet".
    [[nodiscard]] static const char* asset_kind_label(assets::AssetKind kind) noexcept;
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
    void draw_hierarchy_node(scene::Entity e, int depth = 0);
    // The categorised "+ Add" / Entity menu / Create Child items; `parent` null = scene root.
    void draw_create_entity_items(scene::Entity parent);
    // Entities to draw while filtering: matches plus their ancestors.
    void collect_hierarchy_matches(scene::Entity e, std::unordered_set<Uuid, UuidHash>& keep, bool& any);
    std::string hierarchy_filter_, add_component_filter_;
    bool draw_add_component(scene::Entity e);       // the Inspector button and its picker
    bool add_component_raw(scene::Entity e, std::string_view id); // no undo (inside an inspector edit)
    std::unordered_set<Uuid, UuidHash> hierarchy_keep_, hierarchy_reveal_;
    Uuid hierarchy_revealed_for_{};
    bool hierarchy_scroll_{false};
    int hierarchy_open_add_{0}, add_component_open_{0};
    std::size_t hierarchy_rows_{0}, hierarchy_matches_{0};
    void draw_inspector();
    void draw_assets();
    void draw_console();
    void draw_viewport();
    void draw_status_bar();
    void draw_modals();
    void handle_shortcuts();
    void viewport_input(math::Vec2 size, bool hovered);
    void build_layout(unsigned int dockspace_id, Layout layout);
    void draw_window_menu();
    bool* panel_flag(Panel panel) noexcept;
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
    std::string asset_search_, asset_folder_;
    std::optional<assets::AssetKind> asset_kind_filter_;
    [[nodiscard]] bool asset_visible(const assets::AssetRecord& r) const;
    void draw_asset_folders(const std::vector<std::string>& folders);
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
    std::unique_ptr<ai::AiSystem> play_ai_;                         // fresh per play session
    std::unique_ptr<sequencer::SequencerSystem> play_sequencer_;    // fresh per play session
    bool show_ai_{false};
    std::vector<ai::DebugLine> ai_scratch_;
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
    render::DebugView debug_view_{render::DebugView::None};
    void apply_quality();
    std::vector<physics::DebugLine> collider_scratch_;
    physics::CollisionMeshCache collider_meshes_; // edit-mode overlay of convex/mesh colliders
    const void* collider_meshes_db_{nullptr};     // database the cache's provider reads
    [[nodiscard]] physics::CollisionMeshCache* collider_meshes();
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
    mutable ConsoleCounts console_counts_;
    mutable std::uint64_t console_counted_{~std::uint64_t{0}}; // RingSink::total_written() the counts match
    int focus_console_{0};
    std::string console_search_;
    bool console_show_[3]{true, true, true}; // messages, warnings, errors
    bool console_collapse_{true}, console_follow_{true};
    std::size_t console_last_rows_{0};
    long long console_selected_{-1}; // RingSink total index of the selected row's first record
    struct ConsoleRow {
        std::size_t record;  // index into the snapshot
        std::size_t repeats; // 1 when the line did not repeat
    };
    [[nodiscard]] std::vector<ConsoleRow> console_filtered(const std::vector<log::Record>& records) const;

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
    std::string new_character_{project::kDefaultCharacter}; // starter character for templates with one
    std::string open_path_;
    std::string launcher_error_;
    LauncherPage launcher_page_{LauncherPage::New};
    bool launcher_page_set_{false};
    mutable std::optional<std::vector<SampleInfo>> samples_;
    void draw_launcher_new();
    void draw_launcher_recent();
    void draw_launcher_samples();
    void draw_launcher_learn();

    // modals
    enum class Pending : std::uint8_t { None, Quit, CloseProject, OpenRecent };
    Pending pending_action_{Pending::None};
    fs::Path pending_path_;
    bool quit_confirmed_{false};
    bool show_save_as_{false};
    std::string save_as_path_;
    bool layout_built_{false};
    bool layout_rebuild_{false}; // force re-docking (Reset Layout, presets)
    Layout layout_{Layout::Default};
    bool core_open_[6]{true, true, true, true, true, true}; // Viewport, Hierarchy, Inspector, Assets, Console, Problems
    // guide.cpp: preferences, interface size, Getting Started, Help, Undo History.
    void load_preferences();
    void save_preferences() const;
    void apply_ui_scale();
    void draw_getting_started();
    void draw_help();
    void draw_history();
    void draw_project_settings();
    void draw_help_menu_items();
    void observe_guide(); // ticks Getting Started steps off from undo entries, play and export
    std::uint64_t guide_seen_revision_{0};
    float ui_scale_{1.0f}, applied_ui_scale_{0.0f};
    ImGuiStyle base_style_{};
    std::uint32_t guide_done_{0};
    bool guide_dismissed_{false}, guide_auto_opened_{false};
    bool show_getting_started_{false}, show_help_{false}, show_project_settings_{false}, show_history_{false};
    std::string help_topic_, help_query_;
    int help_focus_{0};
    int focus_panel_{-1};
    // Problems panel
    std::future<Result<player::ValidationReport>> check_future_;
    bool check_again_{false};
    std::optional<player::ValidationReport> problems_;
    std::string problems_error_;
    double problems_checked_at_{0}; // steady-clock seconds of the last result
    bool problems_show_errors_{true}, problems_show_warnings_{true};
    std::string problems_filter_;
    void poll_project_check();
    // Export Game
    std::future<Result<player::ExportReport>> export_future_;
    std::optional<player::ExportReport> export_report_;
    std::string export_error_, export_output_, export_name_;
    double export_started_at_{0};
    bool show_export_{false};
    std::future<Result<player::BundleResult>> bundle_future_;
    std::optional<player::BundleResult> bundle_result_;
    std::string bundle_error_, bundle_output_;
    bool bundle_keep_paths_{false}, show_bundle_{false}, crash_notice_{false}, show_about_{false};
    void poll_bundle();
    void draw_diagnostics_dialog();
    void draw_crash_notice();
    bool safe_mode_{false}, safe_request_{false}, safe_mode_card_hidden_{false};
    std::string safe_mode_reason_;
    std::uint32_t startup_frames_left_{0};
    fs::Path startup_marker_;
    void begin_startup(const fs::Path& root);
    void finish_startup();
    void update_startup();
    void draw_safe_mode_card();
    void draw_about_dialog();
    void poll_export();
    void draw_export_dialog();
    void draw_problems();
    int focus_attempts_{0};
    bool show_demo_{false};
    bool show_input_actions_{false};
    std::string capture_action_;
    bool capture_negative_{false};
    std::string input_error_;
    std::unordered_map<std::string, std::string> binding_text_; // typed bindings per action side
    void draw_input_actions();
    void draw_prefab_section(scene::Entity e);
    void draw_ragdoll_section(scene::Entity e);
    int help_place_{0};
    bool help_was_open_{false};
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
    void draw_terrain_tools();
    void draw_ai_debugger();
    void terrain_viewport_input(math::Vec2 origin, math::Vec2 size, bool hovered);
    void draw_terrain_overlay(render::SceneRenderer& r);
    [[nodiscard]] scene::Entity terrain_target();
    // Shared data of a terrain entity, promoted so asset reloads cannot drop unsaved edits.
    [[nodiscard]] std::shared_ptr<terrain::Heightfield> editable_terrain(scene::Entity terrain, Uuid* id = nullptr);
    void push_terrain_edit(std::string label, const Uuid& data, terrain::HeightfieldRegion before,
                           terrain::HeightfieldRegion after);
    Status save_terrains();
    struct TerrainStroke {
        bool active{false};
        Uuid data;
        terrain::Brush brush;
        math::Mat4 to_local;
        math::Vec3 last{0, 0, 0}; // local
        terrain::SampleRect changed;
        terrain::Heightfield original; // heights and splat at the start of the stroke
    };
    terrain::Brush terrain_brush_;
    TerrainStroke terrain_stroke_;
    bool terrain_tool_{false};
    bool show_terrain_tools_{true};
    std::optional<terrain::TerrainHit> terrain_hover_;
    terrain::GenerateSettings terrain_generate_;
    std::unordered_set<Uuid, UuidHash> dirty_terrains_;
    TileBrush tile_brush_;
    TileStroke tile_stroke_;
    bool tile_tool_{false};
    // Opens by itself when a tilemap becomes selected (and from Window > Tile Palette).
    bool show_tile_palette_{false};
    bool tile_palette_had_target_{false};
    std::optional<std::array<std::int32_t, 2>> tile_hover_;
    Uuid tileset_texture_; // "Create Tileset" popup target
    int tileset_columns_{8}, tileset_rows_{8}, tileset_collision_{1};
    bool show_material_editor_{false};
    bool material_dirty_{false};
    Uuid material_editing_;
    assets::MaterialAsset material_edit_;
    std::unordered_map<std::string, std::uint64_t> written_materials_; // path -> hash of our last write
    // sequencer panel (sequencer_panel.cpp)
    void draw_sequencer();
    void refresh_sequence_preview();
    void update_sequence_preview(float dt);
    void sequence_changed_on_disk(const std::string& path);
    bool show_sequencer_{false};
    std::string seq_path_;
    std::optional<sequencer::Sequence> seq_doc_;
    std::vector<sequencer::Sequence> seq_undo_, seq_redo_;
    bool seq_dirty_{false};
    float seq_time_{0};
    bool seq_playing_{false};
    float seq_zoom_{80.0f}; // pixels per second
    Uuid seq_player_;
    SeqKey seq_selected_;
    int seq_track_selected_{-1};
    struct SeqDrag {
        SeqKey key;
        float time{0};
        bool moved{false};
    };
    std::optional<SeqDrag> seq_drag_;
    sequencer::Track seq_scratch_; // key inspector edits (committed when an edit finishes)
    SeqKey seq_scratch_for_;
    char seq_target_buf_[128]{};
    std::unique_ptr<sequencer::SequencerSystem> edit_sequencer_;
    struct SeqBackup {
        struct Item {
            Uuid uuid;
            scene::Transform transform;
            std::optional<bool> camera_primary;
            std::optional<animation::AnimatorComponent> animator;
        };
        std::vector<Item> items;
    };
    std::optional<SeqBackup> seq_backup_;
    // animation window (animation_panel.cpp)
    void draw_animation_panel();
    void update_animation_preview(float dt);
    struct AnimBackup {
        Uuid target;
        animation::AnimatorComponent animator;
        std::vector<std::pair<Uuid, scene::Transform>> transforms; // target and its descendants
    };
    std::optional<AnimBackup> anim_backup_;
    std::unique_ptr<animation::AnimationSystem> edit_animation_; // edit-mode preview of one Animator
    bool anim_preview_paused_{false};
    bool show_animation_{false};
    std::unordered_map<std::string, std::uint64_t> written_sequences_; // path -> hash of our last write
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
    Result<Uuid> create_graph_file(std::string_view relative_path, graph::GraphDomain domain);
    // Pushes an undo snapshot; consecutive edits with the same non-empty key share one step.
    void graph_begin_edit(std::string_view coalesce_key = {});
    void graph_end_edit();
    bool show_graph_editor_{false};
    bool graph_dirty_{false};
    bool graph_focused_{false};
    Uuid graph_asset_;
    std::string graph_path_;
    std::uint64_t graph_disk_hash_{0};
    graph::ScriptGraph graph_;
    std::vector<graph::GraphDiagnostic> graph_diags_;
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
