// SPDX-License-Identifier: Apache-2.0
// Editor UI: launcher, dockspace workspace and panels (Dear ImGui).
#include "victus/editor/editor.hpp"

#include "editor_client.hpp"

#include "victus/core/build_info.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"
#include "victus/scene/reflect.hpp"

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


std::string asset_label(assets::AssetDatabase* db, const Uuid& id);

// Asset kind accepted by a reflected UUID field's hint ("model", "texture", ...).
assets::AssetKind asset_kind_for_hint(std::string_view hint) {
    if (hint == "model") return assets::AssetKind::Model;
    if (hint == "texture") return assets::AssetKind::Texture;
    if (hint == "material") return assets::AssetKind::Material;
    if (hint == "prefab") return assets::AssetKind::Prefab;
    if (hint == "spritesheet") return assets::AssetKind::SpriteSheet;
    if (hint == "tileset") return assets::AssetKind::Tileset;
    if (hint == "terrain") return assets::AssetKind::Terrain;
    return assets::AssetKind::Unknown;
}

// Editor widgets for one reflected field. Returns true when the value changed.
bool field_edit(const scene::FieldInfo& f, void* comp, assets::AssetDatabase* db, const scene::Scene* scene) {
    const char* label = f.label.c_str();
    const float speed = f.speed > 0 ? f.speed : 0.05f;
    const float lo = f.bounded() ? f.min : 0.0f, hi = f.bounded() ? f.max : 0.0f;
    bool changed = false;
    switch (f.kind) {
    case scene::FieldKind::Bool: changed = ImGui::Checkbox(label, &f.ref<bool>(comp)); break;
    case scene::FieldKind::Float: changed = ImGui::DragFloat(label, &f.ref<float>(comp), speed, lo, hi, "%.3f"); break;
    case scene::FieldKind::Int:
    case scene::FieldKind::UInt: {
        if (f.hint == "bitmask16") {
            auto bits = static_cast<std::uint32_t>(f.get_int(comp));
            if (ImGui::TreeNode(label)) {
                for (int i = 0; i < 16; ++i) {
                    bool on = (bits >> i) & 1u;
                    if (i % 8) ImGui::SameLine();
                    ImGui::PushID(i);
                    if (ImGui::Checkbox("##bit", &on)) {
                        bits = on ? (bits | (1u << i)) : (bits & ~(1u << i));
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Layer %d", i);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            if (changed) f.set_int(comp, bits);
            break;
        }
        int v = static_cast<int>(f.get_int(comp));
        if (ImGui::DragInt(label, &v, 0.1f, static_cast<int>(lo), static_cast<int>(hi))) {
            if (f.kind == scene::FieldKind::UInt && v < 0) v = 0;
            f.set_int(comp, v);
            changed = true;
        }
        break;
    }
    case scene::FieldKind::Vec2: changed = vec2_edit(label, f.ref<math::Vec2>(comp), speed); break;
    case scene::FieldKind::Vec3: changed = vec3_edit(label, f.ref<math::Vec3>(comp), speed); break;
    case scene::FieldKind::Color: changed = color_edit(label, f.ref<math::Color>(comp)); break;
    case scene::FieldKind::String: {
        auto& str = f.ref<std::string>(comp);
        changed = ImGui::InputText(label, &str);
        // Asset-path fields accept a drop from the asset table when the kind matches the hint.
        const assets::AssetKind want = f.hint == "audio"           ? assets::AssetKind::Audio
                                       : f.hint == "sequence"      ? assets::AssetKind::Sequence
                                       : f.hint == "behavior_tree" ? assets::AssetKind::BehaviorTree
                                                                   : assets::AssetKind::Unknown;
        if (want != assets::AssetKind::Unknown && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db ? db->find(id) : nullptr; rec && rec->kind == want) {
                    str = rec->path;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        break;
    }
    case scene::FieldKind::Enum: {
        int cur = static_cast<int>(f.get_int(comp));
        const char* preview = cur >= 0 && cur < static_cast<int>(f.enum_names.size()) ? f.enum_names[cur].c_str() : "?";
        if (ImGui::BeginCombo(label, preview)) {
            for (int i = 0; i < static_cast<int>(f.enum_names.size()); ++i) {
                if (ImGui::Selectable(f.enum_names[i].c_str(), i == cur) && i != cur) {
                    f.set_int(comp, i);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case scene::FieldKind::Uuid: {
        Uuid& id = f.ref<Uuid>(comp);
        if (f.hint == "entity") {
            // Entity reference: drop an entity from the hierarchy; the button clears it.
            std::string shown = "(none)";
            if (!id.is_nil()) {
                const Entity target = scene ? scene->find(id) : Entity{};
                const auto* n = target.is_null() ? nullptr : scene->world().get<scene::Name>(target);
                shown = n ? n->value : "(missing) " + id.to_string();
            }
            ImGui::PushID(label);
            if (ImGui::SmallButton("x") && !id.is_nil()) {
                id = {};
                changed = true;
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::Text("%s: %s", label, shown.c_str());
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                    std::memcpy(&id, p->Data, sizeof(Uuid));
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
        } else if (const auto kind = asset_kind_for_hint(f.hint); kind != assets::AssetKind::Unknown) {
            // Asset reference: drop an asset of the hinted kind from Assets; the button clears it.
            ImGui::PushID(label);
            if (ImGui::SmallButton("x") && !id.is_nil()) {
                id = {};
                changed = true;
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::Text("%s: %s", label, asset_label(db, id).c_str());
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                    Uuid dropped;
                    std::memcpy(&dropped, p->Data, sizeof(Uuid));
                    if (const auto* rec = db ? db->find(dropped) : nullptr; rec && rec->kind == kind) {
                        id = dropped;
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
        } else {
            ImGui::Text("%s: %s", label, id.to_string().c_str());
        }
        break;
    }
    }
    if (!f.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.tooltip.c_str());
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
    case assets::AssetKind::Script: return "[lua]";
    case assets::AssetKind::Audio: return "[snd]";
    case assets::AssetKind::Prefab: return "[pfb]";
    case assets::AssetKind::Material: return "[mat]";
    case assets::AssetKind::Shader: return "[shd]";
    case assets::AssetKind::SpriteSheet: return "[sht]";
    case assets::AssetKind::Tileset: return "[tls]";
    case assets::AssetKind::Terrain: return "[ter]";
    case assets::AssetKind::BehaviorTree: return "[bt]";
    case assets::AssetKind::Sequence: return "[seq]";
    default: return "[file]";
    }
}
} // namespace

// ---- frame --------------------------------------------------------------------------------------

void Editor::frame(float dt) {
    update_profiler();
    if (play_state_ == PlayState::Editing) update_autosave(dt);
    if (dt > 0) fps_ = fps_ <= 0 ? 1.0f / dt : fps_ * 0.95f + 0.05f / dt;
    view_->renderer().set_frame_time(dt); // auto exposure adapts in real time
    if (status_time_ > 0) status_time_ -= dt;
    if (input_ && capturing_binding()) {
        input_->set_enabled(true); // "press a control to bind" listens to every device
        (void)capture_from(*input_);
    }
    if (input_) input_->set_enabled(play_state_ == PlayState::Playing && viewport_focused_);
    if (play_state_ == PlayState::Playing && play_scene_) tick_play(dt);
    else if (!play_scene_) {
        if (edit_sprites_) edit_sprites_->update_sprites(active_scene(), 0); // frame preview
        if (terrains_) terrains_->update(active_scene());                   // terrain chunks in edit mode
        if (tilemaps_) tilemaps_->update(active_scene());                   // tile sets for drawing
    }
    update_graph_debug(dt);
    // Inspector property discovery follows script edits made in an external editor.
    if ((script_poll_ += dt) >= 1.0f) {
        script_poll_ = 0;
        if (edit_scripts_) (void)edit_scripts_->reload_changed();
        (void)check_asset_changes(); // textures, models and audio edited outside the editor
        sync_graph_from_disk();
    }
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
    draw_input_actions();
    draw_network();
    draw_profiler();
    draw_command_palette();
    draw_material_editor();
    draw_graph_editor();
    draw_tile_palette();
    draw_terrain_tools();
    draw_ai_debugger();
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
        ImGui::Separator();
        ImGui::MenuItem("Input Actions...", nullptr, &show_input_actions_);
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
        item("Spot Light", Primitive::SpotLight);
        ImGui::Separator();
        if (ImGui::MenuItem("Terrain", nullptr, false, db_ != nullptr && play_state_ == PlayState::Editing))
            if (auto t = create_terrain(); !t) notify("cannot create terrain: " + t.error().message, true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Assets")) {
        if (ImGui::MenuItem("Reimport All")) (void)reimport_assets();
        if (ImGui::MenuItem("New Material", nullptr, false, db_ != nullptr)) new_material_for_selection();
        if (ImGui::MenuItem("New Surface Shader", nullptr, false, db_ != nullptr)) {
            if (auto id = create_surface_shader(); !id) notify("cannot create shader: " + id.error().message, true);
            else if (show_material_editor_) (void)set_editing_material_shader(*id);
        }
        ImGui::MenuItem("Material Editor", nullptr, &show_material_editor_);
        ImGui::Separator();
        if (ImGui::MenuItem("New Visual Script", nullptr, false, db_ != nullptr)) {
            if (auto id = create_graph(); !id) notify("cannot create visual script: " + id.error().message, true);
            else if (auto st = open_graph_editor(*id); !st.ok()) notify(st.error().message, true);
        }
        if (ImGui::MenuItem("New Material Graph", nullptr, false, db_ != nullptr)) {
            if (auto id = create_material_graph(); !id) notify("cannot create material graph: " + id.error().message, true);
            else {
                if (show_material_editor_) (void)set_editing_material_shader(*id);
                if (auto st = open_graph_editor(*id); !st.ok()) notify(st.error().message, true);
            }
        }
        ImGui::MenuItem("Graph Editor", nullptr, &show_graph_editor_);
        if (ImGui::MenuItem("Copy Assets Folder Path") && platform_) platform_->set_clipboard(project_->assets_dir().string());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P")) open_command_palette();
        if (ImGui::MenuItem("Reset Layout")) layout_built_ = false;
        ImGui::MenuItem("Physics Colliders", nullptr, &show_colliders_);
        ImGui::MenuItem("Navigation Mesh", nullptr, &show_navmesh_);
        ImGui::MenuItem("AI Debug", nullptr, &show_ai_);
        ImGui::MenuItem("Network", nullptr, &show_network_);
        if (ImGui::MenuItem("Profiler", nullptr, show_profiler_)) set_show_profiler(!show_profiler_);
        ImGui::MenuItem("Tile Palette", nullptr, &show_tile_palette_);
        ImGui::MenuItem("Terrain Tools", nullptr, &show_terrain_tools_);
        ImGui::MenuItem("UI Demo Window", nullptr, &show_demo_);
        if (ImGui::BeginMenu("Rendering Quality")) {
            for (auto tier : {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High,
                              render::QualityTier::Ultra}) {
                const std::string label(render::quality_tier_name(tier));
                if (ImGui::MenuItem(label.c_str(), nullptr, quality_ == tier) && quality_ != tier)
                    if (auto st = set_quality(tier); !st.ok()) notify("could not save quality: " + st.error().message, true);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        if (ImGui::MenuItem(editing ? "Play" : "Resume", nullptr, false, play_state_ != PlayState::Playing)) play();
        if (ImGui::MenuItem("Pause", nullptr, false, play_state_ == PlayState::Playing)) pause();
        if (ImGui::MenuItem("Stop", nullptr, false, !editing)) stop();
        ImGui::Separator();
        if (ImGui::MenuItem("Play as Host", nullptr, false, editing && project_.has_value())) play_host();
        if (ImGui::BeginMenu("Network Mode", editing)) {
            NetPlaySettings s = net_settings_;
            if (ImGui::MenuItem("Offline", nullptr, s.mode == NetPlaySettings::Mode::Offline)) s.mode = NetPlaySettings::Mode::Offline;
            if (ImGui::MenuItem("Host (listen server)", nullptr, s.mode == NetPlaySettings::Mode::Host)) s.mode = NetPlaySettings::Mode::Host;
            (void)set_net_settings(s);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Network Settings...")) show_network_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Record Capture", nullptr, false, editing && project_.has_value())) record();
        const auto list = captures();
        if (ImGui::MenuItem("Replay Last Capture", nullptr, false, !list.empty() && !recording()))
            if (auto st = replay(list.front()); !st.ok()) notify("cannot replay: " + st.error().message, true);
        if (ImGui::BeginMenu("Replay Capture", !list.empty() && !recording())) {
            for (const auto& c : list)
                if (ImGui::MenuItem(c.filename().string().c_str()))
                    if (auto st = replay(c); !st.ok()) notify("cannot replay: " + st.error().message, true);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Copy Captures Folder Path", nullptr, false, project_.has_value() && platform_ != nullptr))
            platform_->set_clipboard((project_->root() / ".victus" / "captures").string());
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
            const float w = 300.0f;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - w) * 0.5f));
            const bool editing = play_state_ == PlayState::Editing;
            if (editing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.45f, 1));
                if (ImGui::MenuItem(net_settings_.mode == NetPlaySettings::Mode::Host ? "> Play (Host)" : "> Play")) play();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(net_settings_.mode == NetPlaySettings::Mode::Host ? "Play as a listen server (Play > Network Mode)"
                                                                                         : "Play offline (Play > Network Mode)");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1));
                if (ImGui::MenuItem("(o) Record", nullptr, false, project_.has_value())) record();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Play while recording input for a deterministic replay (Play > Replay Capture)");
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
            if (play_server_) {
                const auto d = play_server_->diagnostics();
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1), "HOST %u", d.clients + 1);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%u players, %.1f kbps out - View > Network", d.clients + 1, static_cast<double>(d.send_kbps));
                if (ImGui::IsItemClicked()) show_network_ = true;
                if (!play_clients_.empty()) {
                    // Which player the viewport shows and the keyboard drives (Ctrl+1..4 too).
                    if (ImGui::BeginMenu(std::format("View: {}###viewpeer", peer_label(view_peer_)).c_str())) {
                        for (std::size_t i = 0; i <= play_clients_.size(); ++i)
                            if (ImGui::MenuItem(peer_label(i).c_str(), std::format("Ctrl+{}", i + 1).c_str(), view_peer_ == i)) set_view_peer(i);
                        ImGui::EndMenu();
                    }
                }
            } else if (recording()) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1), "REC");
            } else if (replay_ && !replay_result_.empty()) {
                ImGui::TextColored(replay_diverged_ ? ImVec4(1.0f, 0.5f, 0.3f, 1) : ImVec4(0.45f, 0.95f, 0.45f, 1), "%s",
                                   replay_diverged_ ? "Diverged" : "Matched");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", replay_result_.c_str());
            } else if (replay_) {
                ImGui::TextDisabled("Replaying %llu/%zu", static_cast<unsigned long long>(replay_->frame()),
                                    replay_capture_ ? replay_capture_->frames.size() : std::size_t{0});
            } else {
                ImGui::TextDisabled(editing ? "Editing" : play_state_ == PlayState::Playing ? "Playing" : "Paused");
            }
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
    if (focus_viewport_) {
        ImGui::SetNextWindowFocus(); // Play hands keyboard input to the game
        focus_viewport_ = false;
    }
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
        if (input_) {
            const float fb = ImGui::GetIO().DisplayFramebufferScale.x;
            input_->set_mouse_origin({origin.x * fb, origin.y * fb});
        }
        // Accept model drops.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db_->find(id); rec && rec->kind == assets::AssetKind::Model) instantiate_asset(id);
                else if (rec && rec->kind == assets::AssetKind::Prefab) instantiate_prefab(id);
            }
            ImGui::EndDragDropTarget();
        }
        // Overlay: mode + stats.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 6));
        const bool two_d = runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD;
        const std::string tier(render::quality_tier_name(quality_));
        ImGui::TextDisabled("%s  %s  |  %s quality  |  %u meshes (%u draws, %.1fk tris, %u at LOD)  %u sprites  %u lights",
                            two_d ? "2D" : "3D", play_state_ == PlayState::Editing ? "editor camera" : "game camera",
                            tier.c_str(), last_view_stats_.extract.meshes, render_stats().main_batches,
                            static_cast<double>(render_stats().triangles) / 1000.0, render_stats().lod_draws,
                            last_view_stats_.sprites, last_view_stats_.extract.lights);
        if (!two_d && render_stats().temporal_upscale) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8, origin.y + 6 + ImGui::GetTextLineHeightWithSpacing()));
            ImGui::TextDisabled("temporal upscale from %ux%u", render_stats().render_width, render_stats().render_height);
        }
        // The tile tool owns plain left clicks while a tilemap is selected in a 2D scene.
        // The terrain tool does the same while a terrain is selected in a 3D scene.
        const bool painting = ((tile_tool_ && two_d && !tile_target().is_null()) ||
                               (terrain_tool_ && !two_d && !terrain_target().is_null())) &&
                              !ImGui::GetIO().KeyAlt;
        // The transform gizmo takes clicks on its handles before camera navigation and picking.
        gizmo_viewport({origin.x, origin.y}, {avail.x, avail.y}, hovered, painting);
        if (play_state_ == PlayState::Editing) {
            if (!gizmo_consumed_click_) viewport_input({avail.x, avail.y}, hovered);
            if (!gizmo_consumed_click_) tile_viewport_input({origin.x, origin.y}, {avail.x, avail.y}, hovered);
            if (!gizmo_consumed_click_) terrain_viewport_input({origin.x, origin.y}, {avail.x, avail.y}, hovered);
        }
        // Click-to-select (after input so a drag does not select).
        if (hovered && !painting && !gizmo_consumed_click_ && play_state_ == PlayState::Editing &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !lmb_dragged_) {
            const ImVec2 m = ImGui::GetMousePos();
            const Entity hit = pick({m.x - origin.x, m.y - origin.y}, {avail.x, avail.y});
            select(hit, ImGui::GetIO().KeyCtrl);
        }
        // Box select: a left drag that starts on the viewport (not on a gizmo handle, not
        // painting tiles, no Alt orbit). Ctrl/Shift add to the selection.
        if (hovered && !painting && !gizmo_consumed_click_ && play_state_ == PlayState::Editing &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt)
            box_selecting_ = true;
        if (box_selecting_ && (gizmo_consumed_click_ || play_state_ != PlayState::Editing)) box_selecting_ = false;
        if (box_selecting_ && lmb_dragged_) {
            const ImVec2 m = ImGui::GetMousePos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0(std::min(drag_origin_.x, m.x), std::min(drag_origin_.y, m.y));
            const ImVec2 p1(std::max(drag_origin_.x, m.x), std::max(drag_origin_.y, m.y));
            dl->AddRectFilled(p0, p1, IM_COL32(90, 160, 255, 40));
            dl->AddRect(p0, p1, IM_COL32(90, 160, 255, 200));
        }
        if (box_selecting_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (lmb_dragged_) {
                const ImVec2 m = ImGui::GetMousePos();
                const ImGuiIO& io = ImGui::GetIO();
                (void)box_select({drag_origin_.x - origin.x, drag_origin_.y - origin.y}, {m.x - origin.x, m.y - origin.y},
                                 {avail.x, avail.y}, io.KeyCtrl || io.KeyShift);
            }
            box_selecting_ = false;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_consumed_click_ = false;
        draw_gizmo_toolbar({origin.x, origin.y}, {avail.x, avail.y});
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
    const bool prefab_root = s.world().has<scene::PrefabInstance>(e);
    const bool prefab_part = !prefab_root && s.world().has<scene::PrefabPart>(e);
    if (prefab_root || prefab_part)
        ImGui::PushStyleColor(ImGuiCol_Text, prefab_root ? ImVec4(0.45f, 0.75f, 1.0f, 1.0f) : ImVec4(0.62f, 0.8f, 0.98f, 1.0f));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s%s", tag, n ? n->value.c_str() : "Entity");
    if (prefab_root || prefab_part) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyShift) select_range(e);
        else select(e, ImGui::GetIO().KeyCtrl);
    }
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
        ImGui::Separator();
        if (ImGui::MenuItem("Create Prefab", nullptr, false, play_state_ == PlayState::Editing)) {
            if (auto r = create_prefab(e); !r) notify("Create prefab failed: " + r.error().message, true);
        }
        const Entity inst_root = scene::instance_root(s, e);
        if (ImGui::MenuItem("Apply to Prefab", nullptr, false, !inst_root.is_null() && play_state_ == PlayState::Editing)) {
            if (auto st = apply_prefab(inst_root); !st) notify("Apply failed: " + st.error().message, true);
        }
        if (ImGui::MenuItem("Revert to Prefab", nullptr, false, !inst_root.is_null())) {
            if (auto st = revert_prefab(inst_root); !st) notify("Revert failed: " + st.error().message, true);
        }
        if (ImGui::MenuItem("Unpack Prefab", nullptr, false, !inst_root.is_null())) unpack_prefab(inst_root);
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
        if (ImGui::MenuItem("Spot Light")) create_entity(Primitive::SpotLight);
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
        std::vector<Uuid> ids{id};
        // Multi-selection edits change every selected entity: capture them all for one undo step.
        for (const Uuid& other : selection_)
            if (other != id)
                if (const Entity oe = s.find(other); !oe.is_null()) ids.push_back(other);
        // Edits to a prefab part record overrides on the instance root: capture it for undo.
        if (const Entity root = scene::instance_root(s, e); !root.is_null() && root != e &&
            std::find(ids.begin(), ids.end(), s.uuid(root)) == ids.end())
            ids.push_back(s.uuid(root));
        pending_before_ = capture(s, ids);
        pending_entity_ = id;
    }
}

