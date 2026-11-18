// SPDX-License-Identifier: Apache-2.0
// Editor core: projects, scenes, editing operations, undo, selection, play mode, rendering.
#include "victus/gameui/gameui.hpp"
#include "victus/editor/editor.hpp"

#include "editor_client.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/profiler.hpp"
#include "victus/platform/platform.hpp"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <chrono>
#include <format>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

render::Camera3D EditorCamera::camera3d() const {
    const math::Vec3 offset{std::sin(yaw) * std::cos(pitch) * distance, -std::sin(pitch) * distance,
                            std::cos(yaw) * std::cos(pitch) * distance};
    render::Camera3D c = render::Camera3D::look_at(target + offset, target);
    c.z_near = std::max(0.05f, distance * 0.005f);
    c.z_far = std::max(500.0f, distance * 20.0f);
    return c;
}

render::Camera2D EditorCamera::camera2d(float viewport_height) const {
    render::Camera2D c = cam2d;
    c.pixels_per_unit = std::max(viewport_height, 1.0f) / std::max(view_height, 1e-3f);
    return c;
}

namespace {
fs::Path default_projects_dir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    return fs::Path(home ? home : ".") / "VICTUS Projects";
}
} // namespace

Result<std::unique_ptr<Editor>> Editor::create(render::GpuContext& gpu, ui::UiContext& ui, platform::Platform* platform,
                                               platform::Window* window, const EditorOptions& options) {
    std::unique_ptr<Editor> ed(new Editor(gpu, ui, platform, window));
    ed->options_ = options;
    ed->autosave_seconds_ = std::max(options.autosave_seconds, 0.0f);
    project::register_components();
    if (platform) ed->input_ = std::make_unique<player::PlatformInput>(platform->input());
    ed->reset_simulation();
    ed->recent_ = std::make_unique<project::RecentProjects>(options.recent_file);
    ed->log_sink_ = std::make_shared<log::RingSink>(4096);
    log::logger().add_sink(ed->log_sink_);
    {
        // Studio typography: Inter Medium body text, Inter Bold headings (falls back to the built-in font).
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(ui.imgui());
        kit::install_fonts();
        ImGui::SetCurrentContext(previous);
    }
    auto view = runtime::SceneView::create(gpu, rhi::Format::RGBA8_UNORM);
    if (!view) return std::move(view).error();
    ed->view_ = std::move(view).value();
    ed->scene_ = std::make_unique<Scene>();
    ed->new_location_ = (options.default_project_dir.empty() ? default_projects_dir() : options.default_project_dir).string();
    ed->update_title();
    return ed;
}

Editor::Editor(render::GpuContext& gpu, ui::UiContext& ui, platform::Platform* platform, platform::Window* window)
    : gpu_(gpu), ui_(ui), platform_(platform), window_(window) {}

Editor::~Editor() {
    if (check_future_.valid()) check_future_.wait();
    stop_editor_clients();
    if (play_server_) play_server_->shutdown(sim_.time().time); // tell joined players we are gone
    play_server_.reset();
    if (log_sink_) log::logger().remove_sink(log_sink_);
    thumbnails_.reset();
    cache_.reset();
    if (viewport_tex_) gpu_.device().destroy(viewport_tex_);
}

void Editor::notify(std::string message, bool error) {
    if (error) VICTUS_LOG_ERROR("editor", "{}", message);
    else VICTUS_LOG_INFO("editor", "{}", message);
    status_line_ = message;
    status_time_ = 4.0;
    status_messages_.push_back(std::move(message));
    if (status_messages_.size() > 64) status_messages_.erase(status_messages_.begin());
}

void Editor::update_title() {
    if (!window_) return;
    std::string title = "VICTUS Editor";
    if (project_) {
        title = project_->settings().name + " - " + (scene_path_.empty() ? std::string("untitled scene") : scene_path_) +
                (dirty() ? " *" : "") + " - VICTUS Editor";
    }
    window_->set_title(title);
}

// ---- projects ------------------------------------------------------------------------------------

Status Editor::open_project(const fs::Path& path) {
    auto p = project::Project::open(path);
    if (!p) return std::move(p).error();
    close_project();
    assets::RefreshReport report;
    auto db = assets::AssetDatabase::open(p->root(), &report);
    if (!db) return std::move(db).error();
    project_ = std::move(p).value();
    db_ = std::move(db).value();
    for (const auto& e : report.errors) notify("asset metadata: " + e, true);
    const auto imported = db_->import_all();
    for (const auto& m : imported.messages) notify(m, imported.failed > 0);
    cache_ = std::make_unique<runtime::GpuAssetCache>(gpu_, db_.get());
    if (auto thumbs = ThumbnailCache::create(gpu_); thumbs) thumbnails_ = std::move(thumbs).value();
    else notify("asset thumbnails unavailable: " + thumbs.error().message, true);
    hot_reload_ = std::make_unique<runtime::AssetHotReloader>(*db_);
    edit_sprites_ = std::make_unique<animation::AnimationSystem>(
        animation::AnimationSettings{.sheets = assets::sprite_sheet_provider(*db_)});
    tilemaps_ = std::make_unique<tilemap::TilemapSystem>(assets::tileset_provider(*db_));
    terrains_ = std::make_unique<terrain::TerrainSystem>(assets::terrain_provider(*db_));
    {
        script::HostOptions so;
        so.assets_root = db_->assets_dir();
        if (auto host = script::ScriptHost::create(so)) edit_scripts_ = std::move(host).value();
        else notify("scripting unavailable: " + host.error().message, true);
    }
    recent_->add(project_->root());
    (void)recent_->save();
    const std::string startup = project_->settings().startup_scene;
    if (auto st = open_scene(startup); !st.ok()) {
        notify("could not open startup scene '" + startup + "': " + st.error().message, true);
        new_scene();
    }
    camera_ = EditorCamera{};
    quality_ = render::parse_quality_tier(project_->settings().render_quality).value_or(render::QualityTier::High);
    apply_quality();
    notify("Opened project " + project_->settings().name);
    problems_.reset();
    problems_error_.clear();
    check_project();
    return {};
}

Status Editor::create_project(const fs::Path& directory, std::string_view name, std::string_view template_id,
                              const project::TemplateOptions& options) {
    auto p = project::Project::create(directory, name, template_id, options);
    if (!p) return std::move(p).error();
    return open_project(directory);
}

void Editor::close_project() {
    check_again_ = false;
    if (check_future_.valid()) check_future_.wait(); // the check reads this project's files
    check_future_ = {};
    problems_.reset();
    problems_error_.clear();
    stop();
    close_sequence();
    gizmo_cancel();
    selection_.clear();
    tile_stroke_ = {};
    tile_hover_.reset();
    terrain_stroke_ = {};
    terrain_hover_.reset();
    dirty_terrains_.clear();
    undo_.clear();
    hot_reload_.reset();
    thumbnails_.reset();
    cache_.reset();
    edit_scripts_.reset();
    graph_asset_ = {};
    graph_path_.clear();
    graph_ = {};
    graph_diags_.clear();
    graph_undo_.clear();
    graph_redo_.clear();
    graph_dirty_ = false;
    show_graph_editor_ = false;
    graph_heat_.clear();
    graph_hits_seen_.clear();
    graph_error_.reset();
    graph_live_ = false;
    end_animation_preview();
    edit_sprites_.reset();
    tilemaps_.reset();
    terrains_.reset();
    audio_.reset(); // clips resolve against the project's assets
    audio_failed_ = false;
    db_.reset();
    recovery_.reset();
    autosave_written_ = false;
    project_.reset();
    scene_ = std::make_unique<Scene>();
    scene_path_.clear();
    update_title();
}

Status Editor::reimport_assets() {
    if (!db_) return {};
    const auto refresh = db_->refresh();
    for (const auto& e : refresh.errors) notify("asset metadata: " + e, true);
    const auto report = db_->import_all();
    for (const auto& m : report.messages) notify(m, report.failed > 0);
    if (report.imported > 0) {
        cache_->clear();
        if (thumbnails_) thumbnails_->clear();
    }
    notify(std::to_string(report.imported) + " imported, " + std::to_string(report.up_to_date) + " up to date, " +
               std::to_string(report.failed) + " failed",
           report.failed > 0);
    return {};
}

