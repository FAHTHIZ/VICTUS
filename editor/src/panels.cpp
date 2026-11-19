// SPDX-License-Identifier: Apache-2.0
// Editor UI: launcher, dockspace workspace and panels (Dear ImGui).
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "editor_client.hpp"

#include "victus/core/build_info.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"
#include "victus/scene/reflect.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <map>
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

// The icon and tint the Hierarchy shows for an entity, from its most telling component.
kit::Icon entity_icon(Scene& s, Entity e, ImVec4& tint) {
    namespace tk = ui::tokens;
    const ecs::World& w = s.world();
    tint = tk::kTextMuted;
    if (w.has<scene::PrefabInstance>(e)) {
        tint = tk::kInfo;
        return kit::Icon::Prefab;
    }
    if (w.has<CameraComponent>(e)) return kit::Icon::Camera;
    if (w.has<LightComponent>(e) || w.has<gameplay::Light2DComponent>(e)) {
        tint = tk::kWarning;
        return kit::Icon::Light;
    }
    if (w.has<terrain::TerrainComponent>(e)) {
        tint = tk::kSuccess;
        return kit::Icon::Terrain;
    }
    if (w.has<tilemap::TilemapComponent>(e)) return kit::Icon::Tileset;
    if (w.has<particles::ParticleEmitterComponent>(e)) return kit::Icon::Particles;
    if (w.has<audio::AudioSourceComponent>(e)) return kit::Icon::Audio;
    if (w.has<gameui::UiCanvasComponent>(e)) return kit::Icon::Ui;
    if (w.has<gameplay::DecalComponent>(e)) return kit::Icon::Texture;
    if (w.has<gameplay::FogVolumeComponent>(e)) return kit::Icon::Particles;
    if (w.has<SpriteComponent>(e)) return kit::Icon::Sprite;
    if (w.has<MeshRendererComponent>(e) || w.has<ProceduralMeshComponent>(e)) return kit::Icon::Mesh;
    if (w.has<script::ScriptComponent>(e)) {
        tint = tk::kSuccess;
        return kit::Icon::Script;
    }
    if (!s.children(e).empty()) return kit::Icon::Folder;
    return kit::Icon::Entity;
}

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