void Editor::end_inspector_frame(Entity e, bool changed) {
    if (changed) pending_dirty_ = true;
    if (pending_dirty_ && !ImGui::IsAnyItemActive()) {
        mark_prefab_overrides(e, pending_before_);
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
         remove_osc = false, remove_script = false;
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
        else {
            changed |= ImGui::DragFloat("Ortho Height", &c->ortho_height, 0.05f, 0.01f, 10000.0f);
            changed |= ImGui::Checkbox("Pixel Snap", &c->pixel_snap);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("2D: round the camera position to whole screen pixels");
        }
        changed |= ImGui::DragFloat("Near", &c->z_near, 0.01f, 0.001f, c->z_far);
        changed |= ImGui::DragFloat("Far", &c->z_far, 1.0f, c->z_near, 100000.0f);
        changed |= ImGui::Checkbox("Primary", &c->primary);
    }
    if (auto* l = w.get<LightComponent>(e); l && removable("Light", remove_light)) {
        int type = static_cast<int>(l->type);
        if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0")) {
            l->type = static_cast<LightComponent::Type>(type);
            changed = true;
        }
        changed |= color_edit("Color", l->color, false);
        changed |= ImGui::DragFloat("Intensity", &l->intensity, 0.05f, 0.0f, 1000.0f);
        if (l->type != LightComponent::Type::Directional) changed |= ImGui::DragFloat("Range", &l->range, 0.05f, 0.01f, 1000.0f);
        if (l->type == LightComponent::Type::Spot) {
            changed |= ImGui::DragFloat("Inner Angle", &l->inner_angle_degrees, 0.25f, 0.0f, l->outer_angle_degrees, "%.1f deg");
            changed |= ImGui::DragFloat("Outer Angle", &l->outer_angle_degrees, 0.25f, 0.1f, 89.5f, "%.1f deg");
        }
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
        ImGui::TextUnformatted("Material");
        const float mat_buttons = m->material_asset.is_nil() ? ImGui::CalcTextSize("New").x + 24.0f
                                                             : ImGui::CalcTextSize("EditClear").x + 40.0f;
        ImGui::Button((asset_label(db_.get(), m->material_asset) + "##material").c_str(), ImVec2(-mat_buttons, 0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a material (.vmat) from Assets here");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db_->find(id); rec && rec->kind == assets::AssetKind::Material) {
                    m->material_asset = id;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (m->material_asset.is_nil()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("New")) new_material_for_selection();
        } else {
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit")) open_material_editor(m->material_asset);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##mat")) {
                m->material_asset = {};
                changed = true;
            }
        }
        changed |= color_edit(m->material_asset.is_nil() ? "Base Color" : "Tint", m->base_color);
        if (m->material_asset.is_nil()) {
            changed |= ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);
        }
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
        if (w.has<animation::SpriteAnimatorComponent>(e)) {
            ImGui::TextDisabled("UV region set by the Sprite Animator");
        } else {
            float uv[4] = {sp->uv.x, sp->uv.y, sp->uv.z, sp->uv.w};
            if (ImGui::DragFloat4("UV Region", uv, 0.001f, 0.0f, 1.0f, "%.3f")) {
                sp->uv = {uv[0], uv[1], uv[2], uv[3]};
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Part of the texture drawn: u0, v0 (top), u1, v1 (bottom)");
        }
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
    if (auto* sc = w.get<script::ScriptComponent>(e); sc && removable("Script", remove_script)) changed |= inspect_script(*sc);
    // Reflected components (physics and every future schema-driven component).
    const scene::ComponentSchema* remove_schema = nullptr;
    for (const scene::ComponentSchema* schema : scene::schemas()) {
        void* comp = w.get_raw(e, schema->id);
        if (!comp) continue;
        ImGui::PushID(schema->name.c_str());
        bool remove = false;
        if (removable(schema->name.c_str(), remove))
            for (const auto& f : schema->fields) changed |= field_edit(f, comp, db_.get(), &s);
        if (remove) remove_schema = schema;
        ImGui::PopID();
    }
    if (remove_schema) {
        w.remove_raw(e, remove_schema->id);
        changed = true;
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
    if (remove_script) {
        w.remove<script::ScriptComponent>(e);
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
        if (!w.has<script::ScriptComponent>(e) && ImGui::MenuItem("Script")) {
            w.add<script::ScriptComponent>(e);
            changed = true;
        }
        // Reflected components grouped by category.
        std::vector<std::string> categories;
        for (const auto* schema : scene::schemas())
            if (std::find(categories.begin(), categories.end(), schema->category) == categories.end())
                categories.push_back(schema->category);
        for (const auto& cat : categories) {
            if (!ImGui::BeginMenu(cat.c_str())) continue;
            for (const auto* schema : scene::schemas()) {
                if (schema->category != cat) continue;
                const bool present = w.get_raw(e, schema->id) != nullptr;
                if (ImGui::MenuItem(schema->name.c_str(), nullptr, false, !present)) {
                    (void)w.add_raw(e, schema->id);
                    changed = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    return changed;
}

// Script path (drop a .luau asset), enable toggle, and the module's public fields. Fields keep the
// script's default until edited; edited values are stored as per-entity overrides.
bool Editor::inspect_script(script::ScriptComponent& c) {
    bool changed = ImGui::Checkbox("Enabled", &c.enabled);
    changed |= ImGui::InputTextWithHint("Script", "scripts/name.luau", &c.script);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            Uuid id;
            std::memcpy(&id, p->Data, sizeof(Uuid));
            if (const auto* rec = db_ ? db_->find(id) : nullptr; rec && rec->kind == assets::AssetKind::Script) {
                c.script = rec->path;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (c.script.empty()) {
        ImGui::TextDisabled("Drop a .luau or .vgraph file from Assets here.");
        return changed;
    }
    if (!edit_scripts_) return changed;
    auto props = edit_scripts_->describe(c.script);
    if (!props) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", props.error().message.c_str());
        ImGui::PopTextWrapPos();
        return changed;
    }
    if (!c.properties.is_object()) c.properties = json::Value::object();
    if (props->empty()) ImGui::TextDisabled("No public fields (declare e.g. `MyScript.speed = 5`).");
    for (const auto& info : *props) {
        ImGui::PushID(info.name.c_str());
        const json::Value* over = c.properties.find(info.name);
        const json::Value& cur = over ? *over : info.default_value;
        bool edited = false;
        json::Value next;
        if (cur.is_number()) {
            float f = static_cast<float>(cur.as_number());
            if ((edited = ImGui::DragFloat(info.name.c_str(), &f, 0.05f))) next = json::Value(static_cast<double>(f));
        } else if (cur.is_bool()) {
            bool b = cur.as_bool();
            if ((edited = ImGui::Checkbox(info.name.c_str(), &b))) next = json::Value(b);
        } else if (cur.is_string()) {
            std::string str = cur.as_string();
            if ((edited = ImGui::InputText(info.name.c_str(), &str))) next = json::Value(str);
        } else if (cur.is_array() && cur.size() == 3) {
            float v[3];
            for (std::size_t i = 0; i < 3; ++i) v[i] = static_cast<float>(cur[i].as_number());
            if ((edited = ImGui::DragFloat3(info.name.c_str(), v, 0.05f))) {
                next = json::Value::array();
                for (float x : v) next.push(json::Value(static_cast<double>(x)));
            }
        } else {
            ImGui::TextDisabled("%s (unsupported type)", info.name.c_str());
        }
        if (edited) {
            c.properties.set(info.name, std::move(next));
            changed = true;
        }
        if (over) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                json::Value kept = json::Value::object();
                for (const auto& [k, v] : c.properties.as_object())
                    if (k != info.name) kept.set(k, v);
                c.properties = std::move(kept);
                changed = true;
            }
        }
        ImGui::PopID();
    }
    return changed;
}

void Editor::draw_prefab_section(Entity e) {
    Scene& s = active_scene();
    const Entity root = scene::instance_root(s, e);
    if (root.is_null()) return;
    const auto* inst = s.world().get<scene::PrefabInstance>(root);
    const assets::AssetRecord* rec = db_ ? db_->find(inst->prefab) : nullptr;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.75f, 1.0f, 1.0f));
    ImGui::TextWrapped("Prefab: %s%s", rec ? rec->path.c_str() : "(missing prefab asset)", root == e ? "" : "  (part)");
    ImGui::PopStyleColor();
    const bool editing = play_state_ == PlayState::Editing;
    const auto flush = [&] {
        if (pending_dirty_) {
            record_edit("Edit", std::move(pending_before_));
            pending_before_.clear();
            pending_dirty_ = false;
        }
    };
    ImGui::BeginDisabled(!rec || !editing);
    if (ImGui::Button("Apply")) {
        flush();
        if (auto st = apply_prefab(root); !st) notify("Apply failed: " + st.error().message, true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Write this instance to the prefab and update every instance");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        flush();
        if (auto st = revert_prefab(root); !st) notify("Revert failed: " + st.error().message, true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Discard overrides and match the prefab");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Unpack")) {
        flush();
        unpack_prefab(root);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the instance into ordinary entities");
    if (root != e) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Select root")) select(root);
    }
    // Only meaningful for the instance in the scene (overrides of the entity shown).
    if (const auto* src_part = s.world().get<scene::PrefabPart>(e); src_part || root == e) {
        std::string list;
        for (const auto& o : inst->overrides) {
            const bool mine = root == e ? false : o.part == src_part->source;
            if (root == e || mine) {
                if (!list.empty()) list += ", ";
                list += o.component;
            }
        }
        if (root == e && !inst->overrides.empty())
            ImGui::TextDisabled("%zu override%s in this instance", inst->overrides.size(), inst->overrides.size() == 1 ? "" : "s");
        else if (!list.empty())
            ImGui::TextDisabled("Overrides: %s", list.c_str());
    }
    ImGui::Separator();
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
        const bool multi = selection_.size() > 1;
        if (multi) ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%zu selected: changes apply to all", selection_.size());
        draw_prefab_section(e);
        begin_inspector_frame(e);
        const json::Value before_frame = multi ? active_scene().save_entity(e) : json::Value();
        const bool changed = inspect_components(e);
        if (changed && multi) apply_multi_edit(e, before_frame);
        if (changed) active_scene().update_transforms();
        end_inspector_frame(e, changed);
        draw_network_section(e);
    }
    if (play_state_ != PlayState::Editing) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Play mode: edits are temporary.");
    }
    ImGui::End();
}