std::size_t Editor::check_asset_changes() {
    if (!hot_reload_) return 0;
    const auto changes = hot_reload_->check();
    if (changes.empty()) return 0;
    if (thumbnails_) {
        // Previews depend on textures, models, materials and shaders; re-render them all when any
        // of those changed (spread over frames), otherwise only the changed prefabs.
        if (!changes.reimported.empty() || !changes.materials.empty() || !changes.shaders.empty() ||
            !changes.sprite_sheets.empty() || !changes.tilesets.empty())
            thumbnails_->clear();
        for (const auto& p : changes.prefabs)
            if (const auto* rec = db_ ? db_->find_path(p) : nullptr) thumbnails_->invalidate(rec->uuid);
    }
    runtime::AssetHotReloader::apply(changes, cache_.get(), play_animation_.get(), tilemaps_.get(), terrains_.get());
    if (edit_sprites_) edit_sprites_->clear_sheets();
    if (edit_animation_) { // the edit-mode preview follows model and controller edits
        edit_animation_->clear_cache();
        if (!changes.anim_controllers.empty()) edit_animation_->reload_controllers();
    }
    if (audio_)
        for (const auto& clip : changes.audio) (void)audio_->reload_clip(clip);
    for (const auto& p : changes.reimported_paths) notify("Reloaded " + p);
    for (const auto& f : changes.failed) notify("Reload failed, keeping the previous version: " + f, true);
    for (const auto& a : changes.audio) notify("Reloaded audio " + a);
    for (const auto& t : changes.behavior_trees) {
        if (play_ai_) play_ai_->reload(t);
        notify("Reloaded behaviour tree " + t);
    }
    if (!changes.string_tables.empty()) {
        if (play_scripts_) (void)play_scripts_->reload_localization();
        for (const auto& t : changes.string_tables) notify("Reloaded string table " + t);
    }
    for (const auto& q : changes.sequences) {
        if (play_sequencer_) play_sequencer_->reload(q);
        sequence_changed_on_disk(q);
    }
    for (const auto& c : changes.anim_controllers) notify("Reloaded animation controller " + c);
    for (const auto& m : changes.materials) {
        if (const auto it = written_materials_.find(m); it != written_materials_.end() && db_)
            if (const auto* rec = db_->find_path(m))
                if (auto text = fs::read_text(db_->source_path(*rec)); text && fnv1a64(*text) == it->second) continue; // our own save
        notify("Reloaded material " + m);
        // Edited outside the editor: the open material (or one of its parents) changed.
        if (show_material_editor_ && db_ && !material_editing_.is_nil() && !material_dirty_)
            if (auto fresh = db_->resolve_material(material_editing_); fresh) material_edit_ = *fresh;
    }
    for (const auto& sh : changes.shaders) {
        // Report compile errors right away (the viewport falls back to built-in PBR meanwhile).
        const assets::AssetRecord* rec = db_ ? db_->find_path(sh) : nullptr;
        if (rec && cache_) {
            const Uuid id = rec->uuid;
            if (auto c = cache_->surface_shader(id); !c) notify("Shader " + sh + ": " + c.error().message, true);
            else notify("Reloaded shader " + sh);
        }
    }
    for (const auto& p : changes.prefabs) {
        if (play_scripts_) play_scripts_->forget_prefabs();
        const assets::AssetRecord* rec = db_ ? db_->find_path(p) : nullptr;
        if (!rec) continue;
        if (auto text = fs::read_text(db_->source_path(*rec)); text) {
            const auto it = written_prefabs_.find(p);
            if (it != written_prefabs_.end() && it->second == fnv1a64(*text)) continue; // our own save
        }
        if (play_state_ != PlayState::Editing) {
            notify("Prefab " + p + " changed; stop playing to update its instances");
            continue;
        }
        if (auto n = update_prefab_instances(rec->uuid); !n) notify("Prefab " + p + ": " + n.error().message, true);
        else notify(std::format("Reloaded prefab {} ({} instance{} updated)", p, *n, *n == 1 ? "" : "s"));
    }
    for (const auto& r : changes.restart_needed) {
        // Scenes are the editor's own files (saved from here, reopened with File > Open Scene).
        if (assets::kind_for_extension(fs::Path(r).extension().string()) == assets::AssetKind::Scene) continue;
        notify(r + " changed; reopen the project or restart the game to see it");
    }
    return changes.reimported.size() + changes.audio.size() + changes.prefabs.size() + changes.materials.size() + changes.shaders.size() + changes.sprite_sheets.size() + changes.tilesets.size() + changes.terrains.size();
}

// ---- scenes ------------------------------------------------------------------------------------

void Editor::set_scene(std::unique_ptr<Scene> s, std::string path) {
    stop();
    end_sequence_preview(); // restores the outgoing scene before it goes away
    end_animation_preview();
    gizmo_cancel();
    scene_ = std::move(s);
    scene_path_ = std::move(path);
    selection_.clear();
    undo_.clear();
    undo_.mark_saved();
    pending_before_.clear();
    pending_dirty_ = false;
    if (scene_ && runtime::scene_view_mode(*scene_) == runtime::ViewMode::TwoD) {
        if (auto c = runtime::scene_camera_2d(*scene_, 1000.0f)) {
            camera_.cam2d = *c;
            camera_.view_height = 1000.0f / std::max(c->pixels_per_unit, 1e-3f);
            camera_.cam2d.zoom = 1.0f;
        }
    }
    update_title();
}

Status Editor::open_scene(std::string_view rel) {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    auto path = project_->resolve(rel);
    if (!path) return std::move(path).error();
    auto s = std::make_unique<Scene>();
    VICTUS_TRY_STATUS(s->load_file(*path));
    set_scene(std::move(s), std::string(rel));
    check_recovery();
    return {};
}

Status Editor::save_scene() {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (scene_path_.empty()) return make_error(ErrorCode::InvalidArgument, "scene has no path; use Save As");
    auto path = project_->resolve(scene_path_);
    if (!path) return std::move(path).error();
    std::error_code ec;
    sfs::create_directories(path->parent_path(), ec);
    end_sequence_preview(); // never save previewed transforms
    end_animation_preview();
    VICTUS_TRY_STATUS(save_terrains());
    VICTUS_TRY_STATUS(scene_->save_file(*path));
    undo_.mark_saved();
    if (autosave_written_) remove_autosave(autosave_scene_); // e.g. the untitled copy after Save As
    remove_autosave(scene_path_);
    autosave_written_ = false;
    update_title();
    notify("Saved " + scene_path_);
    check_project();
    return {};
}

Status Editor::save_scene_as(std::string_view rel) {
    std::string r(rel);
    if (!r.ends_with(project::kSceneExtension)) r += project::kSceneExtension;
    auto clean = fs::sanitize_relative(r);
    if (!clean) return std::move(clean).error();
    scene_path_ = *clean;
    return save_scene();
}

void Editor::new_scene() {
    auto s = std::make_unique<Scene>();
    project::TemplateOptions bare;
    bare.character = std::string(project::kNoCharacter); // stripped below anyway
    (void)project::build_template_scene("3d", *s, bare);
    // A new scene starts with camera + light only.
    std::vector<Entity> remove;
    for (const Entity e : s->roots()) {
        if (!s->world().has<CameraComponent>(e) && !s->world().has<LightComponent>(e)) remove.push_back(e);
        else if (const auto* l = s->world().get<LightComponent>(e); l && l->type != LightComponent::Type::Directional)
            remove.push_back(e);
    }
    for (const Entity e : remove) s->destroy(e);
    s->name = "New Scene";
    set_scene(std::move(s), "");
    check_recovery();
}

// ---- editing -------------------------------------------------------------------------------------

void Editor::record_edit(std::string label, std::vector<EntityState> before) {
    if (play_state_ != PlayState::Editing) return; // play-mode tweaks are discarded on stop
    std::vector<Uuid> ids;
    for (const auto& s : before) ids.push_back(s.uuid);
    SceneCommand c{std::move(label), std::move(before), capture(*scene_, ids)};
    undo_.push(std::move(c));
    update_title();
}

