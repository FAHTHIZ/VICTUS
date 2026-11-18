// SPDX-License-Identifier: Apache-2.0
// Studio workspace: the Window menu (every panel can be reopened from it), layout presets and
// the docking layouts behind them.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <initializer_list>

namespace victus::editor {

namespace {

// ImGui window names. Titles that change at run time use a "###" id so docking still finds them.
const char* window_id(Panel p) noexcept {
    switch (p) {
    case Panel::Viewport:
        return "Viewport";
    case Panel::Hierarchy:
        return "Hierarchy";
    case Panel::Inspector:
        return "Inspector";
    case Panel::Assets:
        return "Assets";
    case Panel::Console:
        return "Console";
    case Panel::Animation:
        return "Animation";
    case Panel::Sequencer:
        return "Sequencer";
    case Panel::GraphEditor:
        return "###GraphEditor";
    case Panel::MaterialEditor:
        return "Material Editor";
    case Panel::TilePalette:
        return "Tile Palette";
    case Panel::TerrainTools:
        return "Terrain Tools";
    case Panel::AiDebugger:
        return "AI Debugger";
    case Panel::Network:
        return "Network";
    case Panel::Profiler:
        return "Profiler";
    case Panel::InputActions:
        return "Input Actions";
    case Panel::Count:
        break;
    }
    return "";
}

const char* panel_hint(Panel p) noexcept {
    switch (p) {
    case Panel::Viewport:
        return "The scene you are editing; also where Play runs";
    case Panel::Hierarchy:
        return "Every object in the scene, as a tree";
    case Panel::Inspector:
        return "Settings of the selected object";
    case Panel::Assets:
        return "Models, images, sounds, scripts and scenes in this project";
    case Panel::Console:
        return "Messages, warnings and errors";
    case Panel::Animation:
        return "Clips and state machine of the selected animated object";
    case Panel::Sequencer:
        return "Timeline for cutscenes and scripted moments";
    case Panel::GraphEditor:
        return "Visual scripts and material graphs, without code";
    case Panel::MaterialEditor:
        return "Colours and surface of the selected object's material";
    case Panel::TilePalette:
        return "Paint tiles on the selected tilemap";
    case Panel::TerrainTools:
        return "Sculpt and paint the selected terrain";
    case Panel::AiDebugger:
        return "What each AI character is thinking while playing";
    case Panel::Network:
        return "Multiplayer host and client settings";
    case Panel::Profiler:
        return "Frame time, slowest systems and memory";
    case Panel::InputActions:
        return "Keys and buttons mapped to game actions";
    case Panel::Count:
        break;
    }
    return "";
}

const char* layout_hint(Layout l) noexcept {
    switch (l) {
    case Layout::Default:
        return "Scene, objects, settings and assets - the usual starting point";
    case Layout::LevelDesign:
        return "A large viewport for building levels";
    case Layout::Animation:
        return "Animation and Sequencer under the viewport";
    case Layout::Scripting:
        return "Graph Editor in the middle, Console below";
    case Layout::Debugging:
        return "Profiler, Console, Network and AI Debugger together";
    case Layout::Count:
        break;
    }
    return "";
}

constexpr int kCoreCount = 5;

} // namespace

const char* panel_title(Panel p) noexcept {
    switch (p) {
    case Panel::GraphEditor:
        return "Graph Editor";
    default:
        return window_id(p);
    }
}

const char* layout_name(Layout l) noexcept {
    switch (l) {
    case Layout::Default:
        return "Default";
    case Layout::LevelDesign:
        return "Level Design";
    case Layout::Animation:
        return "Animation";
    case Layout::Scripting:
        return "Scripting";
    case Layout::Debugging:
        return "Debugging";
    case Layout::Count:
        break;
    }
    return "";
}

bool* Editor::panel_flag(Panel p) noexcept {
    const int i = static_cast<int>(p);
    if (i < kCoreCount) return &core_open_[i];
    switch (p) {
    case Panel::Animation:
        return &show_animation_;
    case Panel::Sequencer:
        return &show_sequencer_;
    case Panel::GraphEditor:
        return &show_graph_editor_;
    case Panel::MaterialEditor:
        return &show_material_editor_;
    case Panel::TilePalette:
        return &show_tile_palette_;
    case Panel::TerrainTools:
        return &show_terrain_tools_;
    case Panel::AiDebugger:
        return &show_ai_;
    case Panel::Network:
        return &show_network_;
    case Panel::Profiler:
        return &show_profiler_;
    case Panel::InputActions:
        return &show_input_actions_;
    default:
        return nullptr;
    }
}

bool Editor::panel_open(Panel p) const noexcept {
    if (p == Panel::Viewport) return true; // the viewport cannot be closed
    const bool* flag = const_cast<Editor*>(this)->panel_flag(p);
    return flag && *flag;
}

void Editor::set_panel_open(Panel p, bool open) noexcept {
    if (p == Panel::Viewport) return;
    if (p == Panel::Profiler) {
        set_show_profiler(open);
        return;
    }
    if (bool* flag = panel_flag(p)) *flag = open;
}

void Editor::show_panel(Panel p) noexcept {
    set_panel_open(p, true);
    focus_panel_ = static_cast<int>(p);
}

void Editor::apply_layout(Layout l) noexcept {
    layout_ = l;
    layout_rebuild_ = true;
    focus_panel_ = -1;
    for (bool& b : core_open_)
        b = true;
    for (int i = kCoreCount; i < static_cast<int>(Panel::Count); ++i)
        set_panel_open(static_cast<Panel>(i), false);
    switch (l) {
    case Layout::Default:
    case Layout::LevelDesign:
        set_panel_open(Panel::TerrainTools, true);
        break; // shows only with a terrain selected
    case Layout::Animation:
        set_panel_open(Panel::Animation, true);
        set_panel_open(Panel::Sequencer, true);
        break;
    case Layout::Scripting:
        set_panel_open(Panel::GraphEditor, true);
        break;
    case Layout::Debugging:
        set_panel_open(Panel::Profiler, true);
        set_panel_open(Panel::Network, true);
        set_panel_open(Panel::AiDebugger, true);
        break;
    case Layout::Count:
        break;
    }
}

void Editor::build_layout(unsigned int dockspace_id, Layout l) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace_id;
    const auto dock = [](Panel p, ImGuiID node) { ImGui::DockBuilderDockWindow(window_id(p), node); };
    const auto dock_all = [&](std::initializer_list<Panel> panels, ImGuiID node) {
        for (Panel p : panels)
            dock(p, node);
    };
    switch (l) {
    case Layout::Default:
    case Layout::Count: {
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        dock(Panel::Hierarchy, left);
        dock_all({Panel::Inspector, Panel::TerrainTools, Panel::TilePalette, Panel::MaterialEditor, Panel::AiDebugger,
                  Panel::InputActions},
                 right);
        dock_all({Panel::Assets, Panel::Console, Panel::Animation, Panel::Sequencer, Panel::Profiler, Panel::Network},
                 bottom);
        dock_all({Panel::Viewport, Panel::GraphEditor}, center);
        break;
    }
    case Layout::LevelDesign: {
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.16f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
        ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
        dock(Panel::Hierarchy, left);
        dock_all({Panel::Assets, Panel::Console}, left_bottom);
        dock_all(
            {Panel::Inspector, Panel::TerrainTools, Panel::TilePalette, Panel::MaterialEditor, Panel::InputActions},
            right);
        dock_all({Panel::Viewport, Panel::GraphEditor}, center);
        dock_all({Panel::Animation, Panel::Sequencer, Panel::Profiler, Panel::Network, Panel::AiDebugger}, left_bottom);
        break;
    }
    case Layout::Animation: {
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.40f, nullptr, &center);
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
        ImGuiID bottom_right = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, nullptr, &bottom);
        dock(Panel::Hierarchy, left);
        dock_all(
            {Panel::Inspector, Panel::MaterialEditor, Panel::TerrainTools, Panel::TilePalette, Panel::InputActions},
            right);
        dock_all({Panel::Animation, Panel::Assets, Panel::Console}, bottom);
        dock_all({Panel::Sequencer, Panel::Profiler, Panel::Network, Panel::AiDebugger}, bottom_right);
        dock_all({Panel::Viewport, Panel::GraphEditor}, center);
        break;
    }
    case Layout::Scripting: {
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, nullptr, &left);
        dock(Panel::Hierarchy, left);
        dock(Panel::Assets, left_bottom);
        dock_all(
            {Panel::Inspector, Panel::MaterialEditor, Panel::InputActions, Panel::TerrainTools, Panel::TilePalette},
            right);
        dock_all(
            {Panel::Console, Panel::Profiler, Panel::Animation, Panel::Sequencer, Panel::Network, Panel::AiDebugger},
            bottom);
        dock_all({Panel::GraphEditor, Panel::Viewport}, center);
        break;
    }
    case Layout::Debugging: {
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.36f, nullptr, &center);
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
        ImGuiID bottom_right = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.5f, nullptr, &bottom);
        dock(Panel::Hierarchy, left);
        dock_all({Panel::Inspector, Panel::AiDebugger, Panel::Network, Panel::MaterialEditor, Panel::InputActions,
                  Panel::TerrainTools, Panel::TilePalette},
                 right);
        dock_all({Panel::Console, Panel::Assets, Panel::Animation, Panel::Sequencer}, bottom);
        dock(Panel::Profiler, bottom_right);
        dock_all({Panel::Viewport, Panel::GraphEditor}, center);
        break;
    }
    }
    ImGui::DockBuilderFinish(dockspace_id);
}

