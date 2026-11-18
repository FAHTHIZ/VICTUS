// SPDX-License-Identifier: Apache-2.0
// The viewport transform tool: connects gizmo math (gizmo.cpp) to the selection, undo,
// prefab overrides, the viewport pointer and its tool strip.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/scene/prefab.hpp"

#include <imgui.h>

#include <algorithm>

namespace victus::editor {

using namespace scene;

namespace {
bool has_selected_ancestor(Scene& s, Entity e, const std::vector<Uuid>& selection) {
    for (Entity p = s.parent(e); !p.is_null(); p = s.parent(p))
        if (std::find(selection.begin(), selection.end(), s.uuid(p)) != selection.end()) return true;
    return false;
}
const char* gizmo_label(gizmo::Mode m) {
    switch (m) {
    case gizmo::Mode::Translate: return "Move";
    case gizmo::Mode::Rotate: return "Rotate";
    case gizmo::Mode::Scale: return "Scale";
    }
    return "Edit";
}
} // namespace

gizmo::View Editor::gizmo_view(math::Vec2 size) {
    size = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};
    if (runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD)
        return gizmo::View(camera_.camera2d(size.y).view_projection(size.x, size.y), size);
    const render::Camera3D cam = camera_.camera3d();
    return gizmo::View(cam.projection(size.x / size.y) * cam.view, size);
}

std::optional<gizmo::Frame> Editor::gizmo_frame() {
    if (play_state_ != PlayState::Editing || !gizmo_.visible) return std::nullopt;
    const Entity e = selected();
    if (e.is_null() || !active_scene().world().has<Transform>(e)) return std::nullopt;
    if (gizmo_active_) return gizmo_active_->frame();
    math::Vec3 t, sc;
    math::Quat r;
    math::decompose(active_scene().world_matrix(e), t, r, sc);
    gizmo::Frame f{t, {}};
    if (gizmo_.space == gizmo::Space::Local || gizmo_.mode == gizmo::Mode::Scale) f.orientation = math::normalize(r);
    return f;
}

gizmo::Handle Editor::gizmo_hover(math::Vec2 pixel, math::Vec2 size) {
    const auto frame = gizmo_frame();
    if (!frame) return gizmo::Handle::None;
    return gizmo::hit_test(gizmo_.mode, gizmo_view(size), *frame, pixel);
}

bool Editor::gizmo_press(math::Vec2 pixel, math::Vec2 size) {
    if (gizmo_active_) gizmo_cancel();
    const auto frame = gizmo_frame();
    if (!frame) return false;
    const gizmo::View view = gizmo_view(size);
    const gizmo::Handle h = gizmo::hit_test(gizmo_.mode, view, *frame, pixel);
    if (h == gizmo::Handle::None) return false;
    auto drag = gizmo::Drag::begin(gizmo_.mode, h, view, *frame, pixel);
    if (!drag) return false;
    Scene& s = active_scene();
    gizmo_targets_.clear();
    std::vector<Uuid> ids;
    for (const Uuid& u : selection_) {
        const Entity e = s.find(u);
        if (e.is_null() || !s.world().has<Transform>(e) || has_selected_ancestor(s, e, selection_)) continue;
        GizmoTarget t;
        t.id = u;
        t.local = *s.world().get<Transform>(e);
        math::decompose(s.world_matrix(e), t.position, t.rotation, t.scale);
        t.rotation = math::normalize(t.rotation);
        gizmo_targets_.push_back(t);
        ids.push_back(u);
        // Edits to a prefab part record overrides on the instance root: capture it for undo.
        if (const Entity root = instance_root(s, e); !root.is_null() && root != e) {
            const Uuid ru = s.uuid(root);
            if (std::find(ids.begin(), ids.end(), ru) == ids.end()) ids.push_back(ru);
        }
    }
    if (gizmo_targets_.empty()) return false;
    gizmo_before_ = capture(s, ids);
    gizmo_active_ = drag;
    gizmo_hot_ = h;
    return true;
}