// ---- assets & console ---------------------------------------------------------------------------

void Editor::draw_asset_item(const assets::AssetRecord& r) {
    if (r.kind == assets::AssetKind::Texture && ImGui::BeginPopupContextItem("##texctx")) {
        if (ImGui::MenuItem("Import Settings...")) {
            import_texture_ = r.uuid;
            import_edit_ = assets::TextureImportSettings::from_json(r.settings);
        }
        if (ImGui::MenuItem("Create Sprite Sheet...")) sheet_texture_ = r.uuid;
        if (ImGui::MenuItem("Create Tile Set...")) tileset_texture_ = r.uuid;
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kAssetPayload, &r.uuid, sizeof(Uuid));
        ImGui::Text("%s %s", kind_icon(r.kind), r.path.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (r.kind == assets::AssetKind::Model) instantiate_asset(r.uuid);
        else if (r.kind == assets::AssetKind::Prefab) instantiate_prefab(r.uuid);
        else if ((r.kind == assets::AssetKind::Script && r.path.ends_with(".vgraph")) ||
                 (r.kind == assets::AssetKind::Shader && r.path.ends_with(".vmatgraph"))) {
            if (auto st = open_graph_editor(r.uuid); !st.ok()) notify(st.error().message, true);
        } else if (r.kind == assets::AssetKind::Script) {
            const Entity e = selected();
            if (!e.is_null()) {
                auto before = capture(active_scene(), {active_scene().uuid(e)});
                auto& w = active_scene().world();
                if (!w.has<script::ScriptComponent>(e)) w.add<script::ScriptComponent>(e);
                w.get<script::ScriptComponent>(e)->script = r.path;
                record_edit("Assign script", std::move(before));
            }
        } else if (r.kind == assets::AssetKind::Audio) {
            if (auto* a = audio()) {
                if (auto st = a->play_one_shot(r.path, {.bus = audio::Bus::Ui}); !st) notify(st.error().message, true);
            }
        } else if (r.kind == assets::AssetKind::Material) {
            open_material_editor(r.uuid);
        } else if (r.kind == assets::AssetKind::SpriteSheet) {
            (void)assign_sprite_sheet(selected(), r.uuid);
        } else if (r.kind == assets::AssetKind::Tileset) {
            Entity e = selected();
            if (e.is_null()) {
                e = create_entity(Primitive::Empty);
                rename(e, "Tilemap");
            }
            (void)assign_tileset(e, r.uuid);
        } else if (r.kind == assets::AssetKind::Shader) {
            if (show_material_editor_) (void)set_editing_material_shader(r.uuid);
        } else if (r.kind == assets::AssetKind::Texture) {
            const Entity e = selected();
            if (auto* sp = e.is_null() ? nullptr : active_scene().world().get<SpriteComponent>(e)) {
                auto before = capture(active_scene(), {active_scene().uuid(e)});
                sp->texture_asset = r.uuid;
                record_edit("Assign texture", std::move(before));
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nUUID %s\nDouble-click: %s", r.path.c_str(), r.uuid.to_string().c_str(),
                                                  r.kind == assets::AssetKind::Model || r.kind == assets::AssetKind::Prefab
                                                      ? "add to scene"
                                                  : r.kind == assets::AssetKind::Texture ? "assign to selected sprite"
                                                  : r.kind == assets::AssetKind::Script ? "attach to selected entity"
                                                  : r.kind == assets::AssetKind::Audio  ? "preview"
                                                  : r.kind == assets::AssetKind::Material ? "edit"
                                                  : r.kind == assets::AssetKind::Shader ? "use in the open material"
                                                  : r.kind == assets::AssetKind::SpriteSheet ? "animate the selected entity"
                                                  : r.kind == assets::AssetKind::Tileset ? "paint with it on the selected (or a new) entity"
                                                                                         : "-");
}

void Editor::draw_assets() {
    if (!ImGui::Begin(kAssetsWindow)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Reimport")) (void)reimport_assets();
    ImGui::SameLine();
    if (ImGui::RadioButton("List", !asset_grid_)) asset_grid_ = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", asset_grid_)) asset_grid_ = true;
    if (asset_grid_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("##tile", &asset_tile_size_, 32.0f, 256.0f, "%.0f px");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drop files into %s", project_->assets_dir().string().c_str());
    ImGui::Separator();
    std::vector<assets::AssetRecord> recs = db_ ? db_->records() : std::vector<assets::AssetRecord>{};
    std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    // Draws an asset's thumbnail aspect-fitted into a square cell (or its text icon).
    const auto draw_thumb = [this](const assets::AssetRecord& r, float cell) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell), IM_COL32(40, 42, 48, 255), 3.0f);
        if (auto t = asset_thumbnail(r.uuid)) {
            float w = cell, h = cell;
            if (t->aspect > 1.0f) h = cell / t->aspect;
            else w = cell * t->aspect;
            const ImVec2 a(p0.x + (cell - w) * 0.5f, p0.y + (cell - h) * 0.5f);
            dl->AddImage(ImTextureRef(ui::UiContext::texture_id(t->texture)), a, ImVec2(a.x + w, a.y + h), ImVec2(t->uv.x, t->uv.y),
                         ImVec2(t->uv.z, t->uv.w));
        } else {
            const char* icon = ThumbnailCache::supports(r.kind) && ThumbnailCache::renders(r.kind) ? "..." : kind_icon(r.kind);
            const ImVec2 ts = ImGui::CalcTextSize(icon);
            if (ts.x <= cell - 4.0f) // list rows are too small for the text icons (the Kind column says it)
                dl->AddText(ImVec2(p0.x + (cell - ts.x) * 0.5f, p0.y + (cell - ts.y) * 0.5f), IM_COL32(170, 175, 185, 255), icon);
        }
        ImGui::Dummy(ImVec2(cell, cell));
    };
    if (asset_grid_) {
        if (ImGui::BeginChild("##assetgrid")) {
            const float cell = asset_tile_size_;
            const float pad = ImGui::GetStyle().ItemSpacing.x;
            const float label_h = ImGui::GetTextLineHeightWithSpacing();
            const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (cell + pad)));
            int col = 0;
            for (const auto& r : recs) {
                ImGui::PushID(r.path.c_str());
                if (col > 0) ImGui::SameLine();
                ImGui::BeginGroup();
                const ImVec2 p0 = ImGui::GetCursorPos();
                ImGui::Selectable("##tile", false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(cell, cell + label_h));
                draw_asset_item(r);
                ImGui::SetCursorPos(p0);
                draw_thumb(r, cell);
                const std::string name = fs::Path(r.path).filename().string();
                std::string shown = name;
                while (shown.size() > 3 && ImGui::CalcTextSize(shown.c_str()).x > cell) shown = shown.substr(0, shown.size() - 4) + "..";
                ImGui::TextUnformatted(shown.c_str());
                ImGui::EndGroup();
                ImGui::PopID();
                col = (col + 1) % columns;
            }
        }
        ImGui::EndChild();
    } else if (ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();
        const float row = ImGui::GetTextLineHeight() * 1.6f;
        for (const auto& r : recs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r.path.c_str());
            ImGui::Selectable("##row", false,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, row));
            draw_asset_item(r);
            ImGui::SameLine(0, 0);
            draw_thumb(r, row);
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
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
    if (!import_texture_.is_nil()) ImGui::OpenPopup("Texture Import Settings");
    if (ImGui::BeginPopupModal("Texture Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* tex = db_ ? db_->find(import_texture_) : nullptr;
        ImGui::Text("%s", tex ? tex->path.c_str() : "?");
        int filter = import_edit_.nearest ? 1 : 0;
        const char* filters[] = {"Linear (smooth)", "Nearest (pixel art)"};
        if (ImGui::Combo("Filter", &filter, filters, 2)) import_edit_.nearest = filter == 1;
        ImGui::Checkbox("sRGB colour", &import_edit_.srgb);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off for data textures (normal, metallic-roughness maps)");
        ImGui::Checkbox("Mipmaps", &import_edit_.mipmaps);
        ImGui::Checkbox("Flip Y", &import_edit_.flip_y);
        int max_size = static_cast<int>(import_edit_.max_size);
        if (ImGui::InputInt("Max Size", &max_size, 256)) import_edit_.max_size = static_cast<std::uint32_t>(std::clamp(max_size, 1, 16384));
        {
            // Presets plus whatever custom codec list the .meta already holds.
            const char* presets[] = {"none", "auto", "small", "bc7", "astc4x4", "astc6x6"};
            const char* labels[] = {"None (RGBA8)", "Auto (BC7 + ASTC 4x4)", "Small (BC1 + ASTC 6x6)", "Desktop only (BC7)",
                                    "Mobile only (ASTC 4x4)", "Mobile small (ASTC 6x6)"};
            int current = -1;
            for (int i = 0; i < 6; ++i)
                if (import_edit_.compression == presets[i]) current = i;
            if (ImGui::BeginCombo("GPU Compression", current >= 0 ? labels[current] : import_edit_.compression.c_str())) {
                for (int i = 0; i < 6; ++i)
                    if (ImGui::Selectable(labels[i], i == current)) import_edit_.compression = presets[i];
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Block-compressed copies baked by the cooker; each device loads the first one it supports");
            ImGui::Checkbox("Thorough encoding", &import_edit_.compression_thorough);
        }
        if (ImGui::Button("Apply")) {
            if (auto st = set_texture_import_settings(import_texture_, import_edit_); !st) notify(st.error().message, true);
            import_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            import_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!sheet_texture_.is_nil()) ImGui::OpenPopup("Create Sprite Sheet");
    if (ImGui::BeginPopupModal("Create Sprite Sheet", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* tex = db_ ? db_->find(sheet_texture_) : nullptr;
        ImGui::Text("Slice %s into a grid of frames", tex ? tex->path.c_str() : "?");
        ImGui::InputInt("Columns", &sheet_columns_);
        ImGui::InputInt("Rows", &sheet_rows_);
        ImGui::DragFloat("FPS", &sheet_fps_, 0.5f, 0.5f, 120.0f);
        sheet_columns_ = std::clamp(sheet_columns_, 1, 256);
        sheet_rows_ = std::clamp(sheet_rows_, 1, 256);
        if (ImGui::Button("Create")) {
            auto made = create_sprite_sheet(sheet_texture_, static_cast<std::uint32_t>(sheet_columns_),
                                            static_cast<std::uint32_t>(sheet_rows_), sheet_fps_);
            if (!made) notify(made.error().message, true);
            sheet_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            sheet_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!tileset_texture_.is_nil()) ImGui::OpenPopup("Create Tile Set");
    if (ImGui::BeginPopupModal("Create Tile Set", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* tex = db_ ? db_->find(tileset_texture_) : nullptr;
        ImGui::Text("Slice %s into a grid of tiles", tex ? tex->path.c_str() : "?");
        ImGui::InputInt("Columns", &tileset_columns_);
        ImGui::InputInt("Rows", &tileset_rows_);
        tileset_columns_ = std::clamp(tileset_columns_, 1, 256);
        tileset_rows_ = std::clamp(tileset_rows_, 1, 256);
        const char* collisions[] = {"None", "Solid", "One-way"};
        ImGui::Combo("Collision", &tileset_collision_, collisions, 3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default for every tile; edit per-tile overrides in the .vtileset file");
        if (ImGui::Button("Create")) {
            auto made = create_tileset(tileset_texture_, static_cast<std::uint32_t>(tileset_columns_),
                                       static_cast<std::uint32_t>(tileset_rows_),
                                       static_cast<tilemap::TileCollision>(std::clamp(tileset_collision_, 0, 2)));
            if (!made) notify(made.error().message, true);
            tileset_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            tileset_texture_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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

// ---- input actions -------------------------------------------------------------------------------

void Editor::draw_input_actions() {
    if (!show_input_actions_ || !project_) return;
    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input Actions", &show_input_actions_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Scripts read these with input.action(\"jump\"), input.value(\"move_x\") and "
                       "input.vector(\"move_x\", \"move_y\"). Changes are saved to victus.project.");
    if (!input_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", input_error_.c_str());
    ImGui::Separator();

    gameplay::InputMap map = project_->settings().input;
    bool changed = false;
    int remove_action = -1;
    const auto binding_chips = [&](gameplay::InputAction& a, std::vector<gameplay::InputBinding>& list, bool negative) {
        int remove = -1;
        // Items flow left to right and wrap onto indented lines when the row is full.
        const ImGuiStyle& style = ImGui::GetStyle();
        const auto fit = [&](float width) {
            if (ImGui::GetContentRegionAvail().x < width) {
                ImGui::NewLine();
                ImGui::SetCursorPosX(90);
            }
        };
        for (std::size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string label = gameplay::binding_to_string(list[i]) + "  x";
            fit(ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2);
            if (ImGui::SmallButton(label.c_str())) remove = static_cast<int>(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this binding");
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (remove >= 0) {
            list.erase(list.begin() + remove);
            changed = true;
        }
        const bool listening = capture_action_ == a.name && capture_negative_ == negative;
        fit(ImGui::CalcTextSize(listening ? "press a control... (Esc cancels)" : "+ Listen").x + style.FramePadding.x * 2);
        if (ImGui::SmallButton(listening ? "press a control... (Esc cancels)" : "+ Listen")) {
            if (listening) cancel_binding_capture();
            else begin_binding_capture(a.name, negative);
        }
        // Or type a control by name (e.g. Escape, which Listen uses to cancel).
        ImGui::SameLine();
        fit(130);
        std::string& text = binding_text_[a.name + (negative ? "-" : "+")];
        ImGui::SetNextItemWidth(130);
        if (ImGui::InputTextWithHint("##type", "or type + Enter", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto b = gameplay::parse_binding(text)) {
                list.push_back(*b);
                changed = true;
                text.clear();
            } else {
                input_error_ = b.error().message;
            }
        }
    };
    for (std::size_t ai = 0; ai < map.actions.size(); ++ai) {
        auto& a = map.actions[ai];
        ImGui::PushID(static_cast<int>(ai));
        const bool axis = !a.negative.empty();
        const bool open = ImGui::TreeNodeEx("##action", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "%s   (%s)", a.name.c_str(), axis ? "axis" : "button");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - 60));
        if (ImGui::SmallButton("Delete")) remove_action = static_cast<int>(ai);
        if (open) {
            std::string name = a.name;
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != a.name) {
                a.name = name;
                changed = true;
            }
            ImGui::TextDisabled(axis ? "Positive" : "Bindings");
            ImGui::SameLine(90);
            ImGui::PushID("pos");
            binding_chips(a, a.positive, false);
            ImGui::PopID();
            if (axis) {
                ImGui::TextDisabled("Negative");
                ImGui::SameLine(90);
                ImGui::PushID("neg");
                binding_chips(a, a.negative, true);
                ImGui::PopID();
            } else if (ImGui::SmallButton("Make Axis")) {
                // An axis needs a negative side; start listening for it.
                begin_binding_capture(a.name, true);
            }
            float dz = a.dead_zone;
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("Dead zone", &dz, 0.0f, 0.9f, "%.2f")) a.dead_zone = dz;
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove_action >= 0) {
        map.actions.erase(map.actions.begin() + remove_action);
        changed = true;
    }
    ImGui::Separator();
    if (ImGui::Button("Add Action")) {
        int n = 1;
        std::string name = "action";
        while (map.find(name)) name = "action_" + std::to_string(++n);
        map.actions.push_back({.name = name});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        map = gameplay::default_input_map();
        changed = true;
    }
    if (changed) {
        if (auto st = set_input_map(std::move(map)); !st) input_error_ = st.error().message;
        else input_error_.clear();
    }
    ImGui::End();
}

// ---- modals & shortcuts -------------------------------------------------------------------------

void Editor::draw_modals() {
    draw_recovery_modal();
    if (pending_action_ != Pending::None && !ImGui::IsPopupOpen("Unsaved Changes")) ImGui::OpenPopup("Unsaved Changes");
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before %s?", scene_path_.empty() ? "the untitled scene" : scene_path_.c_str(),
                    pending_action_ == Pending::Quit ? "quitting" : "closing");
        ImGui::Spacing();
        const auto finish = [&] {
            const Pending a = pending_action_;
            pending_action_ = Pending::None;
            ImGui::CloseCurrentPopup();
            if (dirty() && project_) {
                remove_autosave(autosave_written_ ? autosave_scene_ : scene_path_); // Don't Save discards the autosave too
                autosave_written_ = false;
            }
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
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P) || ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        open_command_palette();
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) {
        if (editing) play();
        else stop();
    }
    // Ctrl+1..4 switch the viewed player of a hosted session with in-editor clients (also while
    // the game has the keyboard, so testing several players needs no mouse).
    if (!editing && !play_clients_.empty()) {
        constexpr ImGuiKey keys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};
        for (std::size_t i = 0; i <= play_clients_.size() && i < 4; ++i)
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | keys[i])) set_view_peer(i);
    }
    // While the game has the keyboard, editor shortcuts other than Play/Stop stay out of its way.
    if (!editing && viewport_focused_) return;
    if (graph_focused_) return; // the Graph Editor handles its own save, undo and delete
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && editing) {
        if (scene_path_.empty()) show_save_as_ = true;
        else if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selected();
    const bool mouse_nav = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!mouse_nav && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selected();
    if (!mouse_nav && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selected();
    gizmo_shortcuts();
}

} // namespace victus::editor