void Editor::draw_window_menu() {
    if (!ImGui::BeginMenu("Window")) return;
    const auto item = [this](Panel p, const char* shortcut = nullptr) {
        const bool open = panel_open(p);
        // Picking a panel always shows it (opens it if closed, else brings it to the front);
        // panels close with the x on their tab.
        if (ImGui::MenuItem(panel_title(p), shortcut, open)) show_panel(p);
        kit::tooltip(panel_hint(p));
    };
    ImGui::TextDisabled("Panels");
    for (Panel p : {Panel::Viewport, Panel::Hierarchy, Panel::Inspector, Panel::Assets, Panel::Console})
        item(p);
    ImGui::Separator();
    ImGui::TextDisabled("Tools");
    for (Panel p : {Panel::Animation, Panel::Sequencer, Panel::GraphEditor, Panel::MaterialEditor, Panel::TilePalette,
                    Panel::TerrainTools, Panel::InputActions})
        item(p);
    ImGui::Separator();
    ImGui::TextDisabled("Debug");
    for (Panel p : {Panel::Profiler, Panel::Network, Panel::AiDebugger})
        item(p);
    ImGui::Separator();
    if (ImGui::BeginMenu("Layout")) {
        for (int i = 0; i < static_cast<int>(Layout::Count); ++i) {
            const auto l = static_cast<Layout>(i);
            if (ImGui::MenuItem(layout_name(l), nullptr, layout_ == l)) apply_layout(l);
            kit::tooltip(layout_hint(l));
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Layout")) apply_layout(layout_);
    kit::tooltip("Put every panel of the current layout back in its place");
    ImGui::EndMenu();
}

} // namespace victus::editor