void Editor::apply_gizmo_delta(const gizmo::Delta& d) {
    if (!gizmo_active_) return;
    Scene& s = active_scene();
    const gizmo::Frame& f = gizmo_active_->frame();
    const math::Quat finv = f.orientation.conjugate();
    for (const GizmoTarget& t : gizmo_targets_) {
        const Entity e = s.find(t.id);
        if (e.is_null()) continue;
        math::Vec3 pos = t.position, scale = t.scale;
        math::Quat rot = t.rotation;
        switch (gizmo_active_->mode()) {
        case gizmo::Mode::Translate: pos = t.position + d.translation; break;
        case gizmo::Mode::Rotate:
            pos = f.pivot + d.rotation.rotate(t.position - f.pivot);
            rot = math::normalize(d.rotation * t.rotation);
            break;
        case gizmo::Mode::Scale: {
            // Offsets from the pivot scale along the frame axes; each entity's own scale is
            // multiplied by the factors (exact for the active entity, whose axes are the frame).
            const math::Vec3 off = finv.rotate(t.position - f.pivot) * d.scale;
            pos = f.pivot + f.orientation.rotate(off);
            scale = t.scale * d.scale;
            break;
        }
        }
        Transform* tr = s.world().get<Transform>(e);
        const Entity parent = s.parent(e);
        if (gizmo_active_->mode() == gizmo::Mode::Translate) { // rotation and scale stay bit-exact
            tr->position = parent.is_null() ? pos : math::inverse(s.world_matrix(parent)).transform_point(pos);
        } else if (parent.is_null()) {
            tr->position = pos;
            tr->rotation = rot;
            tr->scale = scale;
        } else {
            const math::Mat4 local = math::inverse(s.world_matrix(parent)) * math::Mat4::trs(pos, rot, scale);
            math::decompose(local, tr->position, tr->rotation, tr->scale);
            tr->rotation = math::normalize(tr->rotation);
        }
    }
    s.update_transforms();
}

void Editor::gizmo_drag(math::Vec2 pixel, math::Vec2 size, bool invert_snap) {
    if (!gizmo_active_) return;
    const bool snap = gizmo_.snap != invert_snap;
    const gizmo::Delta d = gizmo_active_->update(gizmo_view(size), pixel, snap ? &gizmo_.steps : nullptr);
    apply_gizmo_delta(d);
}

void Editor::gizmo_release() {
    if (!gizmo_active_) return;
    const gizmo::Mode mode = gizmo_active_->mode();
    Scene& s = active_scene();
    bool moved = false;
    for (const GizmoTarget& t : gizmo_targets_) {
        const Entity e = s.find(t.id);
        if (e.is_null()) continue;
        const Transform* tr = s.world().get<Transform>(e);
        if (tr->position != t.local.position || tr->rotation != t.local.rotation || tr->scale != t.local.scale) moved = true;
    }
    if (moved) {
        for (const GizmoTarget& t : gizmo_targets_)
            if (const Entity e = s.find(t.id); !e.is_null()) mark_prefab_overrides(e, gizmo_before_);
        record_edit(gizmo_label(mode), std::move(gizmo_before_));
    }
    gizmo_before_.clear();
    gizmo_targets_.clear();
    gizmo_active_.reset();
}

void Editor::gizmo_cancel() {
    if (!gizmo_active_) return;
    Scene& s = active_scene();
    for (const GizmoTarget& t : gizmo_targets_)
        if (const Entity e = s.find(t.id); !e.is_null()) *s.world().get<Transform>(e) = t.local;
    s.update_transforms();
    gizmo_before_.clear();
    gizmo_targets_.clear();
    gizmo_active_.reset();
}

// ---- UI -----------------------------------------------------------------------------------------

void Editor::gizmo_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || play_state_ != PlayState::Editing || gizmo_active_) return;
    // While the right button flies the camera, W/E steer it instead.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_.mode = gizmo::Mode::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_.mode = gizmo::Mode::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_.mode = gizmo::Mode::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false))
        gizmo_.space = gizmo_.space == gizmo::Space::World ? gizmo::Space::Local : gizmo::Space::World;
}

void Editor::gizmo_viewport(math::Vec2 origin, math::Vec2 size, bool hovered, bool painting) {
    ImGuiIO& io = ImGui::GetIO();
    const math::Vec2 mouse{io.MousePos.x - origin.x, io.MousePos.y - origin.y};
    if (play_state_ != PlayState::Editing) {
        gizmo_cancel();
        gizmo_hot_ = gizmo::Handle::None;
        return;
    }
    if (gizmo_active_) {
        // Drags started through the API (tests, tools) are driven by their caller; only a drag
        // started by a viewport click follows the mouse.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            gizmo_cancel();
        } else if (gizmo_consumed_click_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_drag(mouse, size, io.KeyCtrl);
            else gizmo_release();
        }
    } else {
        const bool usable = hovered && !painting && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
                            !ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        gizmo_hot_ = usable ? gizmo_hover(mouse, size) : gizmo::Handle::None;
        if (usable && gizmo_hot_ != gizmo::Handle::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            gizmo_consumed_click_ = gizmo_press(mouse, size);
    }

    // Draw.
    const auto frame = gizmo_frame();
    if (!frame) return;
    gizmo_draw_.clear();
    gizmo::build(gizmo_.mode, gizmo_view(size), *frame, gizmo_hot_, gizmo_active_ ? &*gizmo_active_ : nullptr, gizmo_draw_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), true);
    const auto at = [&](math::Vec2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
    for (const auto& poly : gizmo_draw_.polygons) {
        std::vector<ImVec2> pts;
        pts.reserve(poly.points.size());
        for (const auto& p : poly.points) pts.push_back(at(p));
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), poly.color);
    }
    for (const auto& l : gizmo_draw_.lines) dl->AddLine(at(l.a), at(l.b), l.color, l.thickness);
    if (gizmo_active_) {
        const std::string text = gizmo::describe(gizmo_active_->mode(), gizmo_active_->delta());
        const ImVec2 p(io.MousePos.x + 16, io.MousePos.y + 12);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ts.x + 4, p.y + ts.y + 2), IM_COL32(20, 20, 24, 210), 3.0f);
        dl->AddText(p, IM_COL32(255, 230, 150, 255), text.c_str());
    }
    dl->PopClipRect();
}

