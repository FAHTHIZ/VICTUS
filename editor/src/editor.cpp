// SPDX-License-Identifier: Apache-2.0
// Editor core: projects, scenes, editing operations, undo, selection, play mode, rendering.
#include "victus/editor/editor.hpp"

#include "victus/core/profiler.hpp"
#include "victus/platform/platform.hpp"

#include <algorithm>
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
    project::register_components();
    if (platform) ed->input_ = std::make_unique<player::PlatformInput>(platform->input());
    ed->reset_simulation();
    ed->recent_ = std::make_unique<project::RecentProjects>(options.recent_file);
    ed->log_sink_ = std::make_shared<log::RingSink>(4096);
    log::logger().add_sink(ed->log_sink_);
    auto view = runtime::SceneView::create(gpu, rhi::Format::RGBA8_UNORM);
    if (!view) return std::move(view).error();
    ed->view_ = std::move(view).value();
    ed->scene_ = std::make_unique<Scene>();
    ed->new_location_ = (options.default_project_dir.empty() ? default_projects_dir() : options.default_project_dir).string();
    ed->update_title();
    return ed;
}

Editor::~Editor() {
    if (log_sink_) log::logger().remove_sink(log_sink_);
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
    return {};
}

Status Editor::create_project(const fs::Path& directory, std::string_view name, std::string_view template_id) {
    auto p = project::Project::create(directory, name, template_id);
    if (!p) return std::move(p).error();
    return open_project(directory);
}

void Editor::close_project() {
    stop();
    selection_.clear();
    undo_.clear();
    cache_.reset();
    edit_scripts_.reset();
    audio_.reset(); // clips resolve against the project's assets
    audio_failed_ = false;
    db_.reset();
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
    if (report.imported > 0) cache_->clear();
    notify(std::to_string(report.imported) + " imported, " + std::to_string(report.up_to_date) + " up to date, " +
               std::to_string(report.failed) + " failed",
           report.failed > 0);
    return {};
}

// ---- scenes ------------------------------------------------------------------------------------

void Editor::set_scene(std::unique_ptr<Scene> s, std::string path) {
    stop();
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
    return {};
}

Status Editor::save_scene() {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (scene_path_.empty()) return make_error(ErrorCode::InvalidArgument, "scene has no path; use Save As");
    auto path = project_->resolve(scene_path_);
    if (!path) return std::move(path).error();
    std::error_code ec;
    sfs::create_directories(path->parent_path(), ec);
    VICTUS_TRY_STATUS(scene_->save_file(*path));
    undo_.mark_saved();
    update_title();
    notify("Saved " + scene_path_);
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
    (void)project::build_template_scene("3d", *s);
    // A new scene starts with camera + light only.
    std::vector<Entity> remove;
    for (const Entity e : s->roots()) {
        if (!s->world().has<CameraComponent>(e) && !s->world().has<LightComponent>(e)) remove.push_back(e);
        else if (s->world().get<LightComponent>(e)->type == LightComponent::Type::Point) remove.push_back(e);
    }
    for (const Entity e : remove) s->destroy(e);
    s->name = "New Scene";
    set_scene(std::move(s), "");
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
    static constexpr const char* kNames[] = {"Entity", "Cube", "Sphere", "Plane", "Cylinder",
                                             "Camera", "Directional Light", "Point Light", "Sprite"};
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
    case Primitive::Sprite: w.add<SpriteComponent>(e); break;
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
    auto before = capture(s, {s.uuid(e)});
    n->value = std::move(name);
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
    const Scene& s = play_scene_ ? *play_scene_ : *scene_;
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
    return best;
}

// ---- play mode ---------------------------------------------------------------------------------------

void Editor::play() {
    if (play_state_ == PlayState::Paused) {
        play_state_ = PlayState::Playing;
        return;
    }
    if (play_state_ == PlayState::Playing) return;
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
        so.watch_files = true;
        if (auto host = script::ScriptHost::create(so)) {
            play_scripts_ = std::move(host).value();
            script::add_script_systems(sim_, *play_scripts_);
        } else {
            notify("scripts disabled: " + host.error().message, true);
        }
    }
    if (play_physics_) physics::add_physics_systems(sim_, *play_physics_);
    if (auto* a = audio()) {
        a->stop_all();
        audio::add_audio_systems(sim_, *a);
    }
    focus_viewport_ = true;
    play_state_ = PlayState::Playing;
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
    sim_.input = input_.get();
}

void Editor::pause() {
    if (play_state_ == PlayState::Playing) play_state_ = PlayState::Paused;
}

void Editor::stop() {
    if (play_state_ == PlayState::Editing) return;
    play_scene_.reset();
    reset_simulation();
    play_scripts_.reset();
    play_physics_.reset();
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
    if (show_colliders_) {
        collider_scratch_.clear();
        physics::collider_lines(s, collider_scratch_);
        for (const auto& l : collider_scratch_) r.line(l.a, l.b, l.color);
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
    }
    if (show_colliders_) {
        collider_scratch_.clear();
        physics::collider_lines(s, collider_scratch_);
        for (const auto& l : collider_scratch_) b.draw_line({l.a.x, l.a.y}, {l.b.x, l.b.y}, 1.5f * px, l.color, 999);
    }
}

void Editor::apply_quality() {
    render::RenderSettings settings = render::RenderSettings::preset(quality_);
    if (project_ && project_->settings().render_scale > 0.0f) settings.render_scale = project_->settings().render_scale;
    view_->renderer().set_settings(settings);
    view_->renderer().reset_adaptation();
}

Status Editor::set_quality(render::QualityTier tier) {
    quality_ = tier;
    apply_quality();
    if (!project_) return {};
    project_->settings().render_quality = std::string(render::quality_tier_name(tier));
    VICTUS_TRY_STATUS(project_->save());
    notify(std::string("Rendering quality: ") + std::string(render::quality_tier_name(tier)));
    return {};
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
    return {};
}

} // namespace victus::editor