Entity Editor::create_entity(Primitive kind, Entity parent) {
    Scene& s = active_scene();
    static constexpr const char* kNames[] = {"Entity", "Cube", "Sphere", "Plane", "Cylinder", "Camera",
                                             "Directional Light", "Point Light", "Spot Light", "Sprite", "Decal", "2D Light"};
    static_assert(std::size(kNames) == static_cast<std::size_t>(Primitive::Light2D) + 1, "one name per Primitive");
    const Entity e = s.create(kNames[static_cast<int>(kind)], parent);
    auto& w = s.world();
    // Place new objects in front of the editor camera focus.
    if (parent.is_null()) w.get<Transform>(e)->position = runtime::scene_view_mode(s) == runtime::ViewMode::TwoD
                                                             ? math::Vec3{camera_.cam2d.position.x, camera_.cam2d.position.y, 0}
                                                             : camera_.target;
    const auto mesh = [&](const char* prim) {
        auto& m = w.add<MeshRendererComponent>(e);
        m.primitive = prim;
        m.base_color = {0.8f, 0.8f, 0.8f, 1};
    };
    switch (kind) {
    case Primitive::Empty: break;
    case Primitive::Cube: mesh("cube"); break;
    case Primitive::Sphere: mesh("sphere"); break;
    case Primitive::Plane: mesh("plane"); w.get<Transform>(e)->scale = {5, 1, 5}; break;
    case Primitive::Cylinder: mesh("cylinder"); break;
    case Primitive::Camera: w.add<CameraComponent>(e).primary = false; break;
    case Primitive::DirectionalLight: {
        w.add<LightComponent>(e).type = LightComponent::Type::Directional;
        w.get<Transform>(e)->rotation = math::Quat::euler(math::radians(-50.0f), math::radians(30.0f), 0);
        break;
    }
    case Primitive::PointLight: {
        auto& l = w.add<LightComponent>(e);
        l.type = LightComponent::Type::Point;
        l.intensity = 10.0f;
        w.get<Transform>(e)->position.y += 1.5f;
        break;
    }
    case Primitive::SpotLight: {
        auto& l = w.add<LightComponent>(e);
        l.type = LightComponent::Type::Spot;
        l.intensity = 20.0f;
        w.get<Transform>(e)->position.y += 3.0f;
        w.get<Transform>(e)->rotation = math::Quat::euler(math::radians(-90.0f), 0, 0); // points down
        break;
    }
    case Primitive::Sprite: w.add<SpriteComponent>(e); break;
    case Primitive::Decal: {
        // Projects straight down onto the ground below the focus point.
        w.add<gameplay::DecalComponent>(e).size = {1.0f, 1.0f, 1.0f};
        w.get<Transform>(e)->rotation = math::Quat::euler(math::radians(-90.0f), 0, 0);
        break;
    }
    case Primitive::Light2D: w.add<gameplay::Light2DComponent>(e); break;
    }
    s.update_transforms();
    const Uuid id = s.uuid(e);
    if (play_state_ == PlayState::Editing) {
        auto after = capture(*scene_, {id});
        undo_.push({std::string("Create ") + kNames[static_cast<int>(kind)], absent(after), std::move(after)});
        update_title();
    }
    selection_ = {id};
    return e;
}

void Editor::prune_selection() {
    Scene& s = active_scene();
    std::erase_if(selection_, [&](const Uuid& u) { return s.find(u).is_null(); });
}

void Editor::delete_selected() {
    prune_selection();
    if (selection_.empty()) return;
    Scene& s = active_scene();
    // Only delete top-most selected entities (children go with their parents).
    std::vector<Uuid> roots;
    for (const Uuid& u : selection_) {
        const Entity e = s.find(u);
        bool covered = false;
        for (const Uuid& v : selection_)
            if (v != u && s.is_descendant(e, s.find(v))) covered = true;
        if (!covered) roots.push_back(u);
    }
    auto before = capture_subtrees(s, roots);
    for (const Uuid& u : roots)
        if (const Entity e = s.find(u); !e.is_null()) s.destroy(e);
    if (play_state_ == PlayState::Editing) {
        undo_.push({roots.size() == 1 ? "Delete entity" : "Delete entities", before, absent(before)});
        update_title();
    }
    selection_.clear();
}

void Editor::duplicate_selected() {
    prune_selection();
    if (selection_.empty()) return;
    Scene& s = active_scene();
    std::vector<Uuid> created;
    for (const Uuid& u : selection_) {
        const Entity copy = s.duplicate(s.find(u));
        if (!copy.is_null()) created.push_back(s.uuid(copy));
    }
    s.update_transforms();
    if (play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, created);
        // The sibling after which the copies were inserted changes ordering: capture it too.
        undo_.push({"Duplicate", absent(after), after});
        update_title();
    }
    selection_ = created;
}

bool Editor::reparent(Entity e, Entity new_parent, Entity before) {
    Scene& s = active_scene();
    if (e.is_null()) return false;
    const Uuid id = s.uuid(e);
    auto snap = capture(s, {id});
    const math::Mat4 world = s.world_matrix(e);
    bool ok = before.is_null() ? s.move_before(e, kNullEntity, new_parent) : s.move_before(e, before);
    if (!ok) return false;
    // Keep the world-space placement.
    const Entity p = s.parent(e);
    const math::Mat4 parent_world = p.is_null() ? math::Mat4{} : s.world_matrix(p);
    if (Transform* t = s.world().get<Transform>(e)) math::decompose(math::inverse(parent_world) * world, t->position, t->rotation, t->scale);
    s.update_transforms();
    if (play_state_ == PlayState::Editing) record_edit("Reparent", std::move(snap));
    return true;
}

void Editor::rename(Entity e, std::string name) {
    Scene& s = active_scene();
    Name* n = s.world().get<Name>(e);
    if (!n || n->value == name) return;
    std::vector<Uuid> ids{s.uuid(e)};
    if (const Entity root = instance_root(s, e); !root.is_null() && root != e) ids.push_back(s.uuid(root));
    auto before = capture(s, ids);
    n->value = std::move(name);
    mark_prefab_overrides(e, before);
    record_edit("Rename", std::move(before));
}

Entity Editor::instantiate_asset(const Uuid& asset) {
    if (!db_ || !cache_) return kNullEntity;
    auto model = cache_->model(asset);
    if (!model) {
        notify("cannot load model: " + model.error().message, true);
        return kNullEntity;
    }
    const assets::AssetRecord* rec = db_->find(asset);
    const std::string name = rec ? fs::Path(rec->path).stem().string() : std::string("Model");
    Scene& s = active_scene();
    const Entity root = runtime::instantiate_model(s, asset, (*model)->cpu, name);
    s.world().get<Transform>(root)->position = camera_.target;
    s.update_transforms();
    if (play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, {s.uuid(root)});
        undo_.push({"Add " + name, absent(after), after});
        update_title();
    }
    selection_ = {s.uuid(root)};
    return root;
}

// ---- prefabs -------------------------------------------------------------------------------------

Result<Prefab> Editor::load_prefab(const Uuid& asset) const {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = db_->find(asset);
    if (!rec || rec->kind != assets::AssetKind::Prefab)
        return make_error(ErrorCode::NotFound, "prefab asset " + asset.to_string() + " not found");
    return load_prefab_file(db_->source_path(*rec));
}

Result<Uuid> Editor::create_prefab(Entity e, std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop playing to create prefabs");
    Scene& s = *scene_;
    if (!s.world().alive(e)) return make_error(ErrorCode::InvalidArgument, "no entity");
    std::string rel(relative_path);
    if (rel.empty()) {
        std::string base = s.world().get<Name>(e) ? s.world().get<Name>(e)->value : std::string("Prefab");
        for (char& ch : base)
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
        if (base.empty()) base = "Prefab";
        rel = "prefabs/" + base + std::string(kPrefabExtension);
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("prefabs/{}_{}{}", base, i, kPrefabExtension);
    } else if (!rel.ends_with(kPrefabExtension)) {
        rel += kPrefabExtension;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    const Prefab prefab = make_prefab(s, e);
    const std::string text = json::stringify(prefab_to_json(prefab)) + "\n";
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(fs::write_text_atomic(db_->assets_dir() / *clean, text));
    written_prefabs_[*clean] = fnv1a64(text);
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "prefab was written but not registered: " + *clean);
    const Uuid asset = rec->uuid;
    auto before = capture_subtrees(s, {s.uuid(e)});
    link_as_instance(s, e, asset);
    record_edit("Create prefab", std::move(before));
    notify("Created prefab " + *clean);
    return asset;
}

