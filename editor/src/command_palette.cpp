// SPDX-License-Identifier: Apache-2.0
// Command palette (Ctrl+Shift+P or F1): fuzzy search over menu actions, window toggles, scenes,
// assets and entities of the edited scene.
#include "victus/editor/editor.hpp"

#include "victus/assets/database.hpp"
#include "victus/project/project.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>

namespace victus::editor {

using namespace scene;

namespace {

char lower(char c) noexcept { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool word_start(std::string_view text, std::size_t i) noexcept {
    if (i == 0) return true;
    const char p = text[i - 1];
    const char c = text[i];
    return p == ' ' || p == ':' || p == '/' || p == '_' || p == '.' || p == '-' ||
           (std::islower(static_cast<unsigned char>(p)) && std::isupper(static_cast<unsigned char>(c)));
}

} // namespace

int fuzzy_match_score(std::string_view query, std::string_view text) noexcept {
    // Skip spaces in the query: "tog prof" matches "View: Toggle Profiler".
    int score = 0;
    std::size_t ti = 0;
    bool prev_match = false;
    for (char q : query) {
        if (q == ' ') {
            prev_match = false;
            continue;
        }
        const char lq = lower(q);
        bool found = false;
        while (ti < text.size()) {
            const bool hit = lower(text[ti]) == lq;
            if (hit) {
                score += 1;
                if (prev_match) score += 5;           // consecutive run
                if (word_start(text, ti)) score += 8; // start of a word
                if (text[ti] == q) score += 1;        // exact case
                prev_match = true;
                found = true;
                ++ti;
                break;
            }
            prev_match = false;
            ++ti;
        }
        if (!found) return -1;
    }
    // Shorter texts win ties; contiguous substring matches win outright.
    std::string lt(text);
    std::string lqs(query);
    std::transform(lt.begin(), lt.end(), lt.begin(), lower);
    std::transform(lqs.begin(), lqs.end(), lqs.begin(), lower);
    if (!lqs.empty() && lt.find(lqs) != std::string::npos) score += 20;
    return score * 4 - static_cast<int>(std::min<std::size_t>(text.size(), 200)) / 8;
}

std::vector<EditorCommand> Editor::commands() {
    std::vector<EditorCommand> out;
    const bool editing = play_state_ == PlayState::Editing;
    const bool has_db = db_ != nullptr;
    const auto add = [&](std::string name, std::string shortcut, bool enabled, std::function<void()> fn) {
        out.push_back({std::move(name), std::move(shortcut), enabled, std::move(fn)});
    };
    const auto toggle = [&](std::string label, bool* flag) {
        add("View: Toggle " + label, {}, true, [flag] { *flag = !*flag; });
    };

    // File
    add("File: New Scene", {}, editing && project_.has_value(), [this] { new_scene(); });
    add("File: Save Scene", "Ctrl+S", editing && project_.has_value(), [this] {
        if (scene_path_.empty())
            show_save_as_ = true;
        else if (auto st = save_scene(); !st.ok())
            notify(st.error().message, true);
    });
    add("File: Save Scene As...", {}, editing && project_.has_value(), [this] { show_save_as_ = true; });
    add("File: Export Game...", {}, editing && project_.has_value() && !exporting(), [this] { open_export_dialog(); });
    add("Assets: Bake Lighting", {}, editing && project_.has_value() && !baking_lighting(), [this] {
        if (auto st = start_lighting_bake(); !st.ok()) notify("Bake Lighting: " + st.error().message, true);
    });
    add("Help: Report a Problem (Diagnostics Bundle)...", {}, !creating_bundle(), [this] { open_diagnostics_dialog(); });
    add("Help: About VICTUS Studio", {}, true, [this] { open_about_dialog(); });
    add("File: Close Project", {}, editing && project_.has_value(), [this] { close_project(); });
    if (project_ && editing)
        for (const std::string& s : project_->scenes())
            add("Open Scene: " + s, {}, s != scene_path_, [this, s] {
                if (auto st = open_scene(s); !st.ok()) notify(st.error().message, true);
            });

    // Edit
    add("Edit: Undo", "Ctrl+Z", undo_.can_undo(), [this] { undo(); });
    add("Edit: Redo", "Ctrl+Y", undo_.can_redo(), [this] { redo(); });
    add("Edit: Duplicate Selected", "Ctrl+D", !selection_.empty(), [this] { duplicate_selected(); });
    add("Edit: Delete Selected", "Delete", !selection_.empty(), [this] { delete_selected(); });
    add("Edit: Frame Selected", "F", !selection_.empty(), [this] { frame_selected(); });
    add("Edit: Clear Selection", {}, !selection_.empty(), [this] { clear_selection(); });
    add("Edit: Input Actions...", {}, project_.has_value(), [this] { show_input_actions_ = true; });

    // Entity
    const std::pair<const char*, Primitive> prims[] = {{"Empty", Primitive::Empty},
                                                       {"Cube", Primitive::Cube},
                                                       {"Sphere", Primitive::Sphere},
                                                       {"Plane", Primitive::Plane},
                                                       {"Cylinder", Primitive::Cylinder},
                                                       {"Sprite", Primitive::Sprite},
                                                       {"Camera", Primitive::Camera},
                                                       {"Directional Light", Primitive::DirectionalLight},
                                                       {"Point Light", Primitive::PointLight},
                                                       {"Spot Light", Primitive::SpotLight},
                                                       {"Decal", Primitive::Decal},
                                                       {"2D Light", Primitive::Light2D},
                                                       {"Fog Volume", Primitive::FogVolume}};
    for (const auto& [label, p] : prims)
        add(std::string("Entity: Create ") + label, {}, project_.has_value(),
            [this, kind = p] { (void)create_entity(kind); });

    add("Entity: Create Terrain", {}, has_db, [this] {
        if (auto t = create_terrain(); !t) notify("cannot create terrain: " + t.error().message, true);
    });

    add("Entity: Add Orion (character)", {}, has_db && editing, [this] { (void)add_character("orion", {}, camera_.target); });
    add("Entity: Add Nova (character)", {}, has_db && editing, [this] { (void)add_character("nova", {}, camera_.target); });
    add("Entity: Add Ragdoll Lab", {}, has_db && editing, [this] { (void)add_ragdoll_lab(); });
    add("Entity: Make Ragdoll", {}, !selection_.empty() && can_make_ragdoll(selected()),
        [this] { (void)make_ragdoll(selected()); });
    add("Entity: Add Foot Placement", {}, !selection_.empty() && can_make_ragdoll(selected()),
        [this] { (void)add_foot_placement(selected()); });

    // Help
    add("Help: Getting Started", {}, project_.has_value(), [this] { show_panel(Panel::GettingStarted); });
    add("Help: Search Help", "F2", true, [this] { open_help(); });
    add("Help: Keyboard Shortcuts", {}, true, [this] { open_help("Keyboard shortcuts"); });
    add("View: Bigger Interface", "Ctrl+=", true, [this] { step_ui_scale(1); });
    add("View: Smaller Interface", "Ctrl+-", true, [this] { step_ui_scale(-1); });
    add("View: Normal Interface Size", "Ctrl+0", true, [this] { step_ui_scale(0); });
    add("Edit: Undo History", {}, project_.has_value(), [this] { show_panel(Panel::History); });
    add("Edit: Project Settings", {}, project_.has_value(), [this] { show_panel(Panel::ProjectSettings); });

    // Assets
    add("Assets: Reimport All", {}, has_db, [this] { (void)reimport_assets(); });
    add("Assets: New Material", {}, has_db, [this] { new_material_for_selection(); });
    add("Assets: New Visual Script", {}, has_db, [this] {
        if (auto id = create_graph(); !id)
            notify("cannot create visual script: " + id.error().message, true);
        else if (auto st = open_graph_editor(*id); !st.ok())
            notify(st.error().message, true);
    });
    add("Assets: New Material Graph", {}, has_db, [this] {
        if (auto id = create_material_graph(); !id)
            notify("cannot create material graph: " + id.error().message, true);
        else if (auto st = open_graph_editor(*id); !st.ok())
            notify(st.error().message, true);
    });
    add("Assets: New Surface Shader", {}, has_db, [this] {
        if (auto id = create_surface_shader(); !id) notify("cannot create shader: " + id.error().message, true);
    });
    if (db_) {
        for (const auto& r : db_->records()) {
            if (r.kind == assets::AssetKind::Script && r.path.ends_with(".vgraph"))
                add("Open Visual Script: " + r.path, {}, true, [this, id = r.uuid] {
                    if (auto st = open_graph_editor(id); !st.ok()) notify(st.error().message, true);
                });
            else if (r.kind == assets::AssetKind::Shader && r.path.ends_with(".vmatgraph"))
                add("Open Material Graph: " + r.path, {}, true, [this, id = r.uuid] {
                    if (auto st = open_graph_editor(id); !st.ok()) notify(st.error().message, true);
                });
            else if (r.kind == assets::AssetKind::Material)
                add("Open Material: " + r.path, {}, true, [this, id = r.uuid] { open_material_editor(id); });
            else if (r.kind == assets::AssetKind::Prefab)
                add("Instantiate Prefab: " + r.path, {}, true, [this, id = r.uuid] { (void)instantiate_prefab(id); });
            else if (r.kind == assets::AssetKind::Model)
                add("Instantiate Model: " + r.path, {}, true, [this, id = r.uuid] { (void)instantiate_asset(id); });
        }
    }

    // View
    toggle("Physics Colliders", &show_colliders_);
    toggle("Navigation Mesh", &show_navmesh_);
    toggle("IK and Foot Placement", &show_ik_);
    add("View: Toggle Profiler", {}, true, [this] { set_show_profiler(!show_profiler_); });
    // Window
    for (int i = 0; i < static_cast<int>(Panel::Count); ++i) {
        const auto p = static_cast<Panel>(i);
        add(std::string("Window: Show ") + panel_title(p), {}, true, [this, p] { show_panel(p); });
    }
    for (int i = 0; i < static_cast<int>(Layout::Count); ++i) {
        const auto l = static_cast<Layout>(i);
        add(std::string("Window: Layout ") + layout_name(l), {}, true, [this, l] { apply_layout(l); });
    }
    add("Window: Reset Layout", {}, true, [this] { apply_layout(layout_); });
    for (auto tier :
         {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High, render::QualityTier::Ultra})
        add("View: Rendering Quality " + std::string(render::quality_tier_name(tier)), {}, quality_ != tier,
            [this, tier] {
                if (auto st = set_quality(tier); !st.ok())
                    notify("could not save quality: " + st.error().message, true);
            });

    add("View: Toggle Overdraw View", {}, true, [this] {
        set_debug_view(debug_view_ == render::DebugView::Overdraw ? render::DebugView::None
                                                                  : render::DebugView::Overdraw);
    });

    // Play
    add("Play: Play", "Ctrl+P", project_.has_value() && play_state_ != PlayState::Playing, [this] { play(); });
    add("Play: Pause", {}, play_state_ == PlayState::Playing, [this] { pause(); });
    add("Play: Stop", "Ctrl+P", !editing, [this] { stop(); });
    add("Play: Play as Host", {}, editing && project_.has_value(), [this] { play_host(); });

    // Entities of the scene shown in the panels: select and frame.
    if (project_) {
        Scene& s = active_scene();
        std::size_t n = 0;
        s.world().each<Name>([&](Entity e, const Name& name) {
            if (n++ >= 5000 || name.value.empty()) return;
            const Uuid id = s.uuid(e);
            add("Select: " + name.value, {}, true, [this, id] {
                const Entity found = active_scene().find(id);
                if (found.is_null()) return;
                select(found);
                frame_selected();
            });
        });
    }
    return out;
}

std::vector<EditorCommand> Editor::find_commands(std::string_view query, std::size_t limit) {
    std::vector<EditorCommand> all = commands();
    std::vector<std::pair<int, std::size_t>> scored;
    const bool empty = std::all_of(query.begin(), query.end(), [](char c) { return c == ' '; });
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!all[i].enabled) continue;
        if (empty) {
            // Without a query list actions, not every asset and entity.
            if (all[i].name.starts_with("Select: ") || all[i].name.starts_with("Open ") ||
                all[i].name.starts_with("Instantiate "))
                continue;
            scored.emplace_back(0, i);
            continue;
        }
        const int sc = fuzzy_match_score(query, all[i].name);
        if (sc >= 0) scored.emplace_back(sc, i);
    }
    std::stable_sort(scored.begin(), scored.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return all[a.second].name < all[b.second].name;
    });
    std::vector<EditorCommand> out;
    for (std::size_t k = 0; k < scored.size() && out.size() < limit; ++k)
        out.push_back(std::move(all[scored[k].second]));
    return out;
}

