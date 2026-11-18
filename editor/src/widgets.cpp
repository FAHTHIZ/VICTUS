// SPDX-License-Identifier: Apache-2.0
#include "widgets.hpp"

#include "victus/core/log.hpp"
#include "victus/runtime/builtin_fonts.hpp"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <span>
#include <string>

namespace victus::editor::kit {

namespace tk = ui::tokens;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Maps unit-square coordinates to the icon rect.
struct Pen {
    ImDrawList* dl;
    ImVec2 o;
    float s;
    ImU32 col;
    [[nodiscard]] ImVec2 p(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    [[nodiscard]] float w(float t = 0.085f) const { return std::max(1.0f, t * s); }
    void line(float x0, float y0, float x1, float y1, float t = 0.085f) const {
        dl->AddLine(p(x0, y0), p(x1, y1), col, w(t));
    }
    void rect(float x0, float y0, float x1, float y1, float r = 0.06f, float t = 0.085f) const {
        dl->AddRect(p(x0, y0), p(x1, y1), col, r * s, w(t));
    }
    void fill(float x0, float y0, float x1, float y1, float r = 0.0f) const {
        dl->AddRectFilled(p(x0, y0), p(x1, y1), col, r * s);
    }
    void circle(float x, float y, float r, float t = 0.085f) const { dl->AddCircle(p(x, y), r * s, col, 0, w(t)); }
    void disc(float x, float y, float r) const { dl->AddCircleFilled(p(x, y), r * s, col); }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->AddTriangleFilled(p(x0, y0), p(x1, y1), p(x2, y2), col);
    }
    void poly(std::span<const ImVec2> unit, bool closed, float t = 0.085f) const {
        ImVec2 pts[16];
        const int n = static_cast<int>(std::min<std::size_t>(unit.size(), 16));
        for (int i = 0; i < n; ++i)
            pts[i] = p(unit[static_cast<std::size_t>(i)].x, unit[static_cast<std::size_t>(i)].y);
        dl->AddPolyline(pts, n, col, w(t), closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    }
    void arc(float x, float y, float r, float a0, float a1, float t = 0.085f) const {
        dl->PathArcTo(p(x, y), r * s, a0, a1, 0);
        dl->PathStroke(col, w(t));
    }
    void text(const char* str, float scale = 0.8f) const {
        ImFont* f = bold_font() ? bold_font() : ImGui::GetFont();
        const float size = s * scale;
        const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, str);
        dl->AddText(f, size, ImVec2(o.x + (s - ts.x) * 0.5f, o.y + (s - ts.y) * 0.5f), col, str);
    }
};

void cube(const Pen& pen) {
    const ImVec2 outline[] = {{0.5f, 0.1f}, {0.87f, 0.3f}, {0.87f, 0.7f}, {0.5f, 0.9f}, {0.13f, 0.7f}, {0.13f, 0.3f}};
    pen.poly(outline, true);
    pen.line(0.13f, 0.3f, 0.5f, 0.5f);
    pen.line(0.87f, 0.3f, 0.5f, 0.5f);
    pen.line(0.5f, 0.5f, 0.5f, 0.9f);
}

void picture(const Pen& pen) {
    pen.rect(0.12f, 0.18f, 0.88f, 0.82f);
    const ImVec2 hills[] = {{0.16f, 0.76f}, {0.4f, 0.5f}, {0.56f, 0.66f}, {0.68f, 0.56f}, {0.84f, 0.76f}};
    pen.poly(hills, false);
    pen.disc(0.68f, 0.36f, 0.07f);
}

void arrow_head(const Pen& pen, float x, float y, float dx, float dy, float len = 0.16f) {
    // Filled triangle pointing along (dx, dy) with its tip at (x, y).
    const float nx = -dy, ny = dx;
    pen.tri(x, y, x - dx * len + nx * len * 0.7f, y - dy * len + ny * len * 0.7f, x - dx * len - nx * len * 0.7f,
            y - dy * len - ny * len * 0.7f);
}

} // namespace

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 col) {
    const Pen pen{dl, min, size, col};
    switch (icon) {
    case Icon::Play:
        pen.tri(0.28f, 0.16f, 0.28f, 0.84f, 0.84f, 0.5f);
        break;
    case Icon::Pause:
        pen.fill(0.24f, 0.18f, 0.42f, 0.82f, 0.04f);
        pen.fill(0.58f, 0.18f, 0.76f, 0.82f, 0.04f);
        break;
    case Icon::Stop:
        pen.fill(0.22f, 0.22f, 0.78f, 0.78f, 0.08f);
        break;
    case Icon::Record:
        pen.disc(0.5f, 0.5f, 0.3f);
        break;
    case Icon::Step:
        pen.tri(0.2f, 0.18f, 0.2f, 0.82f, 0.66f, 0.5f);
        pen.fill(0.7f, 0.18f, 0.82f, 0.82f, 0.03f);
        break;
    case Icon::Save:
        pen.rect(0.16f, 0.16f, 0.84f, 0.84f, 0.08f);
        pen.fill(0.32f, 0.16f, 0.68f, 0.38f);
        pen.rect(0.3f, 0.56f, 0.7f, 0.84f, 0.02f);
        break;
    case Icon::Undo:
        pen.arc(0.52f, 0.56f, 0.28f, -kPi * 0.95f, kPi * 0.45f);
        arrow_head(pen, 0.22f, 0.5f, -0.1f, -1.0f, 0.2f);
        break;
    case Icon::Redo:
        pen.arc(0.48f, 0.56f, 0.28f, kPi * 0.55f, kPi * 1.95f);
        arrow_head(pen, 0.78f, 0.5f, 0.1f, -1.0f, 0.2f);
        break;
    case Icon::Search:
        pen.circle(0.44f, 0.44f, 0.24f, 0.1f);
        pen.line(0.62f, 0.62f, 0.84f, 0.84f, 0.12f);
        break;
    case Icon::Plus:
        pen.line(0.5f, 0.18f, 0.5f, 0.82f, 0.11f);
        pen.line(0.18f, 0.5f, 0.82f, 0.5f, 0.11f);
        break;
    case Icon::Close:
        pen.line(0.24f, 0.24f, 0.76f, 0.76f, 0.11f);
        pen.line(0.76f, 0.24f, 0.24f, 0.76f, 0.11f);
        break;
    case Icon::Check: {
        const ImVec2 c[] = {{0.18f, 0.52f}, {0.42f, 0.76f}, {0.84f, 0.28f}};
        pen.poly(c, false, 0.12f);
        break;
    }
    case Icon::Gear:
        pen.circle(0.5f, 0.5f, 0.2f, 0.1f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            pen.line(0.5f + std::cos(a) * 0.26f, 0.5f + std::sin(a) * 0.26f, 0.5f + std::cos(a) * 0.38f,
                     0.5f + std::sin(a) * 0.38f, 0.12f);
        }
        break;
    case Icon::Help:
        pen.circle(0.5f, 0.5f, 0.38f);
        pen.text("?", 0.62f);
        break;
    case Icon::Export:
        pen.rect(0.16f, 0.4f, 0.84f, 0.86f, 0.06f);
        pen.line(0.5f, 0.12f, 0.5f, 0.62f);
        arrow_head(pen, 0.5f, 0.1f, 0.0f, -1.0f, 0.2f);
        break;
    case Icon::Move:
        pen.line(0.5f, 0.16f, 0.5f, 0.84f);
        pen.line(0.16f, 0.5f, 0.84f, 0.5f);
        arrow_head(pen, 0.5f, 0.1f, 0, -1);
        arrow_head(pen, 0.5f, 0.9f, 0, 1);
        arrow_head(pen, 0.1f, 0.5f, -1, 0);
        arrow_head(pen, 0.9f, 0.5f, 1, 0);
        break;
    case Icon::Rotate:
        pen.arc(0.5f, 0.5f, 0.32f, -kPi * 0.35f, kPi * 1.35f);
        arrow_head(pen, 0.78f, 0.24f, 0.55f, -0.8f, 0.2f);
        break;
    case Icon::Scale:
        pen.rect(0.14f, 0.46f, 0.54f, 0.86f, 0.04f);
        pen.line(0.46f, 0.54f, 0.82f, 0.18f);
        arrow_head(pen, 0.86f, 0.14f, 0.707f, -0.707f, 0.22f);
        break;
    case Icon::Globe:
        pen.circle(0.5f, 0.5f, 0.36f);
        dl->AddEllipse(pen.p(0.5f, 0.5f), ImVec2(0.15f * size, 0.36f * size), col, 0.0f, 0, pen.w());
        pen.line(0.14f, 0.5f, 0.86f, 0.5f);
        break;
    case Icon::Cube:
    case Icon::Mesh:
        cube(pen);
        break;
    case Icon::Magnet:
        pen.arc(0.5f, 0.5f, 0.24f, 0.0f, kPi, 0.16f);
        pen.line(0.26f, 0.5f, 0.26f, 0.2f, 0.16f);
        pen.line(0.74f, 0.5f, 0.74f, 0.2f, 0.16f);
        break;
    case Icon::Focus:
        pen.circle(0.5f, 0.5f, 0.1f);
        pen.line(0.14f, 0.3f, 0.14f, 0.14f);
        pen.line(0.14f, 0.14f, 0.3f, 0.14f);
        pen.line(0.7f, 0.14f, 0.86f, 0.14f);
        pen.line(0.86f, 0.14f, 0.86f, 0.3f);
        pen.line(0.86f, 0.7f, 0.86f, 0.86f);
        pen.line(0.86f, 0.86f, 0.7f, 0.86f);
        pen.line(0.3f, 0.86f, 0.14f, 0.86f);
        pen.line(0.14f, 0.86f, 0.14f, 0.7f);
        break;
    case Icon::Folder:
    case Icon::FolderOpen: {
        const ImVec2 f[] = {{0.12f, 0.24f}, {0.4f, 0.24f}, {0.48f, 0.34f},
                            {0.88f, 0.34f}, {0.88f, 0.8f}, {0.12f, 0.8f}};
        pen.poly(f, true);
        if (icon == Icon::FolderOpen) pen.line(0.12f, 0.46f, 0.88f, 0.46f);
        break;
    }
    case Icon::File: {
        const ImVec2 f[] = {{0.22f, 0.1f}, {0.6f, 0.1f}, {0.8f, 0.3f}, {0.8f, 0.9f}, {0.22f, 0.9f}};
        pen.poly(f, true);
        pen.line(0.6f, 0.1f, 0.6f, 0.3f);
        pen.line(0.6f, 0.3f, 0.8f, 0.3f);
        break;
    }
    case Icon::Scene: {
        pen.rect(0.1f, 0.16f, 0.9f, 0.84f);
        const ImVec2 m[] = {{0.14f, 0.78f}, {0.38f, 0.44f}, {0.56f, 0.66f}, {0.68f, 0.52f}, {0.86f, 0.78f}};
        pen.poly(m, false);
        break;
    }
    case Icon::Entity:
        pen.circle(0.5f, 0.5f, 0.28f);
        pen.disc(0.5f, 0.5f, 0.08f);
        break;
    case Icon::Camera:
        pen.rect(0.1f, 0.3f, 0.64f, 0.74f, 0.06f);
        pen.tri(0.66f, 0.52f, 0.9f, 0.32f, 0.9f, 0.72f);
        break;
    case Icon::Light:
        pen.disc(0.5f, 0.5f, 0.16f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            pen.line(0.5f + std::cos(a) * 0.27f, 0.5f + std::sin(a) * 0.27f, 0.5f + std::cos(a) * 0.4f,
                     0.5f + std::sin(a) * 0.4f);
        }
        break;
    case Icon::Sprite:
    case Icon::Texture:
        picture(pen);
        break;
    case Icon::Model:
        cube(pen);
        pen.tri(0.5f, 0.1f, 0.87f, 0.3f, 0.5f, 0.5f);
        pen.tri(0.5f, 0.1f, 0.5f, 0.5f, 0.13f, 0.3f);
        break;
    case Icon::Material:
        pen.circle(0.5f, 0.5f, 0.34f);
        pen.arc(0.5f, 0.5f, 0.22f, kPi * 1.05f, kPi * 1.45f, 0.1f);
        break;
    case Icon::Shader: {
        const ImVec2 d[] = {{0.5f, 0.1f}, {0.62f, 0.38f}, {0.9f, 0.5f}, {0.62f, 0.62f},
                            {0.5f, 0.9f}, {0.38f, 0.62f}, {0.1f, 0.5f}, {0.38f, 0.38f}};
        pen.poly(d, true);
        break;
    }
    case Icon::Script: {
        const ImVec2 l[] = {{0.36f, 0.24f}, {0.12f, 0.5f}, {0.36f, 0.76f}};
        const ImVec2 r[] = {{0.64f, 0.24f}, {0.88f, 0.5f}, {0.64f, 0.76f}};
        pen.poly(l, false);
        pen.poly(r, false);
        pen.line(0.56f, 0.18f, 0.44f, 0.82f);
        break;
    }
    case Icon::Graph:
        pen.line(0.26f, 0.3f, 0.74f, 0.5f);
        pen.line(0.26f, 0.72f, 0.74f, 0.5f);
        pen.fill(0.1f, 0.2f, 0.36f, 0.4f, 0.04f);
        pen.fill(0.1f, 0.62f, 0.36f, 0.82f, 0.04f);
        pen.fill(0.66f, 0.4f, 0.9f, 0.6f, 0.04f);
        break;
    case Icon::Audio: {
        const ImVec2 sp[] = {{0.12f, 0.38f}, {0.3f, 0.38f}, {0.52f, 0.18f},
                             {0.52f, 0.82f}, {0.3f, 0.62f}, {0.12f, 0.62f}};
        pen.poly(sp, true);
        pen.arc(0.52f, 0.5f, 0.18f, -kPi * 0.3f, kPi * 0.3f);
        pen.arc(0.52f, 0.5f, 0.32f, -kPi * 0.3f, kPi * 0.3f);
        break;
    }
    case Icon::Prefab:
        cube(pen);
        pen.disc(0.5f, 0.5f, 0.08f);
        break;
    case Icon::Animation: {
        ImVec2 wv[12];
        for (int i = 0; i < 12; ++i) {
            const float x = 0.1f + 0.8f * static_cast<float>(i) / 11.0f;
            wv[i] = ImVec2(x, 0.5f - 0.26f * std::sin((x - 0.1f) / 0.8f * 2.0f * kPi));
        }
        pen.poly(wv, false);
        break;
    }
    case Icon::Terrain: {
        const ImVec2 m[] = {{0.08f, 0.8f}, {0.34f, 0.36f}, {0.5f, 0.58f}, {0.66f, 0.24f}, {0.92f, 0.8f}};
        pen.poly(m, true);
        break;
    }
    case Icon::Tileset:
        pen.rect(0.14f, 0.14f, 0.86f, 0.86f, 0.04f);
        pen.line(0.5f, 0.14f, 0.5f, 0.86f);
        pen.line(0.14f, 0.5f, 0.86f, 0.5f);
        pen.fill(0.14f, 0.14f, 0.5f, 0.5f);
        break;
    case Icon::Sequence:
        pen.line(0.1f, 0.3f, 0.9f, 0.3f);
        pen.line(0.1f, 0.7f, 0.9f, 0.7f);
        pen.fill(0.24f, 0.22f, 0.52f, 0.38f, 0.04f);
        pen.fill(0.46f, 0.62f, 0.8f, 0.78f, 0.04f);
        break;
    case Icon::Tree:
        pen.fill(0.38f, 0.1f, 0.62f, 0.3f, 0.04f);
        pen.fill(0.1f, 0.7f, 0.34f, 0.9f, 0.04f);
        pen.fill(0.66f, 0.7f, 0.9f, 0.9f, 0.04f);
        pen.line(0.5f, 0.3f, 0.5f, 0.5f);
        pen.line(0.22f, 0.5f, 0.78f, 0.5f);
        pen.line(0.22f, 0.5f, 0.22f, 0.7f);
        pen.line(0.78f, 0.5f, 0.78f, 0.7f);
        break;
    case Icon::Font:
        pen.text("Aa", 0.62f);
        break;
    case Icon::Particles:
        pen.disc(0.3f, 0.66f, 0.1f);
        pen.disc(0.62f, 0.4f, 0.13f);
        pen.disc(0.76f, 0.76f, 0.07f);
        pen.disc(0.3f, 0.26f, 0.06f);
        break;
    case Icon::Physics:
        pen.circle(0.5f, 0.42f, 0.24f);
        pen.line(0.12f, 0.84f, 0.88f, 0.84f);
        pen.line(0.5f, 0.66f, 0.5f, 0.84f);
        break;
    case Icon::Ui:
        pen.rect(0.1f, 0.2f, 0.9f, 0.8f, 0.06f);
        pen.fill(0.2f, 0.32f, 0.5f, 0.44f, 0.03f);
        pen.fill(0.2f, 0.56f, 0.8f, 0.68f, 0.03f);
        break;
    case Icon::Info:
        pen.circle(0.5f, 0.5f, 0.38f);
        pen.disc(0.5f, 0.31f, 0.06f);
        pen.line(0.5f, 0.44f, 0.5f, 0.72f, 0.11f);
        break;
    case Icon::Warning: {
        const ImVec2 t[] = {{0.5f, 0.12f}, {0.9f, 0.84f}, {0.1f, 0.84f}};
        pen.poly(t, true);
        pen.line(0.5f, 0.38f, 0.5f, 0.6f, 0.1f);
        pen.disc(0.5f, 0.71f, 0.05f);
        break;
    }
    case Icon::Error:
        pen.circle(0.5f, 0.5f, 0.38f);
        pen.line(0.36f, 0.36f, 0.64f, 0.64f, 0.1f);
        pen.line(0.64f, 0.36f, 0.36f, 0.64f, 0.1f);
        break;
    case Icon::Eye:
    case Icon::EyeOff:
        dl->PathArcTo(pen.p(0.5f, 0.95f), 0.56f * size, -kPi * 0.82f, -kPi * 0.18f, 0);
        dl->PathArcTo(pen.p(0.5f, 0.05f), 0.56f * size, kPi * 0.18f, kPi * 0.82f, 0);
        dl->PathStroke(col, pen.w(), ImDrawFlags_Closed);
        pen.disc(0.5f, 0.5f, 0.11f);
        if (icon == Icon::EyeOff) pen.line(0.16f, 0.84f, 0.84f, 0.16f, 0.1f);
        break;
    case Icon::Lock:
        pen.fill(0.2f, 0.46f, 0.8f, 0.88f, 0.06f);
        pen.arc(0.5f, 0.46f, 0.2f, kPi, 2.0f * kPi, 0.1f);
        break;
    case Icon::Network:
        pen.disc(0.5f, 0.2f, 0.1f);
        pen.disc(0.2f, 0.8f, 0.1f);
        pen.disc(0.8f, 0.8f, 0.1f);
        pen.line(0.5f, 0.2f, 0.2f, 0.8f);
        pen.line(0.5f, 0.2f, 0.8f, 0.8f);
        pen.line(0.2f, 0.8f, 0.8f, 0.8f);
        break;
    case Icon::Layout:
        pen.rect(0.1f, 0.14f, 0.9f, 0.86f, 0.05f);
        pen.line(0.38f, 0.14f, 0.38f, 0.86f);
        pen.line(0.38f, 0.56f, 0.9f, 0.56f);
        break;
    case Icon::Problems:
        pen.rect(0.16f, 0.12f, 0.84f, 0.88f, 0.06f);
        pen.line(0.3f, 0.32f, 0.7f, 0.32f);
        pen.line(0.3f, 0.5f, 0.7f, 0.5f);
        pen.disc(0.36f, 0.7f, 0.07f);
        pen.line(0.5f, 0.7f, 0.7f, 0.7f);
        break;
    case Icon::Rocket: {
        const ImVec2 b[] = {{0.5f, 0.08f}, {0.66f, 0.3f}, {0.66f, 0.68f}, {0.34f, 0.68f}, {0.34f, 0.3f}};
        pen.poly(b, true);
        pen.tri(0.34f, 0.5f, 0.34f, 0.74f, 0.18f, 0.74f);
        pen.tri(0.66f, 0.5f, 0.82f, 0.74f, 0.66f, 0.74f);
        pen.line(0.5f, 0.76f, 0.5f, 0.92f, 0.1f);
        break;
    }
    }
}

