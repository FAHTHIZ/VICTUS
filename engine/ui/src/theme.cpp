// SPDX-License-Identifier: Apache-2.0
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"

namespace victus::ui {

void apply_victus_theme(ImGuiStyle& style) {
    using namespace tokens;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = kRadius;
    style.ChildRounding = kRadius;
    style.FrameRounding = kRadius;
    style.GrabRounding = kRadiusSmall;
    style.TabRounding = kRadius;
    style.PopupRounding = kRadius;
    style.ScrollbarRounding = kRadiusLarge;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.WindowPadding = ImVec2(kSpace3, kSpace3);
    style.FramePadding = ImVec2(kSpace3, kSpace2 + 1.0f);
    style.CellPadding = ImVec2(kSpace3 - 2.0f, kSpace2 - 1.0f);
    style.ItemSpacing = ImVec2(kSpace3, kSpace3 - 2.0f);
    style.ItemInnerSpacing = ImVec2(kSpace3 - 2.0f, kSpace2);
    style.IndentSpacing = kSpace5;
    style.ScrollbarSize = kSpace4 + 1.0f;
    style.GrabMinSize = kSpace4;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(kSpace3, kSpace1);
    style.DockingSeparatorSize = 2.0f;
    style.DockingNodeHasCloseButton = false; // each tab has its own x; one per node was redundant

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDisabled;
    c[ImGuiCol_WindowBg] = kSurfacePanel;
    c[ImGuiCol_ChildBg] = with_alpha(kSurfacePanel, 0.0f);
    c[ImGuiCol_PopupBg] = kSurfacePopup;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kSurfaceRaised;
    c[ImGuiCol_FrameBgHovered] = kSurfaceHover;
    c[ImGuiCol_FrameBgActive] = kSurfaceActive;
    c[ImGuiCol_TitleBg] = kSurfaceBase;
    c[ImGuiCol_TitleBgActive] = kSurfaceBase;
    c[ImGuiCol_TitleBgCollapsed] = kSurfaceBase;
    c[ImGuiCol_MenuBarBg] = kSurfaceBase;
    c[ImGuiCol_ScrollbarBg] = with_alpha(kSurfaceBase, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = kSurfaceHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kSurfaceActive;
    c[ImGuiCol_ScrollbarGrabActive] = kTextDisabled;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccentSoft;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = kSurfaceRaised;
    c[ImGuiCol_ButtonHovered] = kSurfaceHover;
    c[ImGuiCol_ButtonActive] = kSurfaceActive;
    // Neutral section headers; lists that show a selection push kAccentSubtle themselves.
    c[ImGuiCol_Header] = kSurfaceRaised;
    c[ImGuiCol_HeaderHovered] = kSurfaceHover;
    c[ImGuiCol_HeaderActive] = kAccentSoft;
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_SeparatorHovered] = kAccentSoft;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = with_alpha(kTextDisabled, 0.2f);
    c[ImGuiCol_ResizeGripHovered] = kAccentSoft;
    c[ImGuiCol_ResizeGripActive] = kAccent;
    c[ImGuiCol_InputTextCursor] = kText;
    c[ImGuiCol_Tab] = kSurfaceBase;
    c[ImGuiCol_TabHovered] = kSurfaceHover;
    c[ImGuiCol_TabSelected] = kSurfacePanel;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kSurfaceBase;
    c[ImGuiCol_TabDimmedSelected] = kSurfacePanel;
    c[ImGuiCol_TabDimmedSelectedOverline] = with_alpha(kAccent, 0.0f);
    c[ImGuiCol_DockingPreview] = kAccentSoft;
    c[ImGuiCol_DockingEmptyBg] = kSurfaceBase;
    c[ImGuiCol_PlotLines] = kInfo;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_TableHeaderBg] = kSurfaceRaised;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = with_alpha(kBorder, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextLink] = kInfo;
    c[ImGuiCol_TextSelectedBg] = with_alpha(kAccent, 0.35f);
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

} // namespace victus::ui