Entity Editor::instantiate_prefab(const Uuid& asset, Entity parent) {
    auto prefab = load_prefab(asset);
    if (!prefab) {
        notify("cannot load prefab: " + prefab.error().message, true);
        return kNullEntity;
    }
    Scene& s = active_scene();
    InstantiateOptions o;
    o.parent = parent;
    if (parent.is_null()) o.position = camera_.target;
    auto root = scene::instantiate(s, *prefab, asset, o);
    if (!root) {
        notify("cannot instantiate prefab: " + root.error().message, true);
        return kNullEntity;
    }
    s.update_transforms();
    if (play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, {s.uuid(*root)});
        undo_.push({"Add " + prefab->name, absent(after), after});
        update_title();
    }
    selection_ = {s.uuid(*root)};
    return *root;
}

Result<std::size_t> Editor::update_prefab_instances(const Uuid& asset) {
    auto prefab = load_prefab(asset);
    if (!prefab) return std::move(prefab).error();
    Scene& s = *scene_;
    std::vector<Uuid> roots;
    for (const Entity e : find_instances(s, asset)) roots.push_back(s.uuid(e));
    if (roots.empty()) return std::size_t{0};
    auto before = capture_subtrees(s, roots);
    auto n = update_instances(s, asset, *prefab);
    if (!n) return std::move(n).error();
    // Undo restores the previous instance content (entities the update removed come back too).
    auto after = capture_subtrees(s, roots);
    std::vector<Uuid> after_ids;
    for (const auto& st : after) after_ids.push_back(st.uuid);
    for (const auto& st : before)
        if (std::find(after_ids.begin(), after_ids.end(), st.uuid) == after_ids.end()) after.push_back({st.uuid, std::nullopt});
    for (const auto& st : after)
        if (std::none_of(before.begin(), before.end(), [&](const EntityState& b) { return b.uuid == st.uuid; }))
            before.push_back({st.uuid, std::nullopt});
    undo_.push({"Update prefab instances", std::move(before), std::move(after)});
    s.update_transforms();
    prune_selection();
    update_title();
    return n;
}

Status Editor::apply_prefab(Entity instance) {
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop playing to apply prefab changes");
    Scene& s = *scene_;
    const Entity root = instance_root(s, instance);
    if (root.is_null()) return make_error(ErrorCode::InvalidArgument, "not a prefab instance");
    const Uuid asset = s.world().get<PrefabInstance>(root)->prefab;
    auto previous = load_prefab(asset);
    if (!previous) return std::move(previous).error();
    auto before = capture_subtrees(s, {s.uuid(root)});
    const Prefab applied = apply_instance(s, root, *previous);
    const assets::AssetRecord* rec = db_->find(asset);
    const std::string text = json::stringify(prefab_to_json(applied)) + "\n";
    VICTUS_TRY_STATUS(fs::write_text_atomic(db_->source_path(*rec), text));
    written_prefabs_[rec->path] = fnv1a64(text);
    if (thumbnails_) thumbnails_->invalidate(asset);
    record_edit("Apply prefab", std::move(before));
    auto n = update_prefab_instances(asset);
    if (!n) return std::move(n).error();
    notify(std::format("Applied to {} ({} instance{} in this scene)", rec->path, *n, *n == 1 ? "" : "s"));
    return {};
}

Status Editor::revert_prefab(Entity instance) {
    Scene& s = active_scene();
    const Entity root = instance_root(s, instance);
    if (root.is_null()) return make_error(ErrorCode::InvalidArgument, "not a prefab instance");
    auto prefab = load_prefab(s.world().get<PrefabInstance>(root)->prefab);
    if (!prefab) return std::move(prefab).error();
    auto before = capture_subtrees(s, {s.uuid(root)});
    VICTUS_TRY_STATUS(update_instance(s, root, *prefab, /*keep_overrides=*/false));
    if (play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, {s.uuid(root)});
        for (const auto& st : before)
            if (std::none_of(after.begin(), after.end(), [&](const EntityState& a) { return a.uuid == st.uuid; }))
                after.push_back({st.uuid, std::nullopt});
        for (const auto& st : after)
            if (std::none_of(before.begin(), before.end(), [&](const EntityState& b) { return b.uuid == st.uuid; }))
                before.push_back({st.uuid, std::nullopt});
        undo_.push({"Revert prefab", std::move(before), std::move(after)});
        update_title();
    }
    s.update_transforms();
    prune_selection();
    return {};
}

void Editor::unpack_prefab(Entity instance) {
    Scene& s = active_scene();
    const Entity root = instance_root(s, instance);
    if (root.is_null()) return;
    auto before = capture_subtrees(s, {s.uuid(root)});
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        if (e != root && s.world().has<PrefabInstance>(e)) continue; // nested instances stay instances
        s.world().remove<PrefabInstance>(e);
        s.world().remove<PrefabPart>(e);
        for (const Entity c : s.children(e)) stack.push_back(c);
    }
    record_edit("Unpack prefab", std::move(before));
}

void Editor::mark_prefab_overrides(Entity e, const std::vector<EntityState>& before) {
    Scene& s = active_scene();
    const Entity root = instance_root(s, e);
    if (root.is_null()) return;
    const Uuid id = s.uuid(e);
    const json::Value* old = nullptr;
    for (const auto& st : before)
        if (st.uuid == id && st.snapshot) old = &*st.snapshot;
    if (!old) return;
    const json::Value now = s.save_entity(e);
    std::vector<std::string> changed;
    if (!(now["name"] == (*old)["name"])) changed.emplace_back("name");
    const json::Value& oc = (*old)["components"];
    const json::Value& nc = now["components"];
    if (nc.is_object())
        for (const auto& [k, v] : nc.as_object())
            if (const json::Value* o = oc.find(k); !o || !(*o == v)) changed.push_back(k);
    if (oc.is_object())
        for (const auto& [k, v] : oc.as_object())
            if (!nc.find(k)) changed.push_back(k);
    if (changed.empty()) return;
    auto prefab = load_prefab(s.world().get<PrefabInstance>(root)->prefab);
    if (!prefab) return; // missing prefab: the instance keeps its content anyway
    for (const auto& k : changed) (void)mark_override(s, e, k, *prefab);
}

void Editor::undo() {
    if (play_state_ != PlayState::Editing) return;
    const SceneCommand* c = undo_.next_undo();
    if (!c) return;
    const std::string label = c->label;
    if (auto st = undo_.undo(*scene_); !st.ok()) notify("undo failed: " + st.error().message, true);
    else notify("Undo " + label);
    prune_selection();
    update_title();
}

void Editor::redo() {
    if (play_state_ != PlayState::Editing) return;
    const SceneCommand* c = undo_.next_redo();
    if (!c) return;
    const std::string label = c->label;
    if (auto st = undo_.redo(*scene_); !st.ok()) notify("redo failed: " + st.error().message, true);
    else notify("Redo " + label);
    prune_selection();
    update_title();
}

// ---- selection -------------------------------------------------------------------------------------

void Editor::select(Entity e, bool additive) {
    Scene& s = active_scene();
    if (e.is_null()) {
        if (!additive) selection_.clear();
        return;
    }
    const Uuid id = s.uuid(e);
    if (additive) {
        const auto it = std::find(selection_.begin(), selection_.end(), id);
        if (it != selection_.end()) selection_.erase(it);
        else selection_.push_back(id);
    } else {
        selection_ = {id};
    }
}

Entity Editor::selected() noexcept {
    if (selection_.empty()) return kNullEntity;
    return active_scene().find(selection_.back());
}

bool Editor::is_selected(Entity e) const {
    const Scene& s = const_cast<Editor*>(this)->active_scene();
    return std::find(selection_.begin(), selection_.end(), s.uuid(e)) != selection_.end();
}

