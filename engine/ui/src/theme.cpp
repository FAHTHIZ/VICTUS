// SPDX-License-Identifier: Apache-2.0
#include "victus/ui/ui.hpp"

namespace victus::ui {

void apply_victus_theme(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 5);
    style.IndentSpacing = 14.0f;

    ImVec4* c = style.Colors;
    const ImVec4 bg{0.105f, 0.11f, 0.125f, 1.0f};
    const ImVec4 panel{0.135f, 0.14f, 0.16f, 1.0f};
    const ImVec4 frame{0.18f, 0.19f, 0.215f, 1.0f};
    const ImVec4 accent{0.95f, 0.45f, 0.16f, 1.0f}; // VICTUS orange
    const ImVec4 accent_soft{0.95f, 0.45f, 0.16f, 0.55f};
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.52f, 0.56f, 1.0f);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.145f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.25f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.24f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.27f, 0.31f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent_soft;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.27f, 0.31f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent_soft;
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.23f, 0.26f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.28f, 0.32f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent_soft;
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = ImVec4(0.27f, 0.28f, 0.32f, 1.0f);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_DockingPreview] = accent_soft;
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.95f, 0.45f, 0.16f, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
}

} // namespace victus::ui