void icon(Icon ic, ImVec4 col) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = std::round(h * 0.06f);
    draw_icon(ImGui::GetWindowDrawList(), ic, ImVec2(p.x + pad, p.y + pad), h - 2.0f * pad, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(h, h));
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

bool icon_button(const char* id, Icon ic, const char* tip, bool selected, bool enabled, ImVec4 tint) {
    const float side = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + side, p.y + side);
    if (selected)
        dl->AddRectFilled(p, q, ImGui::GetColorU32(tk::kAccentSubtle), tk::kRadius);
    else if (held)
        dl->AddRectFilled(p, q, ImGui::GetColorU32(tk::kSurfaceActive), tk::kRadius);
    else if (hovered)
        dl->AddRectFilled(p, q, ImGui::GetColorU32(tk::kSurfaceHover), tk::kRadius);
    if (selected) dl->AddRect(p, q, ImGui::GetColorU32(tk::kAccentSoft), tk::kRadius);
    const float inset = std::round(side * 0.22f);
    const ImVec4 c = !enabled ? tk::kTextDisabled : selected ? tk::kAccent : tint;
    draw_icon(dl, ic, ImVec2(p.x + inset, p.y + inset), side - 2.0f * inset, ImGui::GetColorU32(c));
    ImGui::EndDisabled();
    tooltip(tip);
    ImGui::PopID();
    return clicked && enabled;
}