math::Aabb Editor::entity_bounds(Entity e, bool* has_mesh) {
    Scene& s = active_scene();
    math::Aabb local{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    bool mesh = false;
    if (const auto* mr = s.world().get<MeshRendererComponent>(e); mr && cache_) {
        if (!mr->mesh_asset.is_nil()) {
            if (auto m = cache_->model(mr->mesh_asset); m && mr->mesh_index < (*m)->meshes.size()) {
                local = (*m)->meshes[mr->mesh_index].bounds;
                mesh = true;
            }
        } else if (const auto* g = cache_->primitive(mr->primitive)) {
            local = g->bounds;
            mesh = true;
        }
    } else if (const auto* sp = s.world().get<SpriteComponent>(e)) {
        local = {{-sp->pivot.x * sp->size.x, -sp->pivot.y * sp->size.y, -0.01f},
                 {(1 - sp->pivot.x) * sp->size.x, (1 - sp->pivot.y) * sp->size.y, 0.01f}};
        mesh = true;
    } else if (const auto* tm = s.world().get<tilemap::TilemapComponent>(e)) {
        const tilemap::TileBounds b = tilemap::bounds(*tm);
        if (!b.empty()) {
            local = {{static_cast<float>(b.min_x) * tm->cell_size.x, static_cast<float>(b.min_y) * tm->cell_size.y, -0.01f},
                     {static_cast<float>(b.max_x + 1) * tm->cell_size.x, static_cast<float>(b.max_y + 1) * tm->cell_size.y, 0.01f}};
        } else {
            local = {{0, 0, -0.01f}, {tm->cell_size.x, tm->cell_size.y, 0.01f}};
        }
        mesh = true;
    } else if (const auto* dc = s.world().get<gameplay::DecalComponent>(e)) {
        // The projection box (outlined when selected); not pickable, it would cover the surfaces it decorates.
        local = {dc->size * -0.5f, dc->size * 0.5f};
    } else {
        local = {{-0.25f, -0.25f, -0.25f}, {0.25f, 0.25f, 0.25f}};
    }
    if (has_mesh) *has_mesh = mesh;
    return local;
}

void Editor::frame_selected() {
    const Entity e = selected();
    if (e.is_null()) return;
    Scene& s = active_scene();
    const math::Mat4 m = s.world_matrix(e);
    const math::Aabb local = entity_bounds(e, nullptr);
    const math::Vec3 c = m.transform_point((local.min + local.max) * 0.5f);
    const math::Vec3 ext = m.transform_vector(local.max - local.min);
    const float radius = std::max(0.5f, math::length(ext) * 0.5f);
    camera_.target = c;
    camera_.distance = radius * 2.8f;
    camera_.cam2d.position = {c.x, c.y};
}

// Slab test of a ray against an entity's local bounds (ray transformed into local space).
Entity Editor::pick(math::Vec2 pixel, math::Vec2 size) {
    Scene& s = active_scene();
    if (size.x <= 0 || size.y <= 0) return kNullEntity;
    const bool two_d = runtime::scene_view_mode(s) == runtime::ViewMode::TwoD;
    Entity best = kNullEntity;
    if (two_d) {
        const math::Vec2 world = camera_.camera2d(size.y).screen_to_world(pixel, size.x, size.y);
        std::int32_t best_layer = std::numeric_limits<std::int32_t>::min();
        s.world().each<SpriteComponent, WorldTransform>([&](Entity e, const SpriteComponent& sp, const WorldTransform& wt) {
            const math::Vec3 local = math::inverse(wt.matrix).transform_point({world.x, world.y, 0});
            const float x0 = -sp.pivot.x * sp.size.x, y0 = -sp.pivot.y * sp.size.y;
            if (local.x >= x0 && local.x <= x0 + sp.size.x && local.y >= y0 && local.y <= y0 + sp.size.y &&
                sp.layer >= best_layer) {
                best_layer = sp.layer;
                best = e;
            }
        });
        // Tilemaps are hit on painted cells.
        s.world().each<tilemap::TilemapComponent, WorldTransform>([&](Entity e, const tilemap::TilemapComponent& tm,
                                                                      const WorldTransform& wt) {
            if (tm.layer < best_layer) return;
            const math::Vec3 local = math::inverse(wt.matrix).transform_point({world.x, world.y, 0});
            const auto cx = static_cast<std::int32_t>(std::floor(local.x / std::max(tm.cell_size.x, 1e-6f)));
            const auto cy = static_cast<std::int32_t>(std::floor(local.y / std::max(tm.cell_size.y, 1e-6f)));
            if (tm.tile(cx, cy) >= 0) {
                best_layer = tm.layer;
                best = e;
            }
        });
        return best;
    }
    const render::Camera3D cam = camera_.camera3d();
    const math::Mat4 inv_vp = math::inverse(cam.projection(size.x / size.y) * cam.view);
    const float nx = pixel.x / size.x * 2.0f - 1.0f, ny = 1.0f - pixel.y / size.y * 2.0f;
    const math::Vec4 n4 = inv_vp * math::Vec4{nx, ny, 0.0f, 1.0f};
    const math::Vec4 f4 = inv_vp * math::Vec4{nx, ny, 1.0f, 1.0f};
    const math::Vec3 origin{n4.x / n4.w, n4.y / n4.w, n4.z / n4.w};
    const math::Vec3 far_point{f4.x / f4.w, f4.y / f4.w, f4.z / f4.w};
    float best_t = std::numeric_limits<float>::max();
    s.world().each<WorldTransform>([&](Entity e, const WorldTransform& wt) {
        bool has_mesh = false;
        const math::Aabb box = entity_bounds(e, &has_mesh);
        if (!has_mesh && !s.world().has<CameraComponent>(e) && !s.world().has<LightComponent>(e)) return;
        const math::Mat4 inv = math::inverse(wt.matrix);
        const math::Vec3 o = inv.transform_point(origin);
        const math::Vec3 d = inv.transform_point(far_point) - o;
        float t0 = 0.0f, t1 = 1.0f;
        const float ov[3] = {o.x, o.y, o.z}, dv[3] = {d.x, d.y, d.z};
        const float mn[3] = {box.min.x, box.min.y, box.min.z}, mx[3] = {box.max.x, box.max.y, box.max.z};
        for (int a = 0; a < 3; ++a) {
            if (std::abs(dv[a]) < 1e-9f) {
                if (ov[a] < mn[a] || ov[a] > mx[a]) return;
                continue;
            }
            float ta = (mn[a] - ov[a]) / dv[a], tb = (mx[a] - ov[a]) / dv[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return;
        }
        if (t0 < best_t) {
            best_t = t0;
            best = e;
        }
    });
    // Terrains are hit on their surface (t is a fraction of the near-far segment, like above).
    const float segment = math::length(far_point - origin);
    if (segment > 0.0f) {
        if (terrains_) terrains_->update(s);
        if (const auto hit = terrain::raycast(s, origin, (far_point - origin) * (1.0f / segment), segment);
            hit && hit->distance / segment < best_t)
            best = hit->entity;
    }
    return best;
}

// ---- play mode ---------------------------------------------------------------------------------------

void Editor::play() {
    if (play_state_ == PlayState::Paused) {
        if (replay_ && (replay_->done() || replay_->diverged())) return; // a finished replay stays paused
        play_state_ = PlayState::Playing;
        return;
    }
    if (play_state_ == PlayState::Playing) return;
    start_play(0, net_settings_.mode == NetPlaySettings::Mode::Host);
}

void Editor::play_host() {
    if (play_state_ != PlayState::Editing) return;
    net_settings_.mode = NetPlaySettings::Mode::Host;
    start_play(0, true);
}

Status Editor::set_net_settings(NetPlaySettings s) {
    if (!s.conditions.empty())
        if (auto c = net::parse_conditions(s.conditions); !c) return make_error(ErrorCode::InvalidArgument, "link conditions: " + c.error().message);
    if (s.insecure && !s.password.empty()) return make_error(ErrorCode::InvalidArgument, "a password needs encryption; turn off insecure");
    if (s.max_clients == 0 || s.max_clients > 64) return make_error(ErrorCode::InvalidArgument, "max clients must be 1-64");
    if (s.port == 0) return make_error(ErrorCode::InvalidArgument, "port must be 1-65535");
    if (s.editor_clients > 3) return make_error(ErrorCode::InvalidArgument, "in-editor clients must be 0-3");
    if (s.editor_clients > s.max_clients) return make_error(ErrorCode::InvalidArgument, "more in-editor clients than max clients");
    net_settings_ = std::move(s);
    return {};
}

std::string Editor::join_command() const {
    std::string cmd = "victus run \"";
    cmd += project_ ? project_->root().string() : std::string("<project>");
    cmd += std::format("\" --connect 127.0.0.1:{} --name player2", net_settings_.port);
    if (net_settings_.insecure) cmd += " --insecure";
    else if (!server_key_.empty()) cmd += " --server-key " + server_key_;
    if (!net_settings_.password.empty()) cmd += " --password <password>";
    return cmd;
}

Status Editor::start_hosting() {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project");
    net::UdpOptions uo;
    uo.port = net_settings_.port;
    uo.max_peers = net_settings_.max_clients + 4u; // room to tell extra peers the server is full
    auto transport = net::create_udp_transport(uo);
    if (!transport) return std::move(transport).error();
    std::unique_ptr<net::Transport> t = std::move(*transport);
    if (!net_settings_.conditions.empty()) {
        auto c = net::parse_conditions(net_settings_.conditions);
        if (!c) return std::move(c).error();
        t = net::with_conditions(std::move(t), *c);
    }
    server_key_.clear();
    if (!net_settings_.insecure) {
        // Same key file as `victus serve` / `victus run --host`, so pinned keys keep working.
        auto kp = net::load_or_create_key(project_->root() / ".victus" / "server.key");
        if (!kp) return std::move(kp).error();
        net::SecureOptions sec;
        sec.server_secret = kp->secret;
        sec.password = net_settings_.password;
        t = net::with_encryption(std::move(t), std::move(sec));
        server_key_ = net::key_to_hex(kp->public_key);
    }
    net::ServerOptions so;
    so.game_id = net::make_game_id(project_->settings().name, scene_path_);
    so.max_clients = net_settings_.max_clients;
    so.host_player = true;
    so.host_name = net_settings_.player_name;
    play_server_ = std::make_unique<net::NetServer>(std::move(t), so);
    play_server_->install(sim_);
    net_send_history_.clear();
    net_receive_history_.clear();
    net_sample_ = 0;
    return {};
}

void Editor::record() {
    if (play_state_ != PlayState::Editing) return;
    if (net_settings_.mode == NetPlaySettings::Mode::Host) notify("captures record offline sessions; recording without hosting");
    auto c = std::make_unique<gameplay::Capture>();
    c->seed = Random(static_cast<std::uint64_t>(platform::Platform::seconds() * 1e9)).next_u64() | 1;
    if (project_) c->project = project_->settings().name;
    c->scene = scene_path_;
    const auto seed = c->seed;
    start_play(seed);
    if (play_state_ != PlayState::Playing) return;
    recording_ = std::move(c);
    captured_input_ = {};
    sim_.input = &captured_input_;
    notify("Recording - Stop saves the capture");
}

Status Editor::replay(const std::filesystem::path& path) {
    if (play_state_ != PlayState::Editing) stop();
    auto c = gameplay::Capture::load(path);
    if (!c) return std::move(c).error();
    if (c->frames.empty()) return make_error(ErrorCode::InvalidArgument, "capture has no frames");
    if (!c->scene.empty() && c->scene != scene_path_)
        notify(std::format("capture was recorded in {}, replaying in {}", c->scene, scene_path_.empty() ? "an unsaved scene" : scene_path_), true);
    auto cap = std::make_unique<gameplay::Capture>(std::move(*c));
    start_play(cap->seed);
    if (play_state_ != PlayState::Playing) return make_error(ErrorCode::InvalidArgument, "cannot start play mode");
    replay_capture_ = std::move(cap);
    replay_ = std::make_unique<gameplay::CaptureReplay>(*replay_capture_);
    replay_result_.clear();
    replay_diverged_ = false;
    sim_.input = &replay_->input();
    notify(std::format("Replaying {} ({} frames)", path.filename().string(), replay_capture_->frames.size()));
    return {};
}

std::vector<std::filesystem::path> Editor::captures() const {
    std::vector<std::filesystem::path> out;
    if (!project_) return out;
    std::error_code ec;
    const auto dir = project_->root() / ".victus" / "captures";
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".vcapture") out.push_back(e.path());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        const auto ta = std::filesystem::last_write_time(a, e1), tb = std::filesystem::last_write_time(b, e2);
        return ta != tb ? ta > tb : a.filename() > b.filename();
    });
    return out;
}

