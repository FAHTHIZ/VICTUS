// SPDX-License-Identifier: Apache-2.0
// Editor UI: launcher, dockspace workspace and panels (Dear ImGui).
#include "victus/editor/editor.hpp"

#include "victus/core/build_info.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

namespace {
constexpr const char* kEntityPayload = "VICTUS_ENTITY";
constexpr const char* kAssetPayload = "VICTUS_ASSET";
constexpr const char* kViewportWindow = "Viewport";
constexpr const char* kHierarchyWindow = "Hierarchy";
constexpr const char* kInspectorWindow = "Inspector";
constexpr const char* kAssetsWindow = "Assets";
constexpr const char* kConsoleWindow = "Console";

ImVec4 accent() { return ImVec4(1.0f, 0.55f, 0.1f, 1.0f); }

bool color_edit(const char* label, math::Color& c, bool alpha = true) {
    float v[4] = {c.r, c.g, c.b, c.a};
    const bool changed = alpha ? ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_Float)
                               : ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
    if (changed) c = {v[0], v[1], v[2], v[3]};
    return changed;
}

bool vec3_edit(const char* label, math::Vec3& v, float speed = 0.05f) {
    float f[3] = {v.x, v.y, v.z};
    const bool changed = ImGui::DragFloat3(label, f, speed, 0, 0, "%.3f");
    if (changed) v = {f[0], f[1], f[2]};
    return changed;
}

bool vec2_edit(const char* label, math::Vec2& v, float speed = 0.05f) {
    float f[2] = {v.x, v.y};
    const bool changed = ImGui::DragFloat2(label, f, speed, 0, 0, "%.3f");
    if (changed) v = {f[0], f[1]};
    return changed;
}

std::string asset_label(assets::AssetDatabase* db, const Uuid& id) {
    if (id.is_nil()) return "(none)";
    if (db)
        if (const auto* r = db->find(id)) return r->path;
    return "(missing) " + id.to_string();
}

const char* kind_icon(assets::AssetKind k) {
    switch (k) {
    case assets::AssetKind::Texture: return "[tex]";
    case assets::AssetKind::Model: return "[mdl]";
    case assets::AssetKind::Scene: return "[scn]";
    default: return "[file]";
    }
}
} // namespace

// ---- frame --------------------------------------------------------------------------------------

void Editor::frame(float dt) {
    if (dt > 0) fps_ = fps_ <= 0 ? 1.0f / dt : fps_ * 0.95f + 0.05f / dt;
    if (status_time_ > 0) status_time_ -= dt;
    if (play_state_ == PlayState::Playing && play_scene_) sim_.tick(*play_scene_, dt);
    if (!project_) draw_launcher();
    else draw_workspace(dt);
    draw_modals();
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
}

// ---- launcher -----------------------------------------------------------------------------------