bool icon_text_button(const char* label, Icon ic, bool primary, bool enabled, ImVec2 size) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float ih = ImGui::GetTextLineHeight();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float gap = st.ItemInnerSpacing.x;
    ImVec2 sz(ts.x + ih + gap + st.FramePadding.x * 2.0f, ImGui::GetFrameHeight());
    if (size.x > 0) sz.x = size.x;
    if (size.y > 0) sz.y = size.y;
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, tk::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tk::kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, tk::kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, tk::kSurfaceBase);
    }
    ImGui::BeginDisabled(!enabled);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // The button carries no visible text; icon and label are drawn over it.
    const bool clicked = ImGui::Button((std::string("##") + label).c_str(), sz);
    const float content = ih + gap + ts.x;
    const float x0 = p.x + std::max(st.FramePadding.x, (sz.x - content) * 0.5f);
    const float y0 = p.y + (sz.y - ih) * 0.5f;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inset = std::round(ih * 0.08f);
    draw_icon(dl, ic, ImVec2(x0 + inset, y0 + inset), ih - 2.0f * inset, col);
    dl->AddText(ImVec2(x0 + ih + gap, y0), col, label, end);
    ImGui::EndDisabled();
    if (primary) ImGui::PopStyleColor(4);
    return clicked && enabled;
}