void Editor::tick_play(float dt) {
    if (replay_) {
        if (replay_->done() || replay_->diverged()) return;
        dt = replay_->begin_tick();
        sim_.tick(*play_scene_, dt);
        if (!replay_->end_tick(*play_scene_)) {
            replay_diverged_ = true;
            replay_result_ = "Replay diverged at " + replay_->mismatch();
            play_state_ = PlayState::Paused;
            notify(replay_result_, true);
        } else if (replay_->done()) {
            replay_result_ = std::format("Replay matched: {} frames, {} digests", replay_->frame(), replay_->digests_checked());
            play_state_ = PlayState::Paused;
            notify(replay_result_);
        }
        return;
    }
    if (recording_) {
        // The live input is sampled once per frame and fed back through a ReplayInput, so the
        // recording sees exactly the states its replay will.
        const auto state = input_ ? gameplay::sample_input(*input_) : gameplay::InputState{};
        recording_->add_frame(state, dt);
        captured_input_.set(state);
        sim_.tick(*play_scene_, dt);
        captured_input_.end_frame();
        recording_->after_tick(*play_scene_);
        return;
    }
    sim_.tick(*play_scene_, dt);
    tick_editor_clients(dt);
    if (play_server_) {
        // Bandwidth history for the Network window (4 samples per second, last 30 s).
        if ((net_sample_ += dt) >= 0.25f) {
            net_sample_ = 0;
            const auto d = play_server_->diagnostics();
            net_send_history_.push_back(d.send_kbps);
            net_receive_history_.push_back(d.receive_kbps);
            constexpr std::size_t kSamples = 120;
            if (net_send_history_.size() > kSamples) net_send_history_.erase(net_send_history_.begin());
            if (net_receive_history_.size() > kSamples) net_receive_history_.erase(net_receive_history_.begin());
        }
    }
}

void Editor::start_play(std::uint64_t seed, bool networked) {
    gizmo_cancel(); // a half-finished drag never leaks into play mode
    end_sequence_preview(); // play the authored scene, not the previewed pose
    end_animation_preview();
    // Commit any in-flight inspector edit to the authored scene first.
    if (pending_dirty_ && !pending_before_.empty()) record_edit("Edit", std::move(pending_before_));
    pending_before_.clear();
    pending_dirty_ = false;
    auto copy = std::make_unique<Scene>();
    if (auto st = copy->load(scene_->save()); !st.ok()) {
        notify("cannot start play mode: " + st.error().message, true);
        return;
    }
    play_scene_ = std::move(copy);
    reset_simulation();
    play_physics_.reset();
    {
        physics::PhysicsSettings ps;
        ps.jobs = &workers();
        if (auto pw = physics::PhysicsWorld::create(ps)) play_physics_ = std::move(pw).value();
        else notify("physics disabled: " + pw.error().message, true);
    }
    if (db_) {
        script::HostOptions so;
        so.assets_root = db_->assets_dir();
        so.watch_files = seed == 0; // captures need the scripts they were recorded with
        so.trace_graphs = true;     // the Graph Editor highlights running nodes (ADR-0060)
        so.random_seed = seed;
        if (project_) {
            so.input_map = project_->settings().input;
            so.save_version = project_->settings().save_version;
            so.default_locale = project_->settings().default_locale;
            so.save_directory = project_->dev_save_directory();
            so.project_root = project_->root();
            so.bridge_launcher = [project = &*project_, saves = so.save_directory](std::string_view name) {
                return project::bridge_launch(*project, name, saves);
            };
        }
        // Scene transitions while playing replace only the play copy; the authored scene and
        // its path stay as they are, and Stop returns to it.
        so.on_scene_loaded = [this](scene::Scene&, std::string_view path) {
            if (play_physics_) play_physics_->clear();
            if (auto* a = audio()) a->stop_all();
            notify("Play: scene changed to " + std::string(path));
        };
        if (auto host = script::ScriptHost::create(so)) {
            play_scripts_ = std::move(host).value();
            play_scripts_->set_current_scene(scene_path_);
            script::add_script_systems(sim_, *play_scripts_);
        } else {
            notify("scripts disabled: " + host.error().message, true);
        }
    }
    if (play_physics_) physics::add_physics_systems(sim_, *play_physics_);
    play_animation_ = std::make_unique<animation::AnimationSystem>(
        animation::AnimationSettings{.provider = db_ ? assets::animation_provider(*db_) : animation::SetProvider{},
                                     .controllers = db_ ? animation::directory_loader(db_->assets_dir()) : animation::ControllerLoader{},
                                     .sheets = db_ ? assets::sprite_sheet_provider(*db_) : animation::SheetProvider{},
                                     .jobs = &workers()});
    animation::add_animation_systems(sim_, *play_animation_);
    if (!tilemaps_) tilemaps_ = std::make_unique<tilemap::TilemapSystem>();
    tilemap::add_tilemap_systems(sim_, *tilemaps_);
    if (!terrains_) terrains_ = std::make_unique<terrain::TerrainSystem>();
    terrain::add_terrain_systems(sim_, *terrains_);
    play_particles_ = std::make_unique<particles::ParticleSystem>(particles::ParticleSettings{.jobs = &workers()});
    particles::add_particle_systems(sim_, *play_particles_);
    play_navigation_ = std::make_unique<navigation::NavigationSystem>();
    navigation::add_navigation_systems(sim_, *play_navigation_);
    play_ai_ = std::make_unique<ai::AiSystem>(db_ ? ai::directory_loader(db_->assets_dir()) : ai::TreeLoader{});
    ai::add_ai_systems(sim_, *play_ai_);
    play_sequencer_ = std::make_unique<sequencer::SequencerSystem>(db_ ? sequencer::directory_loader(db_->assets_dir()) : sequencer::SequenceLoader{});
    sequencer::add_sequencer_systems(sim_, *play_sequencer_);
    if (auto* a = audio()) {
        a->stop_all();
        audio::add_audio_systems(sim_, *a);
    }
    if (networked) {
        if (auto st = start_hosting(); !st.ok()) {
            notify("cannot host: " + st.error().message, true);
            play_server_.reset();
            play_scene_.reset();
            reset_simulation();
            play_scripts_.reset();
            play_physics_.reset();
            play_animation_.reset();
            play_particles_.reset();
            play_navigation_.reset();
            play_ai_.reset();
            play_sequencer_.reset();
            return;
        }
    }
    if (play_server_) start_editor_clients();
    route_input();
    focus_viewport_ = true;
    play_state_ = PlayState::Playing;
    if (play_server_ && !play_clients_.empty())
        notify(std::format("Hosting on UDP port {} with {} in-editor client{} - View switches between players", net_settings_.port,
                           play_clients_.size(), play_clients_.size() == 1 ? "" : "s"));
    else if (play_server_)
        notify(std::format("Hosting on UDP port {} ({}) - others join with: {}", net_settings_.port,
                           server_key_.empty() ? "insecure" : "encrypted", join_command()));
    else
        notify("Playing - changes made now are discarded on Stop");
}