// A reference to an object or asset: a wide button showing the current choice that opens a
// searchable list, a clear button and the label. Also accepts drag and drop of `payload`.
template <class Accept>
bool reference_field(const char* id_label, const std::string& display, const std::string& shown, kit::Icon icon,
                     Uuid& id, const std::vector<std::pair<Uuid, std::string>>& options, const char* payload,
                     Accept accept, const std::string& help) {
    namespace tk = ui::tokens;
    bool changed = false;
    ImGui::PushID(id_label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float full = ImGui::CalcItemWidth();
    const float h = ImGui::GetFrameHeight();
    const float main_w = std::max(h, full - h - st.ItemInnerSpacing.x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    const std::string text = std::string("      ") + shown + "##pick";
    if (ImGui::Button(text.c_str(), ImVec2(main_w, h))) ImGui::OpenPopup("##refpick");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    const float isz = ImGui::GetTextLineHeight();
    kit::draw_icon(ImGui::GetWindowDrawList(), icon, ImVec2(p0.x + st.FramePadding.x, p0.y + (h - isz) * 0.5f), isz,
                   ImGui::GetColorU32(id.is_nil() ? tk::kTextDisabled : tk::kAccent));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", help.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(payload)) {
            Uuid dropped;
            std::memcpy(&dropped, pl->Data, sizeof(Uuid));
            bool ok = true;
            if constexpr (!std::is_same_v<Accept, std::nullptr_t>) ok = accept(dropped);
            if (ok && dropped != id) {
                id = dropped;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::BeginDisabled(id.is_nil());
    if (kit::icon_button("##clear", kit::Icon::Close, "Clear (set to None)")) {
        id = {};
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::TextUnformatted(display.c_str());
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(main_w, 280.0f), 0), ImVec2(FLT_MAX, h * 14));
    if (ImGui::BeginPopup("##refpick")) {
        static std::string query;
        if (ImGui::IsWindowAppearing()) {
            query.clear();
            ImGui::SetKeyboardFocusHere();
        }
        (void)kit::search_input("##refq", "Search", query, -1);
        ImGui::Separator();
        if (ImGui::Selectable("None", id.is_nil())) {
            if (!id.is_nil()) changed = true;
            id = {};
        }
        std::size_t shown_count = 0;
        for (const auto& [uuid, name] : options) {
            if (!query.empty() && !kit::contains_icase(name, query)) continue;
            ++shown_count;
            ImGui::PushID(&uuid);
            if (ImGui::Selectable(name.c_str(), uuid == id) && uuid != id) {
                id = uuid;
                changed = true;
            }
            ImGui::PopID();
        }
        if (shown_count == 0)
            ImGui::TextDisabled(options.empty() ? "Nothing of this type in the project yet" : "Nothing matches");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Editor widgets for one reflected field. Returns true when the value changed.
bool field_edit(const scene::FieldInfo& f, void* comp, assets::AssetDatabase* db, const scene::Scene* scene) {
    // Readable label, stable ID: "linear_damping" shows as "Linear Damping".
    const std::string shown_label = kit::humanize(f.label.empty() ? f.name : f.label) + "##" + f.name;
    const char* label = shown_label.c_str();
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
    case scene::FieldKind::Vec2List: {
        // Outline points: one row per point, "+" appends a copy of the last point, "x" removes.
        auto& list = f.ref<std::vector<math::Vec2>>(comp);
        const std::string head = shown_label.substr(0, shown_label.find("##")) + " (" + std::to_string(list.size()) + ")##" + f.name;
        if (ImGui::TreeNode(head.c_str())) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("x")) {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                changed |= vec2_edit(std::to_string(i).c_str(), list[i], speed);
                ImGui::PopID();
            }
            if (list.size() < scene::kMaxListLength && ImGui::SmallButton("+ point")) {
                list.push_back(list.empty() ? math::Vec2{} : list.back() + math::Vec2{0.5f, 0});
                changed = true;
            }
            ImGui::TreePop();
        }
        break;
    }
    case scene::FieldKind::Color: changed = color_edit(label, f.ref<math::Color>(comp)); break;
    case scene::FieldKind::String: {
        auto& str = f.ref<std::string>(comp);
        changed = ImGui::InputText(label, &str);
        // Asset-path fields accept a drop from the asset table when the kind matches the hint.
        const assets::AssetKind want = f.hint == "audio"           ? assets::AssetKind::Audio
                                       : f.hint == "sequence"      ? assets::AssetKind::Sequence
                                       : f.hint == "behavior_tree" ? assets::AssetKind::BehaviorTree
                                       : f.hint == "anim_controller" ? assets::AssetKind::AnimController
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
        const std::string preview =
            cur >= 0 && cur < static_cast<int>(f.enum_names.size()) ? kit::humanize(f.enum_names[cur]) : "?";
        if (ImGui::BeginCombo(label, preview.c_str())) {
            for (int i = 0; i < static_cast<int>(f.enum_names.size()); ++i) {
                const std::string item = kit::humanize(f.enum_names[i]) + "##" + f.enum_names[i];
                if (ImGui::Selectable(item.c_str(), i == cur) && i != cur) {
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
        const std::string display = kit::humanize(f.label.empty() ? f.name : f.label);
        if (f.hint == "entity") {
            // Entity reference: click to pick from the scene, or drop an entity from the Hierarchy.
            std::string shown = "None";
            if (!id.is_nil()) {
                const Entity target = scene ? scene->find(id) : Entity{};
                const auto* n = target.is_null() ? nullptr : scene->world().get<scene::Name>(target);
                shown = n ? n->value : "Missing object";
            }
            std::vector<std::pair<Uuid, std::string>> options;
            if (scene) {
                std::vector<Entity> stack = scene->roots();
                while (!stack.empty() && options.size() < 2000) {
                    const Entity e = stack.back();
                    stack.pop_back();
                    const auto* n = scene->world().get<scene::Name>(e);
                    options.emplace_back(scene->uuid(e), n ? n->value : std::string("Entity"));
                    for (const Entity c : scene->children(e)) stack.push_back(c);
                }
                std::ranges::sort(options, {}, &std::pair<Uuid, std::string>::second);
            }
            changed = reference_field(label, display, shown, kit::Icon::Entity, id, options, kEntityPayload, nullptr,
                                      "Click to choose an object, or drag one here from the Hierarchy");
        } else if (const auto kind = asset_kind_for_hint(f.hint); kind != assets::AssetKind::Unknown) {
            // Asset reference: click to pick an asset of the right type, or drop one from Assets.
            const std::string path = id.is_nil() ? std::string() : asset_label(db, id);
            const std::string shown = id.is_nil() ? std::string("None") : std::filesystem::path(path).filename().string();
            std::vector<std::pair<Uuid, std::string>> options;
            if (db)
                for (const auto& r : db->records())
                    if (r.kind == kind) options.emplace_back(r.uuid, r.path);
            std::ranges::sort(options, {}, &std::pair<Uuid, std::string>::second);
            const auto accept = [db, kind](const Uuid& dropped) {
                const auto* rec = db ? db->find(dropped) : nullptr;
                return rec && rec->kind == kind;
            };
            changed = reference_field(label, display, shown, kit::icon_for(kind), id, options, kAssetPayload, accept,
                                      std::format("{}{}Click to choose: {}. You can also drag one here from Assets.", path,
                                                  path.empty() ? "" : "\n\n", Editor::asset_kind_label(kind)));
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

} // namespace

// ---- frame --------------------------------------------------------------------------------------

void Editor::frame(float dt) {
    // While a script is stopped in the debugger this runs as a nested frame inside that script
    // call (ADR-0106): draw the Studio, but advance nothing that the stopped game uses.
    const bool stopped = debug_stopped();
    update_startup();
    update_profiler();
    if (play_state_ == PlayState::Editing && !stopped) update_autosave(dt);
    if (dt > 0) fps_ = fps_ <= 0 ? 1.0f / dt : fps_ * 0.95f + 0.05f / dt;
    view_->renderer().set_frame_time(dt); // auto exposure adapts in real time
    if (status_time_ > 0) status_time_ -= dt;
    if (input_ && capturing_binding()) {
        input_->set_enabled(true); // "press a control to bind" listens to every device
        (void)capture_from(*input_);
    }
    if (input_) input_->set_enabled(play_state_ == PlayState::Playing && viewport_focused_ && !stopped);
    if (stopped) {
        // the game is stopped in the debugger
    } else if (play_state_ == PlayState::Playing && play_scene_) {
        tick_play(dt);
        if (debug_stop_after_resume_) { // Stop was pressed while a script was stopped
            debug_stop_after_resume_ = false;
            stop();
        }
    } else if (!play_scene_) {
        if (edit_sprites_) edit_sprites_->update_sprites(active_scene(), 0); // frame preview
        if (terrains_) terrains_->update(active_scene());                   // terrain chunks in edit mode
        if (tilemaps_) tilemaps_->update(active_scene());                   // tile sets for drawing
    }
    if (!stopped) {
        update_graph_debug(dt);
        update_sequence_preview(dt);
        update_animation_preview(dt);
    }
    // Inspector property discovery follows script edits made in an external editor.
    if (!stopped && (script_poll_ += dt) >= 1.0f) {
        script_poll_ = 0;
        if (edit_scripts_) (void)edit_scripts_->reload_changed();
        (void)check_asset_changes(); // textures, models and audio edited outside the editor
        sync_graph_from_disk();
    }
    if (!project_) draw_launcher();
    else draw_workspace(dt);
    draw_modals();
    draw_export_dialog();
    draw_diagnostics_dialog();
    draw_about_dialog();
    draw_crash_notice();
    draw_safe_mode_card();
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
    observe_guide();
    apply_ui_scale(); // takes effect from the next frame
}

// ---- launcher -----------------------------------------------------------------------------------

// ---- workspace ----------------------------------------------------------------------------------

void Editor::draw_workspace(float) {
    // A stopped script keeps the Studio read-only except for the Script Debugger (ADR-0106).
    const bool stopped = debug_stopped();
    ImGui::BeginDisabled(stopped);
    draw_menu_bar();
    draw_toolbar();
    ImGui::EndDisabled();
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
    if (!layout_built_ || layout_rebuild_) {
        // First frame: build the default layout unless the saved one (editor_layout.ini) has
        // docked panels. Reset Layout and the presets always rebuild.
        if (layout_rebuild_ || !ImGui::DockBuilderGetNode(dock_id) || ImGui::DockBuilderGetNode(dock_id)->IsLeafNode())
            build_layout(dock_id, layout_);
        layout_built_ = true;
        layout_rebuild_ = false;
    }
    ImGui::DockSpace(dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    ImGui::BeginDisabled(stopped);
    draw_viewport();
    draw_hierarchy();
    draw_inspector();
    draw_assets();
    draw_console();
    draw_problems();
    draw_input_actions();
    draw_network();
    draw_profiler();
    draw_command_palette();
    draw_material_editor();
    draw_graph_editor();
    draw_tile_palette();
    draw_terrain_tools();
    draw_ai_debugger();
    draw_animation_panel();
    draw_sequencer();
    draw_getting_started();
    draw_help();
    draw_history();
    draw_project_settings();
    ImGui::EndDisabled();
    draw_script_debugger();
    draw_status_bar();
    if (focus_panel_ >= 0) {
        // The window exists from the frame after it opens; try until it does (a few frames at most).
        const char* id = panel_title(static_cast<Panel>(focus_panel_));
        if (static_cast<Panel>(focus_panel_) == Panel::GraphEditor) id = "###GraphEditor";
        if (static_cast<Panel>(focus_panel_) == Panel::Problems) id = "###Problems";
        if (ImGui::FindWindowByName(id)) {
            ImGui::SetWindowFocus(id);
            focus_panel_ = -1;
        } else if (++focus_attempts_ > 3) {
            focus_panel_ = -1;
        }
        if (focus_panel_ < 0) focus_attempts_ = 0;
    }
    if (!stopped) handle_shortcuts(); // the Script Debugger handles its own keys while stopped
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
        if (ImGui::MenuItem("Export Game...", nullptr, false, editing && project_.has_value() && !exporting()))
            open_export_dialog();
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
        if (ImGui::MenuItem("Undo History", nullptr, show_history_)) show_panel(Panel::History);
        ImGui::Separator();
        if (ImGui::MenuItem("Project Settings...", nullptr, show_project_settings_)) show_panel(Panel::ProjectSettings);
        ImGui::MenuItem("Input Actions...", nullptr, &show_input_actions_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
        draw_create_entity_items(kNullEntity);
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
        ImGui::MenuItem("Sequencer", nullptr, &show_sequencer_);
        if (ImGui::MenuItem("Copy Assets Folder Path") && platform_) platform_->set_clipboard(project_->assets_dir().string());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P")) open_command_palette();
        if (ImGui::BeginMenu("Interface Size")) {
            for (const UiScaleChoice& c : kUiScales) {
                const std::string label = std::format("{}  ({:.0f} %)", c.name, static_cast<double>(c.scale) * 100.0);
                if (ImGui::MenuItem(label.c_str(), nullptr, std::abs(ui_scale_ - c.scale) < 1e-3f)) set_ui_scale(c.scale);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Bigger", "Ctrl+=")) step_ui_scale(1);
            if (ImGui::MenuItem("Smaller", "Ctrl+-")) step_ui_scale(-1);
            if (ImGui::MenuItem("Default Size", "Ctrl+0")) step_ui_scale(0);
            ImGui::EndMenu();
        }
        kit::tooltip("Make all text and buttons bigger or smaller");
        ImGui::Separator();
        ImGui::MenuItem("Physics Colliders", nullptr, &show_colliders_);
        ImGui::MenuItem("Navigation Mesh", nullptr, &show_navmesh_);
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
        if (ImGui::MenuItem("Overdraw View", nullptr, debug_view_ == render::DebugView::Overdraw))
            set_debug_view(debug_view_ == render::DebugView::Overdraw ? render::DebugView::None : render::DebugView::Overdraw);
        ImGui::EndMenu();
    }
    draw_window_menu();
    if (ImGui::BeginMenu("Play")) {
        if (ImGui::MenuItem(editing ? "Play" : "Resume", editing ? "Ctrl+P" : nullptr, false, play_state_ != PlayState::Playing)) play();
        if (ImGui::MenuItem("Pause", nullptr, false, play_state_ == PlayState::Playing)) pause();
        if (ImGui::MenuItem("Stop", "Ctrl+P", false, !editing)) stop();
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
    if (ImGui::BeginMenu("Help")) {
        draw_help_menu_items();
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette", "Ctrl+Shift+P")) open_command_palette();
        ImGui::Separator();
        if (ImGui::MenuItem("Report a Problem (Diagnostics Bundle)...", nullptr, false, !creating_bundle()))
            open_diagnostics_dialog();
        if (ImGui::MenuItem("Copy Diagnostics Folder Path", nullptr, false, platform_ != nullptr) && platform_)
            platform_->set_clipboard(diagnostics_dir().string());
        if (safe_mode_) {
            if (ImGui::MenuItem("Restart Normally", nullptr, false, !dirty()))
                if (auto st = reopen_normally(); !st.ok()) notify("cannot restart normally: " + st.error().message, true);
        } else if (ImGui::MenuItem("Reopen in Safe Mode", nullptr, false, !dirty() && play_state_ == PlayState::Editing)) {
            const fs::Path root = project_->root();
            close_project();
            if (auto st = open_project_safe(root); !st.ok()) notify("cannot reopen in safe mode: " + st.error().message, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About VICTUS Studio")) open_about_dialog();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::draw_toolbar() {
    // Under the menu: file actions on the left, play controls centred, play state and the
    // command search on the right. Every control is an icon button with a tooltip naming its
    // shortcut, so the bar stays compact without hiding what anything does.
    namespace tk = ui::tokens;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    const float pad = tk::kSpace2;
    const float h = ImGui::GetFrameHeight() + pad * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace3, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(tk::kSpace2, tk::kSpace2));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tk::kSurfaceBase);
    if (ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, h, flags)) {
        const bool editing = play_state_ == PlayState::Editing;
        const float side = ImGui::GetFrameHeight();
        const float gap = ImGui::GetStyle().ItemSpacing.x;

        // Left: save, undo, redo.
        const std::string save_tip = !project_ ? std::string("Save Scene (Ctrl+S) - open a project first")
                                     : !editing ? std::string("Save Scene (Ctrl+S) - stop play mode first")
                                     : dirty()  ? std::format("Save {} (Ctrl+S) - unsaved changes", scene_path_.empty() ? "scene" : scene_path_)
                                                : std::string("Save Scene (Ctrl+S) - no unsaved changes");
        if (kit::icon_button("save", kit::Icon::Save, save_tip.c_str(), false, project_.has_value() && editing,
                             dirty() ? tk::kAccent : tk::kText)) {
            if (scene_path_.empty()) show_save_as_ = true;
            else if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
        }
        ImGui::SameLine();
        const auto* nu = undo_.next_undo();
        const auto* nr = undo_.next_redo();
        const std::string undo_tip = nu ? std::format("Undo {} (Ctrl+Z)", nu->label) : std::string("Nothing to undo (Ctrl+Z)");
        const std::string redo_tip = nr ? std::format("Redo {} (Ctrl+Y)", nr->label) : std::string("Nothing to redo (Ctrl+Y)");
        if (kit::icon_button("undo", kit::Icon::Undo, undo_tip.c_str(), false, editing && nu != nullptr)) undo();
        ImGui::SameLine();
        if (kit::icon_button("redo", kit::Icon::Redo, redo_tip.c_str(), false, editing && nr != nullptr)) redo();
        ImGui::SameLine(0, tk::kSpace4);
        if (kit::icon_button("export", kit::Icon::Export,
                             exporting() ? "Exporting the game..." : "Export Game: build a folder with your game that runs without VICTUS",
                             false, editing && project_.has_value() && !exporting()))
            open_export_dialog();

        // Centre: play, pause, stop, record.
        const float group_w = side * 4.0f + gap * 3.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + tk::kSpace6, (ImGui::GetWindowWidth() - group_w) * 0.5f));
        const bool host = net_settings_.mode == NetPlaySettings::Mode::Host;
        if (editing) {
            if (kit::icon_button("play", kit::Icon::Play, host ? "Play as a listen server (Ctrl+P) - Play > Network Mode" : "Play (Ctrl+P)",
                                 false, true, tk::kSuccess))
                play();
        } else if (play_state_ == PlayState::Paused) {
            if (kit::icon_button("resume", kit::Icon::Play, "Resume", false, true, tk::kSuccess)) play();
        } else {
            kit::icon_button("playing", kit::Icon::Play, "Playing - edits made now are temporary", true, true);
        }
        ImGui::SameLine();
        if (kit::icon_button("pause", kit::Icon::Pause, "Pause", play_state_ == PlayState::Paused, play_state_ == PlayState::Playing))
            pause();
        ImGui::SameLine();
        if (kit::icon_button("stop", kit::Icon::Stop, "Stop and restore the edited scene (Ctrl+P)", false, !editing,
                             tk::kDanger))
            stop();
        ImGui::SameLine();
        if (kit::icon_button("record", kit::Icon::Record,
                             recording() ? "Recording input for a deterministic replay"
                                         : "Play while recording input for a deterministic replay (Play > Replay Capture)",
                             recording(), editing && project_.has_value(), tk::kDanger))
            record();

        // Right of the play group: what the session is doing.
        ImGui::SameLine(0, tk::kSpace4);
        if (play_server_) {
            const auto d = play_server_->diagnostics();
            kit::pill(std::format("Host - {} players", d.clients + 1), ui::Tone::Info);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%u players, %.1f kbps out - click for Window > Network", d.clients + 1, static_cast<double>(d.send_kbps));
            if (ImGui::IsItemClicked()) show_network_ = true;
            if (!play_clients_.empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130);
                if (ImGui::BeginCombo("##viewpeer", peer_label(view_peer_).c_str())) {
                    for (std::size_t i = 0; i <= play_clients_.size(); ++i)
                        if (ImGui::Selectable(std::format("{}  (Ctrl+{})", peer_label(i), i + 1).c_str(), view_peer_ == i)) set_view_peer(i);
                    ImGui::EndCombo();
                }
                kit::tooltip("Which player the viewport shows and the keyboard drives");
            }
        } else if (recording()) {
            kit::pill("Recording", ui::Tone::Danger);
        } else if (replay_ && !replay_result_.empty()) {
            kit::pill(replay_diverged_ ? "Replay diverged" : "Replay matched", replay_diverged_ ? ui::Tone::Warning : ui::Tone::Success);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", replay_result_.c_str());
        } else if (replay_) {
            kit::pill(std::format("Replaying {}/{}", replay_->frame(), replay_capture_ ? replay_capture_->frames.size() : std::size_t{0}),
                      ui::Tone::Info);
        } else if (play_state_ == PlayState::Playing) {
            kit::pill("Playing", ui::Tone::Success);
        } else if (play_state_ == PlayState::Paused) {
            kit::pill("Paused", ui::Tone::Warning);
        }

        // Far right: the command search, so every action is one search away.
        const char* hint = "Search commands";
        const char* keys = "Ctrl+Shift+P";
        const float sw = std::min(300.0f, std::max(170.0f, ImGui::GetWindowWidth() * 0.18f));
        const float sx = ImGui::GetWindowWidth() - sw - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() + tk::kSpace4 < sx) {
            ImGui::SetCursorPosX(sx);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##cmdsearch", ImVec2(sw, side))) open_command_palette();
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + side), ImGui::GetColorU32(hov ? tk::kSurfaceHover : tk::kSurfaceRaised), tk::kRadius);
            dl->AddRect(p, ImVec2(p.x + sw, p.y + side), ImGui::GetColorU32(tk::kBorder), tk::kRadius);
            const float ih = ImGui::GetTextLineHeight();
            const float ty = p.y + (side - ih) * 0.5f;
            kit::draw_icon(dl, kit::Icon::Search, ImVec2(p.x + tk::kSpace3, ty + 1), ih - 2, ImGui::GetColorU32(tk::kTextMuted));
            dl->AddText(ImVec2(p.x + tk::kSpace3 + ih + tk::kSpace2, ty), ImGui::GetColorU32(tk::kTextMuted), hint);
            const float kw = ImGui::CalcTextSize(keys).x;
            if (ImGui::CalcTextSize(hint).x + kw + ih + tk::kSpace6 * 2 < sw)
                dl->AddText(ImVec2(p.x + sw - kw - tk::kSpace3, ty), ImGui::GetColorU32(tk::kTextDisabled), keys);
            kit::tooltip("Find and run any editor command by name");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void Editor::draw_status_bar() {
    // Project and scene on the left (with an unsaved-changes marker), the latest notification in
    // the middle, problem counts (click: Console) and performance on the right.
    namespace tk = ui::tokens;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    const float line = ImGui::GetTextLineHeight();
    const float h = line + tk::kSpace2 * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace3, tk::kSpace2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(tk::kSpace2, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tk::kSurfaceBase);
    if (ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, h, flags)) {
        const auto sep = [] {
            ImGui::SameLine(0, tk::kSpace4);
            ImGui::TextColored(tk::kBorder, "|");
            ImGui::SameLine(0, tk::kSpace4);
        };
        if (project_) {
            kit::icon(kit::Icon::Folder);
            ImGui::SameLine();
            ImGui::TextUnformatted(project_->settings().name.c_str());
            sep();
        }
        kit::icon(kit::Icon::Scene, tk::kAccent);
        ImGui::SameLine();
        ImGui::TextUnformatted(scene_path_.empty() ? "Untitled scene" : scene_path_.c_str());
        ImGui::SameLine();
        if (dirty()) {
            ImGui::TextColored(tk::kWarning, "  Unsaved");
            kit::tooltip("This scene has unsaved changes (Ctrl+S saves)");
        } else if (!scene_path_.empty()) {
            ImGui::TextColored(tk::kTextMuted, "  Saved");
        }
        sep();
        ImGui::TextColored(tk::kTextMuted, "%zu entities", active_scene().size());
        if (status_time_ > 0 && !status_line_.empty()) {
            sep();
            ImGui::TextUnformatted(status_line_.c_str());
        }

        // Right-aligned: errors, warnings, fps.
        const ConsoleCounts cc = console_counts();
        const std::string errs = std::format("{}", cc.errors);
        const std::string warns = std::format("{}", cc.warnings);
        const std::string perf = std::format("{:.0f} fps", static_cast<double>(fps_));
        const std::size_t problem_errors = problems_ ? problems_->errors() : 0;
        const std::size_t problem_count = problems_ ? problems_->diagnostics.size() : 0;
        std::string probs = "Not checked";
        if (!problems_ && checking_project()) probs = "Checking...";
        else if (problems_ && problem_count == 0) probs = "No problems";
        else if (problems_) probs = std::format("{} problem{}", problem_count, problem_count == 1 ? "" : "s");
        const float right = line * 3 + ImGui::CalcTextSize(errs.c_str()).x + ImGui::CalcTextSize(warns.c_str()).x +
                            ImGui::CalcTextSize(probs.c_str()).x + ImGui::CalcTextSize(perf.c_str()).x + tk::kSpace4 * 5 +
                            tk::kSpace2 * 3;
        const float x = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - right;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < x) ImGui::SetCursorPosX(x);
        const auto counter = [&](const char* id, kit::Icon ic, const std::string& n, ImVec4 col, const char* tip,
                                 Panel target = Panel::Console) {
            ImGui::PushID(id);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::BeginGroup();
            kit::icon(ic, col);
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", n.c_str());
            ImGui::EndGroup();
            const ImVec2 sz = ImGui::GetItemRectSize();
            ImGui::SetCursorScreenPos(p0);
            if (ImGui::InvisibleButton("##hit", sz)) {
                if (target == Panel::Console) focus_console();
                else show_panel(target);
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            kit::tooltip(tip);
            ImGui::PopID();
        };
        ImVec4 prob_col = tk::kTextMuted;
        if (problem_errors) prob_col = tk::kDanger;
        else if (problem_count) prob_col = tk::kWarning;
        else if (problems_) prob_col = tk::kSuccess;
        counter("problems", problems_ && problem_count == 0 ? kit::Icon::Check : kit::Icon::Problems, probs, prob_col,
                "Problems found in the project's scenes, scripts and assets - click to see them", Panel::Problems);
        ImGui::SameLine(0, tk::kSpace4);
        counter("errors", kit::Icon::Error, errs, cc.errors ? tk::kDanger : tk::kTextMuted, "Errors in the Console - click to open it");
        ImGui::SameLine(0, tk::kSpace4);
        counter("warnings", kit::Icon::Warning, warns, cc.warnings ? tk::kWarning : tk::kTextMuted,
                "Warnings in the Console - click to open it");
        ImGui::SameLine(0, tk::kSpace4);
        ImGui::TextColored(tk::kTextMuted, "%s", perf.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
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
        // Overlay: view mode and render stats on a translucent backdrop, below the gizmo strip.
        {
            namespace tk = ui::tokens;
            const bool two_d = runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD;
            const std::string tier(render::quality_tier_name(quality_));
            const auto& rs = render_stats();
            std::vector<std::string> lines;
            lines.push_back(std::format("{} {}  |  {} quality  |  {} meshes ({} draws, {:.1f}k tris, {} at LOD, {:.1f}k shadow tris)  {} sprites  {} lights",
                                        two_d ? "2D" : "3D", play_state_ == PlayState::Editing ? "editor camera" : "game camera", tier,
                                        last_view_stats_.extract.meshes, rs.main_batches, static_cast<double>(rs.triangles) / 1000.0,
                                        rs.lod_draws, static_cast<double>(rs.shadow_triangles) / 1000.0, last_view_stats_.sprites,
                                        last_view_stats_.extract.lights));
            if (!two_d && rs.temporal_upscale)
                lines.push_back(std::format("temporal upscale from {}x{}", rs.render_width, rs.render_height));
            // Shading cost apart from geometry: alpha tests, project shaders and blended layers.
            if (!two_d && (rs.alpha_tested_draws > 0 || rs.surface_shader_draws > 0 || rs.transparent_draws > 0))
                lines.push_back(std::format("shading: {} alpha-tested, {} surface-shader, {} blended ({:.1f}k tris)", rs.alpha_tested_draws,
                                            rs.surface_shader_draws, rs.transparent_draws,
                                            static_cast<double>(rs.transparent_triangles) / 1000.0));
            // Rendering-path diagnostics (ADR-0103): what runs below the Ultra reference and why.
            if (!two_d) {
                // Compact: the quality tier is already on the first line, so only other reasons are named.
                std::string reduced;
                for (const auto& f : view_->renderer().path_report().features) {
                    using render::PathReason, render::PathState;
                    const bool lowered = f.state == PathState::Reduced ||
                                         (f.state == PathState::Off && (f.reason == PathReason::QualityTier || f.reason == PathReason::Setting ||
                                                                        f.reason == PathReason::Budget || f.reason == PathReason::Unsupported));
                    if (!lowered) continue;
                    if (!reduced.empty()) reduced += ", ";
                    reduced += f.name;
                    if (f.state == PathState::Off) reduced += " off";
                    if (f.reason != PathReason::QualityTier) reduced += std::format(" ({})", render::path_reason_name(f.reason));
                }
                if (!reduced.empty()) lines.push_back("below ultra: " + reduced);
            }
            if (!two_d && debug_view_ == render::DebugView::Overdraw)
                lines.push_back("overdraw view: layers per pixel, black 0  blue 1  green 2  yellow 3  orange 4  red 5-7  white 8+");
            const float top = play_state_ == PlayState::Editing ? ImGui::GetFrameHeight() + tk::kSpace2 * 2.0f + tk::kSpace3 * 2.0f
                                                                : tk::kSpace3;
            const float lh = ImGui::GetTextLineHeight();
            float tw = 0;
            for (const auto& l : lines) tw = std::max(tw, ImGui::CalcTextSize(l.c_str()).x);
            const ImVec2 b0(origin.x + tk::kSpace3, origin.y + top);
            const ImVec2 b1(b0.x + std::min(tw, avail.x - tk::kSpace5 * 2.0f) + tk::kSpace3 * 2.0f,
                            b0.y + lh * static_cast<float>(lines.size()) + tk::kSpace2 * 2.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
            dl->AddRectFilled(b0, b1, ImGui::GetColorU32(ui::with_alpha(tk::kSurfaceBase, 0.62f)), tk::kRadius);
            for (std::size_t i = 0; i < lines.size(); ++i)
                dl->AddText(ImVec2(b0.x + tk::kSpace3, b0.y + tk::kSpace2 + lh * static_cast<float>(i)), ImGui::GetColorU32(tk::kTextMuted),
                            lines[i].c_str());
            dl->PopClipRect();
        }
        const bool two_d = runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD;
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

void Editor::draw_hierarchy_node(Entity e, int depth) {
    Scene& s = active_scene();
    const Uuid id = s.uuid(e);
    const bool filtering = !hierarchy_filter_.empty();
    if (filtering && !hierarchy_keep_.contains(id)) return;
    const std::vector<Entity> kids = s.children(e);
    const Name* n = s.world().get<Name>(e);
    ++hierarchy_rows_;
    // Only top-level entities start expanded, so deep trees (a character's bones) stay tidy;
    // a search, or selecting something inside a collapsed branch, opens the way to it.
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (depth == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    else if (filtering || hierarchy_reveal_.contains(id)) ImGui::SetNextItemOpen(true);
    const bool dim = filtering && !kit::contains_icase(n ? std::string_view(n->value) : std::string_view("Entity"),
                                                       hierarchy_filter_);
    if (is_selected(e)) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(static_cast<int>(e.index));
    const bool prefab_root = s.world().has<scene::PrefabInstance>(e);
    const bool prefab_part = !prefab_root && s.world().has<scene::PrefabPart>(e);
    ImVec4 icon_tint;
    const kit::Icon icon = entity_icon(s, e, icon_tint);
    // The label is indented by blanks wide enough for the type icon, drawn over them below.
    const float icon_size = ImGui::GetTextLineHeight();
    const float blank = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    const int pad = static_cast<int>(std::ceil((icon_size + ui::tokens::kSpace2) / blank));
    const bool tinted = prefab_root || prefab_part || dim;
    if (tinted)
        ImGui::PushStyleColor(ImGuiCol_Text, dim           ? ui::tokens::kTextMuted
                                             : prefab_root ? ui::tokens::kInfo
                                                           : ui::with_alpha(ui::tokens::kInfo, 0.78f));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%*s%s", pad, "", n ? n->value.c_str() : "Entity");
    if (tinted) ImGui::PopStyleColor();
    if (hierarchy_scroll_ && !selection_.empty() && selection_.back() == id) {
        ImGui::SetScrollHereY(0.4f);
        hierarchy_scroll_ = false;
    }
    {
        const ImVec2 r0 = ImGui::GetItemRectMin();
        const float x = r0.x + ImGui::GetTreeNodeToLabelSpacing();
        const float y = r0.y + (ImGui::GetItemRectSize().y - icon_size) * 0.5f;
        const float inset = std::round(icon_size * 0.08f);
        kit::draw_icon(ImGui::GetWindowDrawList(), icon, ImVec2(x + inset, y + inset), icon_size - 2 * inset,
                       ImGui::GetColorU32(icon_tint));
    }
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
        if (ImGui::BeginMenu("Create Child")) {
            draw_create_entity_items(e);
            ImGui::EndMenu();
        }
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
            if (!s.find(id).is_null()) draw_hierarchy_node(c, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::collect_hierarchy_matches(Entity e, std::unordered_set<Uuid, UuidHash>& keep, bool& any) {
    Scene& s = active_scene();
    const Name* n = s.world().get<Name>(e);
    const bool self = kit::contains_icase(n ? std::string_view(n->value) : std::string_view("Entity"), hierarchy_filter_);
    bool below = false;
    for (const Entity c : s.children(e)) collect_hierarchy_matches(c, keep, below);
    if (self) ++hierarchy_matches_;
    if (self || below) {
        keep.insert(s.uuid(e));
        any = true;
    }
}

void Editor::draw_create_entity_items(Entity parent) {
    namespace tk = ui::tokens;
    const bool editing = play_state_ == PlayState::Editing;
    const auto item = [&](const char* label, kit::Icon ic, const char* help, Primitive p) {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const std::string text = std::string("      ") + label;
        if (ImGui::MenuItem(text.c_str())) create_entity(p, parent); // selects the new entity
        kit::draw_icon(ImGui::GetWindowDrawList(), ic, ImVec2(pos.x, pos.y + (ImGui::GetItemRectSize().y - h) * 0.5f), h,
                       ImGui::GetColorU32(tk::kTextMuted));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", help);
    };
    ImGui::SeparatorText("Basic");
    item("Empty", kit::Icon::Entity, "An empty entity: a folder for grouping, or a spot to attach components", Primitive::Empty);
    ImGui::SeparatorText("3D Shapes");
    item("Cube", kit::Icon::Cube, "A 1 m box with physics-ready size", Primitive::Cube);
    item("Sphere", kit::Icon::Mesh, "A 1 m ball", Primitive::Sphere);
    item("Plane", kit::Icon::Mesh, "A flat 10 m floor", Primitive::Plane);
    item("Cylinder", kit::Icon::Mesh, "A 1 m pillar", Primitive::Cylinder);
    ImGui::SeparatorText("Lights and Camera");
    item("Sun (Directional Light)", kit::Icon::Light, "Light from far away that lights the whole scene and casts shadows",
         Primitive::DirectionalLight);
    item("Point Light", kit::Icon::Light, "A bulb that lights everything around it", Primitive::PointLight);
    item("Spot Light", kit::Icon::Light, "A cone of light, like a torch or stage light", Primitive::SpotLight);
    item("Camera", kit::Icon::Camera, "What the player sees when the game runs", Primitive::Camera);
    if (project_) {
        ImGui::SeparatorText("Characters");
        const auto character = [&](const char* label, const char* help, auto&& fn) {
            const float h = ImGui::GetTextLineHeight();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::MenuItem((std::string("      ") + label).c_str(), nullptr, false, editing)) fn();
            kit::draw_icon(ImGui::GetWindowDrawList(), kit::Icon::Model,
                           ImVec2(pos.x, pos.y + (ImGui::GetItemRectSize().y - h) * 0.5f), h, ImGui::GetColorU32(tk::kTextMuted));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", help);
        };
        const math::Vec3 at = parent.is_null() ? camera_.target : math::Vec3{};
        character("Vanguard V-01", "The starter robot with broad shoulders: animated, ready for a script or a ragdoll",
                  [&] { (void)add_character("v01", parent, at); });
        character("Vanguard V-02", "The starter robot with swept shoulders: animated, ready for a script or a ragdoll",
                  [&] { (void)add_character("v02", parent, at); });
        character("Ragdoll Lab", "Both Vanguards as ragdolls on a floor: press Play to watch V-01 go limp and V-02 stumble",
                  [&] { (void)add_ragdoll_lab(); });
    }
    ImGui::SeparatorText("2D");
    item("Sprite", kit::Icon::Sprite, "A 2D image; drop a texture on it in the Inspector", Primitive::Sprite);
    item("2D Light", kit::Icon::Light, "Lights sprites in a 2D scene", Primitive::Light2D);
    ImGui::SeparatorText("World and Effects");
    item("Decal", kit::Icon::Texture, "Projects a texture onto surfaces, like a sticker or a stain", Primitive::Decal);
    item("Fog Volume", kit::Icon::Particles, "A box of extra fog (shows when the Environment has volumetric fog on)", Primitive::FogVolume);
    if (parent.is_null()) {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::MenuItem("      Terrain", nullptr, false, db_ != nullptr && editing))
            if (auto t = create_terrain(); !t) notify("cannot create terrain: " + t.error().message, true);
        kit::draw_icon(ImGui::GetWindowDrawList(), kit::Icon::Terrain,
                       ImVec2(pos.x, pos.y + (ImGui::GetItemRectSize().y - h) * 0.5f), h,
                       ImGui::GetColorU32(tk::kTextMuted));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A large ground you sculpt and paint with Window > Terrain Tools");
    }
}

void Editor::draw_hierarchy() {
    if (!core_open_[1]) return;
    if (!ImGui::Begin(kHierarchyWindow, &core_open_[1])) {
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    namespace tk = ui::tokens;
    bool auto_open = false; // open_add_menu(): after the workspace settles
    if (hierarchy_open_add_ > 0) auto_open = --hierarchy_open_add_ == 0;
    if (kit::icon_text_button("Add", kit::Icon::Plus, true, true) || auto_open) ImGui::OpenPopup("##add");
    kit::tooltip("Add something to the scene: a shape, light, camera, sprite or terrain");
    if (ImGui::BeginPopup("##add")) {
        draw_create_entity_items(kNullEntity);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    kit::search_input("hierarchy_search", "Search", hierarchy_filter_);

    // Reveal the selection: when it changes (e.g. a click in the viewport), open every parent
    // and scroll it into view.
    const Uuid sel = selection_.empty() ? Uuid{} : selection_.back();
    hierarchy_reveal_.clear();
    if (sel != hierarchy_revealed_for_) {
        hierarchy_revealed_for_ = sel;
        if (const Entity se = s.find(sel); !se.is_null()) {
            for (Entity p = s.parent(se); !p.is_null(); p = s.parent(p)) hierarchy_reveal_.insert(s.uuid(p));
            hierarchy_scroll_ = true;
        }
    }
    hierarchy_keep_.clear();
    hierarchy_matches_ = 0;
    if (!hierarchy_filter_.empty()) {
        bool any = false;
        for (const Entity e : s.roots()) collect_hierarchy_matches(e, hierarchy_keep_, any);
    }
    hierarchy_rows_ = 0;
    ImGui::Dummy(ImVec2(0, tk::kSpace1));
    ImGui::BeginChild("##tree");
    const std::vector<Entity> roots = s.roots();
    if (roots.empty()) {
        if (kit::empty_state(kit::Icon::Scene, "This scene is empty",
                             "Add a shape, light or camera to start building. Drag files from Assets into the "
                             "viewport to place models and sprites.",
                             "Add a Cube"))
            create_entity(Primitive::Cube);
    } else if (!hierarchy_filter_.empty() && hierarchy_matches_ == 0) {
        ImGui::Dummy(ImVec2(0, tk::kSpace3));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kTextMuted, "Nothing named \"%s\" in this scene.", hierarchy_filter_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Clear search")) hierarchy_filter_.clear();
    }
    ImGui::PushStyleColor(ImGuiCol_Header, tk::kAccentSubtle);
    // Search results open their own copy of the tree, so clearing the search brings back
    // exactly what was open before.
    if (!hierarchy_filter_.empty()) ImGui::PushID("##search_results");
    for (const Entity e : roots) draw_hierarchy_node(e);
    if (!hierarchy_filter_.empty()) ImGui::PopID();
    ImGui::PopStyleColor();
    // Empty space: drop to unparent, click to deselect.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(40.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::IsItemClicked()) clear_selection();
    if (ImGui::BeginPopupContextItem("##tree_ctx")) {
        draw_create_entity_items(kNullEntity);
        ImGui::EndPopup();
    }
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
        changed |= ImGui::Checkbox("Unlit", &sp->unlit);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore 2D lights (only matters when the scene has a Light2D)");
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
        const std::string comp_header = kit::humanize(schema->name) + "###" + schema->name;
        if (removable(comp_header.c_str(), remove))
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

    draw_roles(e); // applied after the inspector frame, as its own undo step
    changed |= draw_add_component(e);
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
        ImGui::TextColored(ui::tokens::kDanger, "%s", props.error().message.c_str());
        ImGui::PopTextWrapPos();
        return changed;
    }
    if (!c.properties.is_object()) c.properties = json::Value::object();
    if (props->empty()) ImGui::TextDisabled("No public fields (declare e.g. `MyScript.speed = 5`).");
    for (const auto& info : *props) {
        ImGui::PushID(info.name.c_str());
        const std::string prop_label = kit::humanize(info.name) + "##" + info.name;
        const json::Value* over = c.properties.find(info.name);
        const json::Value& cur = over ? *over : info.default_value;
        bool edited = false;
        json::Value next;
        if (cur.is_number()) {
            float f = static_cast<float>(cur.as_number());
            if ((edited = ImGui::DragFloat(prop_label.c_str(), &f, 0.05f))) next = json::Value(static_cast<double>(f));
        } else if (cur.is_bool()) {
            bool b = cur.as_bool();
            if ((edited = ImGui::Checkbox(prop_label.c_str(), &b))) next = json::Value(b);
        } else if (cur.is_string()) {
            std::string str = cur.as_string();
            if ((edited = ImGui::InputText(prop_label.c_str(), &str))) next = json::Value(str);
        } else if (cur.is_array() && cur.size() == 3) {
            float v[3];
            for (std::size_t i = 0; i < 3; ++i) v[i] = static_cast<float>(cur[i].as_number());
            if ((edited = ImGui::DragFloat3(prop_label.c_str(), v, 0.05f))) {
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
    ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens::kInfo);
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
    if (!core_open_[2]) return;
    if (!ImGui::Begin(kInspectorWindow, &core_open_[2])) {
        ImGui::End();
        return;
    }
    prune_selection();
    const Entity e = selected();
    if (e.is_null()) {
        kit::empty_state(kit::Icon::Entity, "Nothing selected",
                         "Click something in the Viewport or the Hierarchy to see and change its settings here.");
        if (pending_dirty_) {
            record_edit("Edit", std::move(pending_before_));
            pending_dirty_ = false;
        }
    } else {
        const bool multi = selection_.size() > 1;
        if (multi) ImGui::TextColored(ui::tokens::kInfo, "%zu selected: changes apply to all", selection_.size());
        draw_prefab_section(e);
        if (!multi) draw_ragdoll_section(e);
        begin_inspector_frame(e);
        const json::Value before_frame = multi ? active_scene().save_entity(e) : json::Value();
        const bool changed = inspect_components(e);
        if (changed && multi) apply_multi_edit(e, before_frame);
        if (changed) active_scene().update_transforms();
        end_inspector_frame(e, changed);
        if (auto r = std::exchange(pending_role_, std::nullopt)) make_role(e, *r);
        draw_network_section(e);
    }
    if (play_state_ != PlayState::Editing) {
        ImGui::Spacing();
        ImGui::TextColored(ui::tokens::kWarning, "Play mode: edits are temporary.");
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
        kit::icon(kit::icon_for(r.kind), kit::tint_for(r.kind));
        ImGui::SameLine();
        ImGui::TextUnformatted(r.path.c_str());
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
    if (!core_open_[3]) return;
    if (!ImGui::Begin(kAssetsWindow, &core_open_[3])) {
        ImGui::End();
        return;
    }
    std::vector<assets::AssetRecord> all = db_ ? db_->records() : std::vector<assets::AssetRecord>{};
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::vector<assets::AssetRecord> recs;
    for (const auto& r : all)
        if (asset_visible(r)) recs.push_back(r);

    // ---- toolbar: search, type, view, reimport, help ----
    {
        namespace tk = ui::tokens;
        const char* import_label = "Import";
        const float import_w =
            ImGui::CalcTextSize(import_label).x + ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.x * 2 + tk::kSpace2;
        const float tools_w =
            ImGui::GetFrameHeight() * 4 + tk::kSpace2 * 5 + import_w + (asset_grid_ ? 110 + tk::kSpace2 : 0);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float type_w = std::clamp(avail * 0.22f, 110.0f, 170.0f);
        (void)kit::search_input("##asset_search", "Search assets", asset_search_,
                                std::max(120.0f, avail - type_w - tools_w - tk::kSpace4 * 2));
        ImGui::SameLine(0, tk::kSpace4);
        // Type filter: only kinds the project has, with how many of each.
        std::map<assets::AssetKind, std::size_t> kinds;
        for (const auto& r : all) ++kinds[r.kind];
        const std::string preview =
            asset_kind_filter_ ? std::string(asset_kind_label(*asset_kind_filter_)) : std::string("All types");
        ImGui::SetNextItemWidth(type_w);
        if (ImGui::BeginCombo("##asset_kind", preview.c_str())) {
            if (ImGui::Selectable("All types", !asset_kind_filter_)) asset_kind_filter_.reset();
            for (const auto& [k, n] : kinds) {
                const std::string item = std::format("{}  ({})##{}", asset_kind_label(k), n, static_cast<int>(k));
                if (ImGui::Selectable(item.c_str(), asset_kind_filter_ == k)) asset_kind_filter_ = k;
            }
            ImGui::EndCombo();
        }
        kit::tooltip("Show one type of asset");
        ImGui::SameLine();
        const float x = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - tools_w);
        ImGui::SetCursorPosX(x);
        if (kit::icon_text_button(import_label, kit::Icon::Plus, true)) open_import_dialog();
        kit::tooltip("Copy images, 3D models, sounds or scripts into the project. You can also drop files on this window.");
        ImGui::SameLine(0, tk::kSpace2);
        if (asset_grid_) {
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("##tile", &asset_tile_size_, 32.0f, 256.0f, "%.0f px");
            kit::tooltip("Thumbnail size");
            ImGui::SameLine(0, tk::kSpace2);
        }
        if (kit::icon_button("##list", kit::Icon::Layout, "List: names, types and import status", !asset_grid_))
            asset_grid_ = false;
        ImGui::SameLine(0, tk::kSpace2);
        if (kit::icon_button("##grid", kit::Icon::Texture, "Grid: large thumbnails", asset_grid_)) asset_grid_ = true;
        ImGui::SameLine(0, tk::kSpace2);
        if (kit::icon_button("##reimport", kit::Icon::Rotate, "Reimport every asset (after changing files outside VICTUS)"))
            (void)reimport_assets();
        ImGui::SameLine(0, tk::kSpace2);
        (void)kit::icon_button("##assets_help", kit::Icon::Help,
                               "To add assets, click Import or drop files on the Studio window. Files copied into "
                               "the project's assets folder show up too (click to copy its path).\n\n"
                               "Double-click an asset to use it: models and prefabs are added to the scene, "
                               "materials open in the editor, sounds play.");
        if (ImGui::IsItemClicked() && platform_) platform_->set_clipboard(project_->assets_dir().string());
    }
    ImGui::Separator();

    if (all.empty()) {
        (void)kit::empty_state(kit::Icon::Folder, "No assets yet",
                               std::format("Drop images, 3D models (.glb, .gltf), sounds or scripts on this window, or click "
                                           "Import. They are copied into {}.",
                                           project_->assets_dir().string()));
        ImGui::End();
        return;
    }
    // Folder list on the left when there is room for it.
    const std::vector<std::string> folders = asset_folders();
    const bool sidebar = !folders.empty() && ImGui::GetContentRegionAvail().x > 520;
    if (sidebar) {
        ImGui::BeginChild("##asset_folders", ImVec2(170, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        draw_asset_folders(folders);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("##asset_body");
    if (recs.empty()) {
        if (kit::empty_state(kit::Icon::Search, "Nothing matches",
                             std::format("{} asset{} hidden by the search, type or folder.", all.size(),
                                         all.size() == 1 ? " is" : "s are"),
                             "Show all assets")) {
            asset_search_.clear();
            asset_kind_filter_.reset();
            asset_folder_.clear();
        }
    }
    // Draws an asset's thumbnail aspect-fitted into a square cell (or its text icon).
    const auto draw_thumb = [this](const assets::AssetRecord& r, float cell) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell), ImGui::GetColorU32(ui::tokens::kSurfaceRaised), ui::tokens::kRadiusSmall);
        if (auto t = asset_thumbnail(r.uuid)) {
            float w = cell, h = cell;
            if (t->aspect > 1.0f) h = cell / t->aspect;
            else w = cell * t->aspect;
            const ImVec2 a(p0.x + (cell - w) * 0.5f, p0.y + (cell - h) * 0.5f);
            dl->AddImage(ImTextureRef(ui::UiContext::texture_id(t->texture)), a, ImVec2(a.x + w, a.y + h), ImVec2(t->uv.x, t->uv.y),
                         ImVec2(t->uv.z, t->uv.w));
        } else {
            // Vector type icon until (or instead of) a rendered thumbnail; dimmed while one is pending.
            const bool pending = ThumbnailCache::supports(r.kind) && ThumbnailCache::renders(r.kind);
            const float isz = std::round(cell * 0.5f);
            ImVec4 tint = kit::tint_for(r.kind);
            if (pending) tint = ui::with_alpha(tint, 0.45f);
            kit::draw_icon(dl, kit::icon_for(r.kind), ImVec2(p0.x + (cell - isz) * 0.5f, p0.y + (cell - isz) * 0.5f), isz,
                           ImGui::GetColorU32(tint));
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
    } else if (!recs.empty() &&
               ImGui::BeginTable("##assets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100);
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
            {
                const fs::Path path(r.path);
                const float text_y = ImGui::GetCursorPosY();
                ImGui::TextUnformatted(path.filename().string().c_str());
                if (const std::string folder = path.parent_path().generic_string(); !folder.empty()) {
                    ImGui::SameLine(0, ui::tokens::kSpace3);
                    ImGui::SetCursorPosY(text_y);
                    ImGui::TextColored(ui::tokens::kTextDisabled, "%s", folder.c_str());
                }
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ui::tokens::kTextMuted, "%s", asset_kind_label(r.kind));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (r.importer.empty()) ImGui::TextColored(ui::tokens::kTextMuted, "Ready");
            else {
                auto up = db_->is_up_to_date(r.uuid);
                if (up && *up) ImGui::TextColored(ui::tokens::kTextMuted, "Ready");
                else if (up) {
                    ImGui::TextColored(ui::tokens::kWarning, "Needs reimport");
                    kit::tooltip("The file changed since it was imported. Click the reimport button above.");
                } else {
                    ImGui::TextColored(ui::tokens::kDanger, "Import failed");
                    kit::tooltip("This file could not be imported. Check the Console and the Problems panel.");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild(); // ##asset_body
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
        {
            bool cutout = import_edit_.alpha_coverage_cutoff > 0.0f;
            ImGui::BeginDisabled(!import_edit_.mipmaps);
            if (ImGui::Checkbox("Preserve alpha coverage", &cutout)) import_edit_.alpha_coverage_cutoff = cutout ? 0.5f : 0.0f;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("For alpha-tested (cutout) foliage and fences: keeps them from fading away with distance");
            if (cutout) ImGui::SliderFloat("Alpha cutoff", &import_edit_.alpha_coverage_cutoff, 0.01f, 0.99f, "%.2f");
            ImGui::EndDisabled();
        }
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
    if (!input_error_.empty()) ImGui::TextColored(ui::tokens::kDanger, "%s", input_error_.c_str());
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
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd))
        step_ui_scale(1);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract))
        step_ui_scale(-1);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) step_ui_scale(0);
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) open_help();
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