void heading(std::string_view text, float scale) {
    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopFont();
}

void subtitle(std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, tk::kTextMuted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void pill(std::string_view text, ui::Tone t) {
    const ImVec4 c = ui::tone(t);
    const ImVec2 ts = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const float px = tk::kSpace3, py = tk::kSpace1;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const float y = p.y + (h - ts.y - 2 * py) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, y), b(p.x + ts.x + 2 * px, y + ts.y + 2 * py);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ui::with_alpha(c, 0.16f)), (b.y - a.y) * 0.5f);
    dl->AddRect(a, b, ImGui::GetColorU32(ui::with_alpha(c, 0.45f)), (b.y - a.y) * 0.5f);
    dl->AddText(ImVec2(p.x + px, y + py), ImGui::GetColorU32(c), text.data(), text.data() + text.size());
    ImGui::Dummy(ImVec2(b.x - a.x, h));
}

bool empty_state(Icon ic, std::string_view title, std::string_view body, const char* action) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::min(avail.x, 420.0f);
    const float big = std::round(ImGui::GetTextLineHeight() * 2.4f);
    const float x = ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - width) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(tk::kSpace5, avail.y * 0.18f));
    ImGui::SetCursorPosX(x + (width - big) * 0.5f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_icon(ImGui::GetWindowDrawList(), ic, p, big, ImGui::GetColorU32(tk::kTextDisabled));
    ImGui::Dummy(ImVec2(big, big + tk::kSpace3));
    const auto centered = [&](std::string_view s, bool bold) {
        ImGui::PushFont(bold ? bold_font() : nullptr, 0.0f);
        const float tw = ImGui::CalcTextSize(s.data(), s.data() + s.size(), false, width).x;
        ImGui::SetCursorPosX(x + std::max(0.0f, (width - tw) * 0.5f));
        ImGui::PushTextWrapPos(x + width);
        ImGui::TextUnformatted(s.data(), s.data() + s.size());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };
    centered(title, true);
    ImGui::PushStyleColor(ImGuiCol_Text, tk::kTextMuted);
    centered(body, false);
    ImGui::PopStyleColor();
    if (!action) return false;
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    const float bw = ImGui::CalcTextSize(action).x + ImGui::GetStyle().FramePadding.x * 4.0f;
    ImGui::SetCursorPosX(x + (width - bw) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, tk::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tk::kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tk::kAccent);
    ImGui::PushStyleColor(ImGuiCol_Text, tk::kSurfaceBase);
    const bool clicked = ImGui::Button(action, ImVec2(bw, 0));
    ImGui::PopStyleColor(4);
    return clicked;
}