audio::AudioEngine* Editor::audio() {
    if (!audio_ && !audio_failed_ && db_) {
        audio::EngineOptions o;
        o.assets_root = db_->assets_dir();
        o.null_device = audio::audio_disabled_by_env();
        if (auto a = audio::AudioEngine::create(o)) audio_ = std::move(a).value();
        else {
            audio_failed_ = true;
            notify("audio disabled: " + a.error().message, true);
        }
    }
    return audio_.get();
}

jobs::JobSystem& Editor::workers() {
    if (!jobs_) jobs_ = std::make_unique<jobs::JobSystem>(0);
    return *jobs_;
}

void Editor::reset_simulation() {
    // Systems capture the script host by reference, so the simulation is rebuilt (not just reset)
    // whenever the host changes.
    sim_ = gameplay::Simulation();
    gameplay::add_builtin_systems(sim_);
    gameui::add_ui_systems(sim_);
    sim_.input = game_input_override_ ? game_input_override_ : input_.get();
}

void Editor::set_game_input_override(gameplay::InputSource* input) {
    game_input_override_ = input;
    route_input();
}

void Editor::pause() {
    if (play_state_ == PlayState::Playing) play_state_ = PlayState::Paused;
}

void Editor::stop() {
    if (play_state_ == PlayState::Editing) return;
    if (recording_ && play_scene_ && !recording_->frames.empty()) {
        recording_->finish(*play_scene_);
        if (project_) {
            const auto now = std::chrono::system_clock::now();
            const std::string stem = scene_path_.empty() ? std::string("scene") : std::filesystem::path(scene_path_).stem().string();
            auto path = project_->root() / ".victus" / "captures" /
                        std::format("{}-{:%Y%m%d-%H%M%S}.vcapture", stem, std::chrono::floor<std::chrono::seconds>(now));
            for (int i = 2; std::filesystem::exists(path); ++i)
                path.replace_filename(std::format("{}-{:%Y%m%d-%H%M%S}-{}.vcapture", stem,
                                                  std::chrono::floor<std::chrono::seconds>(now), i));
            if (auto st = recording_->save(path); st) {
                last_capture_ = path;
                notify(std::format("Saved capture {} ({} frames)", path.filename().string(), recording_->frames.size()));
            } else {
                notify("cannot save capture: " + st.error().message, true);
            }
        }
    }
    recording_.reset();
    replay_.reset();
    replay_capture_.reset();
    stop_editor_clients();
    if (play_server_) {
        play_server_->shutdown(sim_.time().time); // joined players see "server shutting down"
        play_server_.reset();
    }
    play_scene_.reset();
    reset_simulation();
    play_scripts_.reset();
    play_physics_.reset();
    play_animation_.reset();
    play_particles_.reset();
    play_navigation_.reset();
    play_ai_.reset();
    play_sequencer_.reset();
    if (audio_) audio_->stop_all();
    if (input_) input_->set_enabled(false);
    play_state_ = PlayState::Editing;
    pending_before_.clear();
    pending_dirty_ = false;
    prune_selection();
    notify("Stopped - scene restored");
}

bool Editor::request_quit() {
    if (!dirty() || !project_) {
        quit_confirmed_ = true;
        return true;
    }
    pending_action_ = Pending::Quit;
    return false;
}

// ---- rendering -----------------------------------------------------------------------------------------

Status Editor::ensure_viewport(std::uint32_t w, std::uint32_t h) {
    if (viewport_tex_ && w == viewport_w_ && h == viewport_h_) return {};
    if (viewport_tex_) gpu_.device().destroy(viewport_tex_);
    rhi::TextureDesc d;
    d.name = "editor_viewport";
    d.format = rhi::Format::RGBA8_UNORM;
    d.width = w;
    d.height = h;
    d.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    auto t = gpu_.device().create_texture(d);
    if (!t) return std::move(t).error();
    viewport_tex_ = t.value();
    viewport_w_ = w;
    viewport_h_ = h;
    return {};
}

void Editor::draw_overlays_3d(render::SceneRenderer& r) {
    Scene& s = active_scene();
    if (play_state_ == PlayState::Editing) r.grid(20.0f, 1.0f, {0.35f, 0.36f, 0.4f, 1});
    for (const Uuid& u : selection_) {
        const Entity e = s.find(u);
        if (e.is_null()) continue;
        const math::Aabb local = entity_bounds(e, nullptr);
        const math::Mat4 m = s.world_matrix(e);
        // Oriented box: transform the 8 corners and draw the 12 edges.
        math::Vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = m.transform_point({(i & 1) ? local.max.x : local.min.x, (i & 2) ? local.max.y : local.min.y,
                                      (i & 4) ? local.max.z : local.min.z});
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : kEdges) r.line(c[edge[0]], c[edge[1]], {1.0f, 0.55f, 0.1f, 1});
    }
    draw_terrain_overlay(r);
    if (show_colliders_) {
        collider_scratch_.clear();
        physics::collider_lines(s, collider_scratch_);
        for (const auto& l : collider_scratch_) r.line(l.a, l.b, l.color);
    }
    if (show_navmesh_) {
        navigation::NavigationSystem* nav = play_state_ == PlayState::Editing ? nullptr : play_navigation_.get();
        if (!nav) {
            if (!edit_navigation_) edit_navigation_ = std::make_unique<navigation::NavigationSystem>();
            nav = edit_navigation_.get();
            // Rebuilds only when static colliders (or NavMeshSurface settings) changed.
            if (auto st = nav->ensure_built(s); !st) notify(st.error().message, true);
        } else if (!nav->valid()) {
            (void)nav->ensure_built(s);
        }
        nav_scratch_.clear();
        nav->debug_lines(nav_scratch_);
        for (const auto& l : nav_scratch_) r.line(l.a, l.b, l.color);
    }
    if (show_ai_ && play_ai_ && play_state_ != PlayState::Editing) {
        ai_scratch_.clear();
        play_ai_->debug_lines(s, ai_scratch_);
        for (const auto& l : ai_scratch_) r.line(l.a, l.b, l.color);
    }
}