void Editor::draw_launcher() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 24));
    ImGui::Begin("##launcher", nullptr, flags);
    ImGui::PopStyleVar();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextColored(accent(), "VICTUS");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("Engine %s  -  offline, no account required", build::version());
    ImGui::Separator();
    ImGui::Spacing();

    const float col_w = std::max(320.0f, ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::BeginChild("##new", ImVec2(col_w, 0), ImGuiChildFlags_None);
    ImGui::TextUnformatted("Create a new project");
    ImGui::Spacing();
    for (const auto& t : project::templates()) {
        const bool sel = new_template_ == t.id;
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable("##tpl", sel, ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight() * 2.6f)))
            new_template_ = t.id;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) new_template_ = t.id;
        ImGui::SameLine(12);
        ImGui::BeginGroup();
        ImGui::TextColored(sel ? accent() : ImVec4(1, 1, 1, 1), "%s", t.display_name.c_str());
        ImGui::TextDisabled("%s", t.description.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::InputText("Name", &new_name_);
    ImGui::InputText("Location", &new_location_);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##new") && platform_) {
        platform_->show_folder_dialog(window_, new_location_, [this](std::string path) {
            if (!path.empty()) new_location_ = std::move(path);
        });
    }
    const fs::Path target = fs::Path(new_location_) / new_name_;
    ImGui::TextDisabled("Folder: %s", target.string().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Create Project", ImVec2(180, 0))) {
        launcher_error_.clear();
        if (new_name_.empty()) launcher_error_ = "Enter a project name.";
        else if (auto st = create_project(target, new_name_, new_template_); !st.ok()) launcher_error_ = st.error().message;
    }
    if (!launcher_error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", launcher_error_.c_str());
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##open", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::TextUnformatted("Open a project");
    ImGui::Spacing();
    ImGui::InputText("##openpath", &open_path_);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##open") && platform_) {
        platform_->show_folder_dialog(window_, open_path_.empty() ? new_location_ : open_path_, [this](std::string path) {
            if (path.empty()) return;
            open_path_ = path;
            if (auto st = open_project(path); !st.ok()) launcher_error_ = st.error().message;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        launcher_error_.clear();
        if (auto st = open_project(open_path_); !st.ok()) launcher_error_ = st.error().message;
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Recent");
    ImGui::Separator();
    if (recent_->entries().empty()) ImGui::TextDisabled("No recent projects yet.");
    fs::Path to_open, to_remove;
    for (const fs::Path& p : recent_->entries()) {
        const bool exists = project::Project::is_project_dir(p);
        ImGui::PushID(p.string().c_str());
        if (ImGui::Selectable(p.filename().string().c_str(), false, exists ? 0 : ImGuiSelectableFlags_Disabled)) to_open = p;
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove from list")) to_remove = p;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", p.string().c_str(), exists ? "" : "  (missing)");
        ImGui::PopID();
    }
    if (!to_remove.empty()) {
        recent_->remove(to_remove);
        (void)recent_->save();
    }
    if (!to_open.empty()) {
        launcher_error_.clear();
        if (auto st = open_project(to_open); !st.ok()) launcher_error_ = st.error().message;
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- workspace ----------------------------------------------------------------------------------

void Editor::build_default_layout(unsigned int dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(kInspectorWindow, right);
    ImGui::DockBuilderDockWindow(kAssetsWindow, bottom);
    ImGui::DockBuilderDockWindow(kConsoleWindow, bottom);
    ImGui::DockBuilderDockWindow(kViewportWindow, center);
    ImGui::DockBuilderFinish(dockspace_id);
}

void Editor::draw_workspace(float) {
    draw_menu_bar();
    draw_toolbar();
    const float status_h = ImGui::GetFrameHeight();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - status_h));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##workspace", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    const ImGuiID dock_id = ImGui::GetID("VictusDockSpace");
    if (!layout_built_) {
        layout_built_ = true;
        if (!ImGui::DockBuilderGetNode(dock_id) || ImGui::DockBuilderGetNode(dock_id)->IsLeafNode())
            build_default_layout(dock_id);
    }
    ImGui::DockSpace(dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    draw_viewport();
    draw_hierarchy();
    draw_inspector();
    draw_assets();
    draw_console();
    draw_status_bar();
    handle_shortcuts();
}

void Editor::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool editing = play_state_ == PlayState::Editing;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", nullptr, false, editing)) {
            new_scene();
        }
        if (ImGui::BeginMenu("Open Scene", editing && project_)) {
            for (const std::string& s : project_->scenes())
                if (ImGui::MenuItem(s.c_str(), nullptr, s == scene_path_))
                    if (auto st = open_scene(s); !st.ok()) notify(st.error().message, true);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editing)) {
            if (scene_path_.empty()) show_save_as_ = true;
            else if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
        }
        if (ImGui::MenuItem("Save Scene As...", nullptr, false, editing)) show_save_as_ = true;
        if (ImGui::MenuItem("Set As Startup Scene", nullptr, false, editing && !scene_path_.empty())) {
            project_->settings().startup_scene = scene_path_;
            if (auto st = project_->save(); !st.ok()) notify(st.error().message, true);
            else notify("Startup scene: " + scene_path_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close Project")) {
            if (dirty()) pending_action_ = Pending::CloseProject;
            else close_project();
        }
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            if (request_quit() && platform_) platform_->request_quit();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const SceneCommand* u = undo_.next_undo();
        const SceneCommand* r = undo_.next_redo();
        if (ImGui::MenuItem(u ? ("Undo " + u->label).c_str() : "Undo", "Ctrl+Z", false, editing && u)) undo();
        if (ImGui::MenuItem(r ? ("Redo " + r->label).c_str() : "Redo", "Ctrl+Y", false, editing && r)) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection_.empty())) duplicate_selected();
        if (ImGui::MenuItem("Delete", "Del", false, !selection_.empty())) delete_selected();
        if (ImGui::MenuItem("Frame Selected", "F", false, !selection_.empty())) frame_selected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
        const auto item = [&](const char* label, Primitive p) {
            if (ImGui::MenuItem(label)) create_entity(p);
        };
        item("Empty", Primitive::Empty);
        ImGui::Separator();
        item("Cube", Primitive::Cube);
        item("Sphere", Primitive::Sphere);
        item("Plane", Primitive::Plane);
        item("Cylinder", Primitive::Cylinder);
        item("Sprite", Primitive::Sprite);
        ImGui::Separator();
        item("Camera", Primitive::Camera);
        item("Directional Light", Primitive::DirectionalLight);
        item("Point Light", Primitive::PointLight);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Assets")) {
        if (ImGui::MenuItem("Reimport All")) (void)reimport_assets();
        if (ImGui::MenuItem("Copy Assets Folder Path") && platform_) platform_->set_clipboard(project_->assets_dir().string());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset Layout")) layout_built_ = false;
        ImGui::MenuItem("UI Demo Window", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::draw_toolbar() {
    // A thin bar under the menu holding play controls, centred.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    const float h = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, h, flags)) {
        if (ImGui::BeginMenuBar()) {
            const float w = 210.0f;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - w) * 0.5f));
            const bool editing = play_state_ == PlayState::Editing;
            if (editing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.45f, 1));
                if (ImGui::MenuItem("> Play")) play();
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1));
                if (ImGui::MenuItem("[] Stop")) stop();
                ImGui::PopStyleColor();
                if (play_state_ == PlayState::Playing) {
                    if (ImGui::MenuItem("|| Pause")) pause();
                } else if (ImGui::MenuItem("> Resume")) {
                    play();
                }
            }
            ImGui::TextDisabled(editing ? "Editing" : play_state_ == PlayState::Playing ? "Playing" : "Paused");
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::draw_status_bar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextDisabled("%s%s", scene_path_.empty() ? "untitled scene" : scene_path_.c_str(), dirty() ? " *" : "");
            ImGui::Separator();
            ImGui::TextDisabled("%zu entities", active_scene().size());
            ImGui::Separator();
            ImGui::TextDisabled("%.0f fps", static_cast<double>(fps_));
            if (status_time_ > 0 && !status_line_.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(status_line_.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ---- viewport -----------------------------------------------------------------------------------

void Editor::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin(kViewportWindow, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    const auto w = static_cast<std::uint32_t>(std::max(1.0f, avail.x * scale));
    const auto h = static_cast<std::uint32_t>(std::max(1.0f, avail.y * scale));
    // The texture is sized here (before ImGui::Image references it) and drawn in render().
    if (auto st = forced_w_ ? ensure_viewport(forced_w_, forced_h_) : ensure_viewport(w, h); !st.ok())
        notify("viewport: " + st.error().message, true);
    if (viewport_tex_) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef(ui::UiContext::texture_id(viewport_tex_)), avail);
        const bool hovered = ImGui::IsItemHovered();
        viewport_focused_ = ImGui::IsWindowFocused();
        // Accept model drops.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db_->find(id); rec && rec->kind == assets::AssetKind::Model) instantiate_asset(id);
            }
            ImGui::EndDragDropTarget();
        }
        // Overlay: mode + stats.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 6));
        const bool two_d = runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD;
        ImGui::TextDisabled("%s  %s  |  %u meshes  %u sprites  %u lights", two_d ? "2D" : "3D",
                            play_state_ == PlayState::Editing ? "editor camera" : "game camera",
                            last_view_stats_.extract.meshes, last_view_stats_.sprites, last_view_stats_.extract.lights);
        if (play_state_ == PlayState::Editing)
            viewport_input({avail.x, avail.y}, hovered);
        (void)origin;
        // Click-to-select (after input so a drag does not select).
        if (hovered && play_state_ == PlayState::Editing && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !lmb_dragged_) {
            const ImVec2 m = ImGui::GetMousePos();
            const Entity hit = pick({m.x - origin.x, m.y - origin.y}, {avail.x, avail.y});
            select(hit, ImGui::GetIO().KeyCtrl);
        }
    }
    ImGui::End();
}