namespace {

ImFont* find_font(const char* name) {
    if (!ImGui::GetCurrentContext()) return nullptr;
    for (ImFont* f : ImGui::GetIO().Fonts->Fonts)
        if (f && std::strcmp(f->GetDebugName(), name) == 0) return f;
    return nullptr;
}

ImFont* add_font(std::span<const unsigned char> data, const char* name, bool merge) {
    ImFontConfig cfg;
    cfg.MergeMode = merge;
    cfg.FontDataOwnedByAtlas = false; // static data embedded in the runtime library
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    // stb_truetype only reads the buffer; ImGui's signature is non-const for owned data.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data.data()),
                                                      static_cast<int>(data.size()), tk::kFontBase, &cfg);
}

constexpr const char* kBody = "Inter Medium";
constexpr const char* kBold = "Inter Bold";

} // namespace

ImFont* body_font() { return find_font(kBody); }
ImFont* bold_font() { return find_font(kBold); }

bool install_fonts() {
    if (!ImGui::GetCurrentContext()) return false;
    if (body_font() && bold_font()) return true;
    namespace bf = runtime::builtin_fonts;
    ImFont* body = add_font(bf::inter_medium(), kBody, false);
    const bool body_ext = body && add_font(bf::inter_medium_latin_ext(), kBody, true) &&
                          add_font(bf::inter_medium_cyrillic(), kBody, true) &&
                          add_font(bf::inter_medium_greek(), kBody, true);
    ImFont* bold = add_font(bf::inter_bold(), kBold, false);
    const bool bold_ext = bold && add_font(bf::inter_bold_latin_ext(), kBold, true) &&
                          add_font(bf::inter_bold_cyrillic(), kBold, true) &&
                          add_font(bf::inter_bold_greek(), kBold, true);
    if (!body || !bold) {
        VICTUS_LOG_WARN("editor", "Inter fonts failed to load; using the built-in font");
        return false;
    }
    if (!body_ext || !bold_ext)
        VICTUS_LOG_WARN("editor", "extended glyph sets failed to load; only Latin-1 text renders");
    ImGui::GetIO().FontDefault = body;
    return true;
}