bool Editor::run_command(std::string_view name) {
    for (auto& c : commands()) {
        if (!c.enabled || c.name.size() != name.size()) continue;
        if (!std::equal(c.name.begin(), c.name.end(), name.begin(),
                        [](char a, char b) { return lower(a) == lower(b); }))
            continue;
        if (c.run) c.run();
        return true;
    }
    return false;
}

void Editor::draw_command_palette() {
    if (!palette_open_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(640.0f, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + 40),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    if (palette_focus_) ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##CommandPalette", nullptr, flags)) {
        ImGui::End();
        return;
    }
    if (palette_focus_) {
        ImGui::SetKeyboardFocusHere();
        palette_focus_ = false;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##query", "Type a command, scene, asset or entity name...", &palette_query_))
        palette_index_ = 0;
    auto results = find_commands(palette_query_, 14);
    const int count = static_cast<int>(results.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) palette_index_ = count ? (palette_index_ + 1) % count : 0;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) palette_index_ = count ? (palette_index_ + count - 1) % count : 0;
    palette_index_ = std::clamp(palette_index_, 0, std::max(count - 1, 0));
    int run = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) run = palette_index_;
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::Separator();
    if (results.empty()) ImGui::TextDisabled("No matching commands");
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        const auto& c = results[static_cast<std::size_t>(i)];
        if (ImGui::Selectable(c.name.c_str(), i == palette_index_, ImGuiSelectableFlags_AllowOverlap)) run = i;
        if (!c.shortcut.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(c.shortcut.c_str()).x);
            ImGui::TextDisabled("%s", c.shortcut.c_str());
        }
        ImGui::PopID();
    }
    // The palette keeps keyboard focus while open (it is modal for the keyboard); clicking
    // outside closes it.
    const bool hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (!hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        close = true;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) palette_focus_ = true;
    ImGui::End();
    if (run >= 0 && run < count) {
        palette_open_ = false;
        if (results[static_cast<std::size_t>(run)].run) results[static_cast<std::size_t>(run)].run();
    } else if (close) {
        palette_open_ = false;
    }
}

} // namespace victus::editor