void Editor::draw_gizmo_toolbar(math::Vec2 origin, math::Vec2 size) {
    if (play_state_ != PlayState::Editing) return;
    // A floating icon strip in the viewport's top-left corner: tool, space, snapping, options.
    namespace tk = ui::tokens;
    (void)size;
    const float side = ImGui::GetFrameHeight();
    const float pad = tk::kSpace2;
    const float gap = tk::kSpace2;
    const float group_gap = tk::kSpace4;
    const float w = side * 6.0f + gap * 4.0f + group_gap + pad * 2.0f;
    const ImVec2 p0(origin.x + tk::kSpace3, origin.y + tk::kSpace3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + side + pad * 2.0f), ImGui::GetColorU32(ui::with_alpha(tk::kSurfaceBase, 0.88f)),
                      tk::kRadiusLarge);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + side + pad * 2.0f), ImGui::GetColorU32(tk::kBorder), tk::kRadiusLarge);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const auto tool = [&](const char* id, kit::Icon icon, gizmo::Mode m, const char* tip) {
        if (kit::icon_button(id, icon, tip, gizmo_.mode == m)) gizmo_.mode = m;
        ImGui::SameLine();
    };
    tool("move", kit::Icon::Move, gizmo::Mode::Translate, "Move (W): drag an arrow, a plane square or the centre");
    tool("rotate", kit::Icon::Rotate, gizmo::Mode::Rotate, "Rotate (E): drag a ring, or the outer ring to turn about the view");
    tool("scale", kit::Icon::Scale, gizmo::Mode::Scale, "Scale (R): drag an axis box, or the centre for uniform scale");
    ImGui::SameLine(0, group_gap - gap);
    const bool local = gizmo_.space == gizmo::Space::Local || gizmo_.mode == gizmo::Mode::Scale;
    if (kit::icon_button("space", local ? kit::Icon::Cube : kit::Icon::Globe,
                         local ? "Local axes (T toggles): handles follow the active entity (scale is always local)"
                               : "World axes (T toggles): handles align to the world",
                         false, gizmo_.mode != gizmo::Mode::Scale))
        gizmo_.space = gizmo_.space == gizmo::Space::World ? gizmo::Space::Local : gizmo::Space::World;
    ImGui::SameLine();
    if (kit::icon_button("snap", kit::Icon::Magnet, gizmo_.snap ? "Snapping on (hold Ctrl while dragging to invert)"
                                                                : "Snapping off (hold Ctrl while dragging to snap)",
                         gizmo_.snap))
        gizmo_.snap = !gizmo_.snap;
    ImGui::SameLine();
    if (kit::icon_button("steps", kit::Icon::Gear, "Snap steps and gizmo visibility")) ImGui::OpenPopup("##snapsteps");
    ImGui::PopStyleVar();
    if (ImGui::BeginPopup("##snapsteps")) {
        kit::heading("Snap steps", tk::kTypeBody);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Move", &gizmo_.steps.translate, 0.01f, 0.001f, 1000.0f, "%.3f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Rotate (deg)", &gizmo_.steps.rotate_degrees, 0.5f, 0.1f, 180.0f, "%.1f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Scale", &gizmo_.steps.scale, 0.005f, 0.001f, 10.0f, "%.3f");
        gizmo_.steps.translate = std::max(gizmo_.steps.translate, 0.001f);
        gizmo_.steps.rotate_degrees = std::max(gizmo_.steps.rotate_degrees, 0.1f);
        gizmo_.steps.scale = std::max(gizmo_.steps.scale, 0.001f);
        ImGui::Separator();
        ImGui::Checkbox("Show gizmo", &gizmo_.visible);
        ImGui::EndPopup();
    }
}

} // namespace victus::editor