void Editor::draw_overlays_2d(render::SpriteBatch& b) {
    Scene& s = active_scene();
    const float px = 1.0f / std::max(last_cam2d_.pixels_per_unit * last_cam2d_.zoom, 0.001f);
    for (const Uuid& u : selection_) {
        const Entity e = s.find(u);
        if (e.is_null()) continue;
        const math::Aabb local = entity_bounds(e, nullptr);
        const math::Mat4 m = s.world_matrix(e);
        const math::Vec3 a = m.transform_point({local.min.x, local.min.y, 0});
        const math::Vec3 bq = m.transform_point({local.max.x, local.min.y, 0});
        const math::Vec3 c = m.transform_point({local.max.x, local.max.y, 0});
        const math::Vec3 d = m.transform_point({local.min.x, local.max.y, 0});
        const math::Color orange{1.0f, 0.55f, 0.1f, 1};
        b.draw_line({a.x, a.y}, {bq.x, bq.y}, 2 * px, orange, 1000);
        b.draw_line({bq.x, bq.y}, {c.x, c.y}, 2 * px, orange, 1000);
        b.draw_line({c.x, c.y}, {d.x, d.y}, 2 * px, orange, 1000);
        b.draw_line({d.x, d.y}, {a.x, a.y}, 2 * px, orange, 1000);
        // 2D light reach (radius circle) and, for spots, the outer cone edges.
        if (const auto* l = s.world().get<gameplay::Light2DComponent>(e); l && l->type != gameplay::Light2DComponent::Type::Global) {
            const math::Vec3 o = m.transform_point({});
            const math::Vec3 ax = m.transform_vector({1, 0, 0}), ay = m.transform_vector({0, 1, 0});
            const float r = l->radius * std::max(std::hypot(ax.x, ax.y), std::hypot(ay.x, ay.y));
            const math::Color yellow{1.0f, 0.9f, 0.3f, 0.8f};
            constexpr int kSegments = 48;
            for (int i = 0; i < kSegments; ++i) {
                const float t0 = 6.2831853f * static_cast<float>(i) / kSegments, t1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
                b.draw_line({o.x + r * std::cos(t0), o.y + r * std::sin(t0)}, {o.x + r * std::cos(t1), o.y + r * std::sin(t1)}, 1.5f * px,
                            yellow, 1000);
            }
            if (l->type == gameplay::Light2DComponent::Type::Spot) {
                const float base = std::atan2(-ay.y, -ay.x), outer = math::radians(l->outer_angle_degrees);
                for (const float a2 : {base - outer, base + outer})
                    b.draw_line({o.x, o.y}, {o.x + r * std::cos(a2), o.y + r * std::sin(a2)}, 1.5f * px, yellow, 1000);
            }
        }
    }
    draw_tile_overlay(b, px);
    if (show_colliders_) {
        collider_scratch_.clear();
        physics::collider_lines(s, collider_scratch_);
        for (const auto& l : collider_scratch_) b.draw_line({l.a.x, l.a.y}, {l.b.x, l.b.y}, 1.5f * px, l.color, 999);
    }
}

void Editor::apply_quality() {
    render::RenderSettings settings = render::RenderSettings::preset(quality_);
    if (project_ && project_->settings().render_scale > 0.0f) settings.render_scale = project_->settings().render_scale;
    settings.debug_view = debug_view_;
    view_->renderer().set_settings(settings);
    view_->renderer().reset_adaptation();
}

void Editor::set_debug_view(render::DebugView view) {
    if (debug_view_ == view) return;
    debug_view_ = view;
    apply_quality();
}

Result<render::OverdrawStats> Editor::read_overdraw() { return view_->renderer().read_overdraw(); }

Status Editor::set_quality(render::QualityTier tier) {
    quality_ = tier;
    apply_quality();
    if (!project_) return {};
    project_->settings().render_quality = std::string(render::quality_tier_name(tier));
    VICTUS_TRY_STATUS(project_->save());
    notify(std::string("Rendering quality: ") + std::string(render::quality_tier_name(tier)));
    return {};
}

const gameplay::InputMap* Editor::input_map() const noexcept {
    return project_ ? &project_->settings().input : nullptr;
}

Status Editor::set_input_map(gameplay::InputMap map) {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    auto parsed = gameplay::InputMap::from_json(map.to_json()); // names, duplicates, syntax
    if (!parsed) return parsed.error();
    for (const auto& a : map.actions)
        if (a.name.empty()) return make_error(ErrorCode::InvalidArgument, "input action names cannot be empty");
    const auto problems =
        map.validate([](std::string_view k) { return platform::key_from_name(k) != platform::Key::Unknown; });
    if (!problems.empty()) return make_error(ErrorCode::InvalidArgument, problems.front());
    gameplay::InputMap previous = project_->settings().input;
    project_->settings().input = map;
    if (auto st = project_->save(); !st) {
        project_->settings().input = std::move(previous);
        return st;
    }
    if (play_scripts_) play_scripts_->set_input_map(std::move(map));
    return {};
}

void Editor::begin_binding_capture(std::string action, bool negative) {
    capture_action_ = std::move(action);
    capture_negative_ = negative;
}

bool Editor::capture_from(const gameplay::InputSource& input) {
    if (capture_action_.empty() || !project_) return false;
    if (input.key_pressed("Escape")) {
        capture_action_.clear();
        return true;
    }
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (auto i = 1u; i < static_cast<unsigned>(platform::Key::Count); ++i) {
            const auto key = static_cast<platform::Key>(i);
            if (key != platform::Key::Escape) k.emplace_back(platform::key_name(key));
        }
        return k;
    }();
    const auto binding = gameplay::capture_binding(input, keys);
    if (!binding) return false;
    gameplay::InputMap map = project_->settings().input;
    for (auto& a : map.actions) {
        if (a.name != capture_action_) continue;
        auto& list = capture_negative_ ? a.negative : a.positive;
        if (std::find(list.begin(), list.end(), *binding) == list.end()) list.push_back(*binding);
    }
    const std::string text = gameplay::binding_to_string(*binding);
    if (auto st = set_input_map(std::move(map)); !st) input_error_ = st.error().message;
    else notify("Bound " + text + " to " + capture_action_);
    capture_action_.clear();
    return true;
}

const render::SceneStats& Editor::render_stats() const { return view_->renderer().stats(); }

Status Editor::render(rhi::CommandBuffer& cmd) {
    VICTUS_PROFILE_SCOPE("editor.render");
    if (!project_ || !cache_) return {};
    const std::uint32_t w = forced_w_ ? forced_w_ : viewport_w_;
    const std::uint32_t h = forced_h_ ? forced_h_ : viewport_h_;
    if (w == 0 || h == 0) return {};
    VICTUS_TRY_STATUS(ensure_viewport(w, h));
    Scene& s = active_scene();
    s.update_transforms();
    runtime::SceneViewOptions opt;
    const bool game_camera = play_state_ != PlayState::Editing;
    const render::Camera3D cam3 = camera_.camera3d();
    last_cam2d_ = camera_.camera2d(static_cast<float>(h));
    if (!game_camera) {
        opt.camera_3d = &cam3;
        opt.camera_2d = &last_cam2d_;
        opt.overlay_3d = [this](render::SceneRenderer& r) { draw_overlays_3d(r); };
        opt.overlay_2d = [this](render::SpriteBatch& b) { draw_overlays_2d(b); };
    }
    auto stats = view_->render(cmd, s, *cache_, viewport_tex_, w, h, opt);
    if (!stats) return std::move(stats).error();
    last_view_stats_ = *stats;
    if (thumbnails_ && db_ && thumbnail_budget_ > 0) {
        VICTUS_PROFILE_SCOPE("editor.thumbnails");
        auto n = thumbnails_->render_pending(cmd, *cache_, *db_, thumbnail_budget_);
        if (!n) return std::move(n).error();
    }
    return {};
}

std::optional<Thumbnail> Editor::asset_thumbnail(const Uuid& asset) {
    if (!thumbnails_ || !db_ || !cache_) return std::nullopt;
    const assets::AssetRecord* rec = db_->find(asset);
    if (!rec) return std::nullopt;
    return thumbnails_->get(*rec, *cache_, *db_);
}

void Editor::set_asset_view(bool grid, float size) noexcept {
    asset_grid_ = grid;
    asset_tile_size_ = std::clamp(size, 32.0f, 256.0f);
}

} // namespace victus::editor