Icon icon_for(assets::AssetKind kind) noexcept {
    using K = assets::AssetKind;
    switch (kind) {
    case K::Texture:
        return Icon::Texture;
    case K::Model:
        return Icon::Model;
    case K::Scene:
        return Icon::Scene;
    case K::Script:
        return Icon::Script;
    case K::Audio:
        return Icon::Audio;
    case K::Prefab:
        return Icon::Prefab;
    case K::Material:
        return Icon::Material;
    case K::Shader:
        return Icon::Shader;
    case K::SpriteSheet:
        return Icon::Sprite;
    case K::Tileset:
        return Icon::Tileset;
    case K::Terrain:
        return Icon::Terrain;
    case K::BehaviorTree:
        return Icon::Tree;
    case K::Sequence:
        return Icon::Sequence;
    case K::StringTable:
        return Icon::Font;
    case K::AnimController:
        return Icon::Animation;
    case K::Unknown:
        break;
    }
    return Icon::File;
}

ImVec4 tint_for(assets::AssetKind kind) noexcept {
    using K = assets::AssetKind;
    switch (kind) {
    case K::Scene:
        return tk::kAccent;
    case K::Prefab:
        return tk::kInfo;
    case K::Script:
    case K::BehaviorTree:
    case K::Sequence:
    case K::AnimController:
        return tk::kSuccess;
    case K::Material:
    case K::Shader:
        return tk::kWarning;
    default:
        return tk::kTextMuted;
    }
}

bool contains_icase(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lower(hay[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

bool search_input(const char* id, const char* hint, std::string& text, float width) {
    ImGui::PushID(id);
    const float icon_size = ImGui::GetTextLineHeight();
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float clear_w = text.empty() ? 0.0f : ImGui::GetFrameHeight();
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0f, w - clear_w - (clear_w > 0 ? ImGui::GetStyle().ItemSpacing.x : 0.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad.x + icon_size + tk::kSpace2, pad.y));
    bool changed = ImGui::InputTextWithHint("##search", hint, &text);
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const float y = min.y + (ImGui::GetItemRectSize().y - icon_size) * 0.5f;
    draw_icon(ImGui::GetWindowDrawList(), Icon::Search, ImVec2(min.x + pad.x, y), icon_size,
              ImGui::GetColorU32(active ? tk::kText : tk::kTextMuted));
    if (active && !text.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        text.clear();
        changed = true;
    }
    if (!text.empty()) {
        ImGui::SameLine();
        if (icon_button("##clear", Icon::Close, "Clear the search (Esc)")) {
            text.clear();
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

} // namespace victus::editor::kit