void Editor::viewport_input(math::Vec2 size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const bool two_d = runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_origin_ = {io.MousePos.x, io.MousePos.y};
        lmb_dragged_ = false;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - drag_origin_.x, dy = io.MousePos.y - drag_origin_.y;
        if (dx * dx + dy * dy > 16.0f) lmb_dragged_ = true;
    }
    if (!hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) return;
    const ImVec2 d = io.MouseDelta;
    if (two_d) {
        render::Camera2D& c = camera_.cam2d;
        const float units_per_px = camera_.view_height / std::max(size.y * c.zoom, 1e-3f);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
            (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            c.position.x -= d.x * units_per_px;
            c.position.y += d.y * units_per_px;
        }
        if (hovered && io.MouseWheel != 0) c.zoom = std::clamp(c.zoom * std::pow(1.15f, io.MouseWheel), 0.05f, 40.0f);
        return;
    }
    const bool orbit = ImGui::IsMouseDown(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Right));
    if (pan) {
        const render::Camera3D cam = camera_.camera3d();
        const math::Vec3 fwd = math::normalize(camera_.target - cam.position);
        const math::Vec3 right = math::normalize(math::cross(fwd, {0, 1, 0}));
        const math::Vec3 up = math::cross(right, fwd);
        const float k = camera_.distance * 0.0016f;
        camera_.target = camera_.target - right * (d.x * k) + up * (d.y * k);
    } else if (orbit) {
        camera_.yaw -= d.x * 0.006f;
        camera_.pitch = std::clamp(camera_.pitch - d.y * 0.006f, -1.55f, 1.55f);
    }
    if (hovered && io.MouseWheel != 0) camera_.distance = std::clamp(camera_.distance * std::pow(0.88f, io.MouseWheel), 0.2f, 2000.0f);
    // WASD fly while orbiting (moves the focus point).
    if (orbit && !io.WantTextInput) {
        const render::Camera3D cam = camera_.camera3d();
        const math::Vec3 fwd = math::normalize(camera_.target - cam.position);
        const math::Vec3 right = math::normalize(math::cross(fwd, {0, 1, 0}));
        const float speed = camera_.distance * io.DeltaTime * (io.KeyShift ? 3.0f : 1.0f);
        if (ImGui::IsKeyDown(ImGuiKey_W)) camera_.target = camera_.target + fwd * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) camera_.target = camera_.target - fwd * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) camera_.target = camera_.target + right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) camera_.target = camera_.target - right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) camera_.target.y += speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) camera_.target.y -= speed;
    }
}

// ---- hierarchy ----------------------------------------------------------------------------------

void Editor::draw_hierarchy_node(Entity e) {
    Scene& s = active_scene();
    const Uuid id = s.uuid(e);
    const std::vector<Entity> kids = s.children(e);
    const Name* n = s.world().get<Name>(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (is_selected(e)) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(static_cast<int>(e.index));
    const char* tag = s.world().has<CameraComponent>(e) ? "(cam) " : s.world().has<LightComponent>(e) ? "(light) " : "";
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s%s", tag, n ? n->value.c_str() : "Entity");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) select(e, ImGui::GetIO().KeyCtrl);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_selected();
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(Uuid));
        ImGui::Text("%s", n ? n->value.c_str() : "Entity");
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Uuid dragged;
            std::memcpy(&dragged, p->Data, sizeof(Uuid));
            // Drop on the upper quarter inserts before this node; otherwise makes it a child.
            const float y = ImGui::GetMousePos().y - ImGui::GetItemRectMin().y;
            const Entity de = s.find(dragged);
            if (!de.is_null() && de != e) {
                if (y < ImGui::GetItemRectSize().y * 0.25f) reparent(de, s.parent(e), e);
                else reparent(de, e);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (!is_selected(e)) select(e);
        if (ImGui::MenuItem("Create Child")) create_entity(Primitive::Empty, e);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selected();
        if (ImGui::MenuItem("Delete", "Del")) delete_selected();
        if (ImGui::MenuItem("Unparent", nullptr, false, !s.parent(e).is_null())) reparent(e, kNullEntity);
        ImGui::EndPopup();
    }
    if (open) {
        for (const Entity c : kids)
            if (!s.find(id).is_null()) draw_hierarchy_node(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::draw_hierarchy() {
    if (!ImGui::Begin(kHierarchyWindow)) {
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##add");
    if (ImGui::BeginPopup("##add")) {
        if (ImGui::MenuItem("Empty")) create_entity(Primitive::Empty);
        if (ImGui::MenuItem("Cube")) create_entity(Primitive::Cube);
        if (ImGui::MenuItem("Sphere")) create_entity(Primitive::Sphere);
        if (ImGui::MenuItem("Plane")) create_entity(Primitive::Plane);
        if (ImGui::MenuItem("Cylinder")) create_entity(Primitive::Cylinder);
        if (ImGui::MenuItem("Sprite")) create_entity(Primitive::Sprite);
        if (ImGui::MenuItem("Camera")) create_entity(Primitive::Camera);
        if (ImGui::MenuItem("Directional Light")) create_entity(Primitive::DirectionalLight);
        if (ImGui::MenuItem("Point Light")) create_entity(Primitive::PointLight);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s.name.c_str());
    ImGui::Separator();
    ImGui::BeginChild("##tree");
    for (const Entity e : s.roots()) draw_hierarchy_node(e);
    // Empty space: drop to unparent, click to deselect.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(40.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::IsItemClicked()) clear_selection();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Uuid dragged;
            std::memcpy(&dragged, p->Data, sizeof(Uuid));
            if (const Entity de = s.find(dragged); !de.is_null()) reparent(de, kNullEntity);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- inspector ----------------------------------------------------------------------------------

void Editor::begin_inspector_frame(Entity e) {
    Scene& s = active_scene();
    const Uuid id = s.uuid(e);
    if (pending_dirty_ && pending_entity_ != id) {
        record_edit("Edit", std::move(pending_before_));
        pending_dirty_ = false;
    }
    if (!pending_dirty_) {
        pending_before_ = capture(s, {id});
        pending_entity_ = id;
    }
}

void Editor::end_inspector_frame(Entity, bool changed) {
    if (changed) pending_dirty_ = true;
    if (pending_dirty_ && !ImGui::IsAnyItemActive()) {
        record_edit("Edit", std::move(pending_before_));
        pending_before_.clear();
        pending_dirty_ = false;
    }
}

bool Editor::inspect_components(Entity e) {
    Scene& s = active_scene();
    ecs::World& w = s.world();
    bool changed = false;
    const ImGuiTreeNodeFlags header = ImGuiTreeNodeFlags_DefaultOpen;

    if (Name* n = w.get<Name>(e)) changed |= ImGui::InputText("Name", &n->value);
    ImGui::TextDisabled("UUID %s", s.uuid(e).to_string().c_str());

    if (Transform* t = w.get<Transform>(e); t && ImGui::CollapsingHeader("Transform", header)) {
        changed |= vec3_edit("Position", t->position);
        // Euler editing with a cache so values do not jump while dragging.
        if (euler_cache_.entity != s.uuid(e) || !(euler_cache_.rotation == t->rotation)) {
            const math::Vec3 r = math::to_euler(t->rotation);
            euler_cache_ = {s.uuid(e), t->rotation, {math::degrees(r.x), math::degrees(r.y), math::degrees(r.z)}};
        }
        math::Vec3 deg = euler_cache_.degrees;
        if (vec3_edit("Rotation", deg, 0.5f)) {
            t->rotation = math::Quat::euler(math::radians(deg.x), math::radians(deg.y), math::radians(deg.z));
            euler_cache_ = {s.uuid(e), t->rotation, deg};
            changed = true;
        }
        changed |= vec3_edit("Scale", t->scale, 0.02f);
    }
    bool remove_camera = false, remove_light = false, remove_mesh = false, remove_sprite = false, remove_rot = false,
         remove_osc = false;
    const auto removable = [](const char* label, bool& remove) {
        bool keep = true;
        const bool open = ImGui::CollapsingHeader(label, &keep, ImGuiTreeNodeFlags_DefaultOpen);
        if (!keep) remove = true;
        return open;
    };
    if (auto* c = w.get<CameraComponent>(e); c && removable("Camera", remove_camera)) {
        int proj = static_cast<int>(c->projection);
        if (ImGui::Combo("Projection", &proj, "Perspective\0Orthographic\0")) {
            c->projection = static_cast<CameraComponent::Projection>(proj);
            changed = true;
        }
        if (c->projection == CameraComponent::Projection::Perspective)
            changed |= ImGui::DragFloat("Field of View", &c->fov_y_degrees, 0.2f, 5.0f, 170.0f, "%.1f deg");
        else
            changed |= ImGui::DragFloat("Ortho Height", &c->ortho_height, 0.05f, 0.01f, 10000.0f);
        changed |= ImGui::DragFloat("Near", &c->z_near, 0.01f, 0.001f, c->z_far);
        changed |= ImGui::DragFloat("Far", &c->z_far, 1.0f, c->z_near, 100000.0f);
        changed |= ImGui::Checkbox("Primary", &c->primary);
    }
    if (auto* l = w.get<LightComponent>(e); l && removable("Light", remove_light)) {
        int type = static_cast<int>(l->type);
        if (ImGui::Combo("Type", &type, "Directional\0Point\0")) {
            l->type = static_cast<LightComponent::Type>(type);
            changed = true;
        }
        changed |= color_edit("Color", l->color, false);
        changed |= ImGui::DragFloat("Intensity", &l->intensity, 0.05f, 0.0f, 1000.0f);
        if (l->type == LightComponent::Type::Point) changed |= ImGui::DragFloat("Range", &l->range, 0.05f, 0.01f, 1000.0f);
        changed |= ImGui::Checkbox("Shadows", &l->shadows);
    }
    if (auto* m = w.get<MeshRendererComponent>(e); m && removable("Mesh Renderer", remove_mesh)) {
        if (m->mesh_asset.is_nil()) {
            static constexpr const char* kPrims[] = {"cube", "sphere", "plane", "cylinder"};
            int cur = 0;
            for (int i = 0; i < 4; ++i)
                if (m->primitive == kPrims[i]) cur = i;
            if (ImGui::Combo("Primitive", &cur, "cube\0sphere\0plane\0cylinder\0")) {
                m->primitive = kPrims[cur];
                changed = true;
            }
        } else {
            ImGui::Text("Mesh: %s #%u", asset_label(db_.get(), m->mesh_asset).c_str(), m->mesh_index);
        }
        changed |= color_edit("Base Color", m->base_color);
        changed |= ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Cast Shadows", &m->cast_shadows);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Visible", &m->visible);
    }
    if (auto* sp = w.get<SpriteComponent>(e); sp && removable("Sprite", remove_sprite)) {
        ImGui::Text("Texture: %s", asset_label(db_.get(), sp->texture_asset).c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db_->find(id); rec && rec->kind == assets::AssetKind::Texture) {
                    sp->texture_asset = id;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (!sp->texture_asset.is_nil()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                sp->texture_asset = {};
                changed = true;
            }
        }
        changed |= vec2_edit("Size", sp->size);
        changed |= vec2_edit("Pivot", sp->pivot, 0.01f);
        changed |= color_edit("Tint", sp->tint);
        changed |= ImGui::DragInt("Layer", &sp->layer);
        changed |= ImGui::Checkbox("Flip X", &sp->flip_x);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip Y", &sp->flip_y);
    }
    if (auto* r = w.get<gameplay::RotatorComponent>(e); r && removable("Rotator", remove_rot)) {
        changed |= vec3_edit("Axis", r->axis, 0.01f);
        changed |= ImGui::DragFloat("Degrees / s", &r->degrees_per_second, 0.5f);
    }
    if (auto* o = w.get<gameplay::OscillatorComponent>(e); o && removable("Oscillator", remove_osc)) {
        changed |= vec3_edit("Amplitude", o->amplitude, 0.01f);
        changed |= ImGui::DragFloat("Frequency (Hz)", &o->frequency, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Phase (rad)", &o->phase, 0.01f);
    }
    if (const auto* u = w.get<UnknownComponents>(e); u && !u->components.empty()) {
        if (ImGui::CollapsingHeader("Unknown Components")) {
            for (const auto& [name, value] : u->components) ImGui::BulletText("%s (preserved)", name.c_str());
        }
    }
    if (remove_camera) {
        w.remove<CameraComponent>(e);
        changed = true;
    }
    if (remove_light) {
        w.remove<LightComponent>(e);
        changed = true;
    }
    if (remove_mesh) {
        w.remove<MeshRendererComponent>(e);
        changed = true;
    }
    if (remove_sprite) {
        w.remove<SpriteComponent>(e);
        changed = true;
    }
    if (remove_rot) {
        w.remove<gameplay::RotatorComponent>(e);
        changed = true;
    }
    if (remove_osc) {
        w.remove<gameplay::OscillatorComponent>(e);
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) ImGui::OpenPopup("##addcomp");
    if (ImGui::BeginPopup("##addcomp")) {
        if (!w.has<CameraComponent>(e) && ImGui::MenuItem("Camera")) {
            w.add<CameraComponent>(e).primary = false;
            changed = true;
        }
        if (!w.has<LightComponent>(e) && ImGui::MenuItem("Light")) {
            w.add<LightComponent>(e);
            changed = true;
        }
        if (!w.has<MeshRendererComponent>(e) && ImGui::MenuItem("Mesh Renderer")) {
            w.add<MeshRendererComponent>(e).primitive = "cube";
            changed = true;
        }
        if (!w.has<SpriteComponent>(e) && ImGui::MenuItem("Sprite")) {
            w.add<SpriteComponent>(e);
            changed = true;
        }
        if (!w.has<gameplay::RotatorComponent>(e) && ImGui::MenuItem("Rotator")) {
            w.add<gameplay::RotatorComponent>(e);
            changed = true;
        }
        if (!w.has<gameplay::OscillatorComponent>(e) && ImGui::MenuItem("Oscillator")) {
            w.add<gameplay::OscillatorComponent>(e);
            changed = true;
        }
        ImGui::EndPopup();
    }
    return changed;
}

void Editor::draw_inspector() {
    if (!ImGui::Begin(kInspectorWindow)) {
        ImGui::End();
        return;
    }
    prune_selection();
    const Entity e = selected();
    if (e.is_null()) {
        ImGui::TextDisabled("Select an entity in the Hierarchy or Viewport.");
        if (pending_dirty_) {
            record_edit("Edit", std::move(pending_before_));
            pending_dirty_ = false;
        }
    } else {
        if (selection_.size() > 1) ImGui::TextDisabled("%zu selected (editing the last)", selection_.size());
        begin_inspector_frame(e);
        const bool changed = inspect_components(e);
        if (changed) active_scene().update_transforms();
        end_inspector_frame(e, changed);
    }
    if (play_state_ != PlayState::Editing) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Play mode: edits are temporary.");
    }
    ImGui::End();
}

// ---- assets & console ---------------------------------------------------------------------------

void Editor::draw_assets() {
    if (!ImGui::Begin(kAssetsWindow)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Reimport")) (void)reimport_assets();
    ImGui::SameLine();
    ImGui::TextDisabled("Drop files into %s", project_->assets_dir().string().c_str());
    ImGui::Separator();
    if (ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        std::vector<assets::AssetRecord> recs = db_ ? db_->records() : std::vector<assets::AssetRecord>{};
        std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        for (const auto& r : recs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r.path.c_str());
            ImGui::Selectable(r.path.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kAssetPayload, &r.uuid, sizeof(Uuid));
                ImGui::Text("%s %s", kind_icon(r.kind), r.path.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (r.kind == assets::AssetKind::Model) instantiate_asset(r.uuid);
                else if (r.kind == assets::AssetKind::Texture) {
                    const Entity e = selected();
                    if (auto* sp = e.is_null() ? nullptr : active_scene().world().get<SpriteComponent>(e)) {
                        auto before = capture(active_scene(), {active_scene().uuid(e)});
                        sp->texture_asset = r.uuid;
                        record_edit("Assign texture", std::move(before));
                    }
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nUUID %s\nDouble-click: %s", r.path.c_str(), r.uuid.to_string().c_str(),
                                                          r.kind == assets::AssetKind::Model ? "add to scene"
                                                          : r.kind == assets::AssetKind::Texture ? "assign to selected sprite"
                                                                                                 : "-");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", assets::to_string(r.kind));
            ImGui::TableNextColumn();
            if (r.importer.empty()) ImGui::TextDisabled("-");
            else {
                auto up = db_->is_up_to_date(r.uuid);
                if (up && *up) ImGui::TextDisabled("imported");
                else ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", up ? "stale" : "error");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::draw_console() {
    if (!ImGui::Begin(kConsoleWindow)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) log_sink_->clear();
    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const auto records = log_sink_->snapshot();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(records.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const auto& r = records[static_cast<std::size_t>(i)];
            const ImVec4 col = r.level >= log::Level::Error  ? ImVec4(1, 0.4f, 0.35f, 1)
                               : r.level == log::Level::Warn ? ImVec4(1, 0.8f, 0.3f, 1)
                                                             : ImVec4(0.8f, 0.8f, 0.82f, 1);
            ImGui::TextColored(col, "[%s] %s: %s", log::to_string(r.level), r.category.c_str(), r.message.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

// ---- modals & shortcuts -------------------------------------------------------------------------

void Editor::draw_modals() {
    if (pending_action_ != Pending::None && !ImGui::IsPopupOpen("Unsaved Changes")) ImGui::OpenPopup("Unsaved Changes");
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before %s?", scene_path_.empty() ? "the untitled scene" : scene_path_.c_str(),
                    pending_action_ == Pending::Quit ? "quitting" : "closing");
        ImGui::Spacing();
        const auto finish = [&] {
            const Pending a = pending_action_;
            pending_action_ = Pending::None;
            ImGui::CloseCurrentPopup();
            if (a == Pending::Quit) {
                quit_confirmed_ = true;
                if (platform_) platform_->request_quit();
            } else if (a == Pending::CloseProject) {
                close_project();
            }
        };
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            if (scene_path_.empty()) (void)save_scene_as("scenes/untitled");
            if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
            else finish();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) finish();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending_action_ = Pending::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (show_save_as_) {
        ImGui::OpenPopup("Save Scene As");
        show_save_as_ = false;
        save_as_path_ = scene_path_.empty() ? "scenes/new_scene.vscene" : scene_path_;
    }
    if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Project-relative path:");
        ImGui::SetNextItemWidth(360);
        const bool enter = ImGui::InputText("##path", &save_as_path_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save", ImVec2(110, 0)) || enter) {
            if (auto st = save_scene_as(save_as_path_); !st.ok()) notify(st.error().message, true);
            else ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Editor::handle_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool editing = play_state_ == PlayState::Editing;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && editing) {
        if (scene_path_.empty()) show_save_as_ = true;
        else if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selected();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) {
        if (editing) play();
        else stop();
    }
    const bool mouse_nav = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!mouse_nav && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selected();
    if (!mouse_nav && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selected();
}

} // namespace victus::editor
