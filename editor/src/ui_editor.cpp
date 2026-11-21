// SPDX-License-Identifier: Apache-2.0
// UI Editor (ADR-0193): a visual canvas for in-game UI. The edited canvas is rendered with the
// runtime GameUiRenderer at a chosen preview screen size (what the game shows on that screen);
// elements are picked, moved and resized with handles on top of it, anchors are set from a 4x4
// preset picker without moving the element, and the toolbar adds panels, text, images, buttons,
// sliders, toggles, text fields, progress bars, layout groups and scroll views. Every edit is one
// scene undo step. The rectangle math lives in gameui (local_rect / set_local_rect / set_anchors)
// so it is tested headlessly.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/editor/undo.hpp"
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>

namespace victus::editor {

using gameui::Rect;
using scene::Entity;
using scene::Scene;
namespace tk = ui::tokens;

namespace {

ImU32 u32(ImVec4 c, float alpha = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ui::with_alpha(c, c.w * alpha)); }

bool same_rect(const gameui::UiRectComponent& a, const gameui::UiRectComponent& b) {
    const auto eq = [](math::Vec2 x, math::Vec2 y) { return std::abs(x.x - y.x) < 1e-4f && std::abs(x.y - y.y) < 1e-4f; };
    return eq(a.anchor_min, b.anchor_min) && eq(a.anchor_max, b.anchor_max) && eq(a.pivot, b.pivot) && eq(a.position, b.position) &&
           eq(a.size, b.size);
}

template <class Fn>
void preorder(const Scene& s, Entity e, Fn&& fn) {
    const ecs::World& w = s.world();
    fn(e);
    const auto* h = w.get<scene::Hierarchy>(e);
    for (Entity c = h ? h->first_child : Entity{}; !c.is_null();) {
        const auto* ch = w.get<scene::Hierarchy>(c);
        const Entity next = ch ? ch->next_sibling : Entity{};
        if (!w.has<gameui::UiCanvasComponent>(c)) preorder(s, c, fn); // nested canvases are separate roots
        c = next;
    }
}

float snap_to(float v, float grid) { return std::round(v / grid) * grid; }

std::string name_of(const Scene& s, Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? n->value : std::string("Entity");
}

} // namespace

const char* ui_element_label(UiElementKind kind) noexcept {
    switch (kind) {
    case UiElementKind::Canvas: return "Canvas";
    case UiElementKind::Panel: return "Panel";
    case UiElementKind::Image: return "Image";
    case UiElementKind::Text: return "Text";
    case UiElementKind::Button: return "Button";
    case UiElementKind::Slider: return "Slider";
    case UiElementKind::Toggle: return "Toggle";
    case UiElementKind::TextField: return "Text Field";
    case UiElementKind::ProgressBar: return "Progress Bar";
    case UiElementKind::VerticalLayout: return "Vertical Layout";
    case UiElementKind::HorizontalLayout: return "Horizontal Layout";
    case UiElementKind::ScrollView: return "Scroll View";
    }
    return "Element";
}

const char* ui_anchor_preset_label(UiAnchorPreset p) noexcept {
    static constexpr const char* kLabels[] = {
        "Top left", "Top centre", "Top right", "Top, stretch across",
        "Middle left", "Centre", "Middle right", "Middle, stretch across",
        "Bottom left", "Bottom centre", "Bottom right", "Bottom, stretch across",
        "Left, stretch down", "Centre, stretch down", "Right, stretch down", "Fill the parent"};
    return kLabels[static_cast<int>(p)];
}

void ui_anchor_preset_values(UiAnchorPreset p, math::Vec2& amin, math::Vec2& amax, math::Vec2& pivot) noexcept {
    const int i = static_cast<int>(p);
    const int row = i / 4, col = i % 4;
    // Columns: left, centre, right, stretch; rows: top, middle, bottom, stretch.
    static constexpr float kFrac[] = {0.0f, 0.5f, 1.0f};
    if (col < 3) {
        amin.x = amax.x = kFrac[col];
        pivot.x = kFrac[col];
    } else {
        amin.x = 0.0f;
        amax.x = 1.0f;
        pivot.x = 0.5f;
    }
    if (row < 3) {
        amin.y = amax.y = kFrac[row];
        pivot.y = kFrac[row];
    } else {
        amin.y = 0.0f;
        amax.y = 1.0f;
        pivot.y = 0.5f;
    }
}

const std::vector<UiPreviewSize>& ui_preview_sizes() {
    static const std::vector<UiPreviewSize> kSizes = {
        {"HD 1280 x 720", 1280, 720},
        {"Full HD 1920 x 1080", 1920, 1080},
        {"QHD 2560 x 1440", 2560, 1440},
        {"Ultrawide 3440 x 1440", 3440, 1440},
        {"4:3 1024 x 768", 1024, 768},
        {"Phone landscape 2340 x 1080", 2340, 1080},
        {"Phone portrait 1080 x 2340", 1080, 2340},
        {"Tablet 2048 x 1536", 2048, 1536},
    };
    return kSizes;
}

// ---- model -----------------------------------------------------------------------------------------------

Entity Editor::ui_editor_canvas() {
    if (!scene_) return {};
    Scene& s = active_scene();
    const ecs::World& w = s.world();
    if (!selection_.empty()) {
        const Entity sel = s.find(selection_.front());
        if (!sel.is_null()) {
            const Entity c = gameui::canvas_of(s, sel);
            if (!c.is_null()) {
                ui_canvas_ = s.uuid(c);
                return c;
            }
        }
    }
    if (!ui_canvas_.is_nil()) {
        const Entity e = s.find(ui_canvas_);
        if (!e.is_null() && w.has<gameui::UiCanvasComponent>(e)) return e;
    }
    Entity first;
    s.world().each<gameui::UiCanvasComponent>([&](Entity e, gameui::UiCanvasComponent&) {
        if (first.is_null()) first = e;
    });
    if (!first.is_null()) ui_canvas_ = s.uuid(first);
    return first;
}

void Editor::set_ui_editor_canvas(Entity canvas) {
    if (!scene_ || canvas.is_null() || !active_scene().world().has<gameui::UiCanvasComponent>(canvas)) return;
    ui_canvas_ = active_scene().uuid(canvas);
    selection_ = {ui_canvas_};
}

void Editor::set_ui_preview_size(std::uint32_t width, std::uint32_t height) noexcept {
    ui_preview_w_ = std::min(width, 4096u);
    ui_preview_h_ = std::min(height, 4096u);
}

math::Vec2 Editor::ui_preview_size() {
    if (ui_preview_w_ > 0 && ui_preview_h_ > 0) return {static_cast<float>(ui_preview_w_), static_cast<float>(ui_preview_h_)};
    const Entity c = ui_editor_canvas();
    if (!c.is_null())
        if (const auto* cc = active_scene().world().get<gameui::UiCanvasComponent>(c))
            return {std::clamp(cc->reference_size.x, 16.0f, 4096.0f), std::clamp(cc->reference_size.y, 16.0f, 4096.0f)};
    return {1280, 720};
}

Entity Editor::create_ui_element(UiElementKind kind, Entity parent) {
    if (!scene_ || play_state_ != PlayState::Editing) return {};
    Scene& s = active_scene();
    ecs::World& w = s.world();
    const char* label = ui_element_label(kind);
    Entity root; // top of what this call creates (one undo step for all of it)
    const auto make_canvas = [&] {
        const Entity c = s.create("Canvas", {});
        w.add<gameui::UiCanvasComponent>(c);
        return c;
    };
    Entity e;
    if (kind == UiElementKind::Canvas) {
        e = root = make_canvas();
    } else {
        if (parent.is_null() && !selection_.empty()) {
            const Entity sel = s.find(selection_.front());
            if (!sel.is_null() && (w.has<gameui::UiRectComponent>(sel) || w.has<gameui::UiCanvasComponent>(sel))) parent = sel;
        }
        if (parent.is_null()) parent = ui_editor_canvas();
        if (parent.is_null()) parent = root = make_canvas();
        e = s.create(label, parent);
        if (root.is_null()) root = e;
        auto& r = w.add<gameui::UiRectComponent>(e);
        switch (kind) {
        case UiElementKind::Canvas: break;
        case UiElementKind::Panel:
            r.size = {400, 300};
            w.add<gameui::UiPanelComponent>(e);
            break;
        case UiElementKind::Image:
            r.size = {160, 160};
            w.add<gameui::UiImageComponent>(e);
            break;
        case UiElementKind::Text:
            r.size = {320, 60};
            w.add<gameui::UiTextComponent>(e).text = "New text";
            break;
        case UiElementKind::Button: {
            r.size = {240, 64};
            w.add<gameui::UiButtonComponent>(e).action = "button";
            auto& t = w.add<gameui::UiTextComponent>(e);
            t.text = "Button";
            t.font_size = 26;
            break;
        }
        case UiElementKind::Slider:
            r.size = {320, 32};
            w.add<gameui::UiSliderComponent>(e).action = "slider";
            break;
        case UiElementKind::Toggle:
            r.size = {64, 34};
            w.add<gameui::UiToggleComponent>(e).action = "toggle";
            break;
        case UiElementKind::TextField:
            r.size = {320, 52};
            w.add<gameui::UiTextInputComponent>(e).action = "text";
            break;
        case UiElementKind::ProgressBar:
            r.size = {320, 20};
            w.add<gameui::UiProgressBarComponent>(e);
            break;
        case UiElementKind::VerticalLayout:
            r.size = {320, 400};
            w.add<gameui::UiPanelComponent>(e);
            w.add<gameui::UiLayoutComponent>(e).direction = gameui::LayoutDirection::Vertical;
            break;
        case UiElementKind::HorizontalLayout: {
            r.size = {640, 88};
            w.add<gameui::UiPanelComponent>(e);
            auto& l = w.add<gameui::UiLayoutComponent>(e);
            l.direction = gameui::LayoutDirection::Horizontal;
            l.align = gameui::LayoutAlign::Center;
            break;
        }
        case UiElementKind::ScrollView:
            r.size = {400, 300};
            r.clip_children = true;
            w.add<gameui::UiPanelComponent>(e);
            w.add<gameui::UiScrollComponent>(e);
            w.add<gameui::UiLayoutComponent>(e).direction = gameui::LayoutDirection::Vertical;
            break;
        }
    }
    s.update_transforms();
    gameui::layout(s); // element rects exist right away (handles, parent sizes)
    auto after = capture_subtrees(s, {s.uuid(root)});
    undo_.push({std::string("Create ") + label, absent(after), std::move(after)});
    update_title();
    const Entity c = gameui::canvas_of(s, e);
    if (!c.is_null()) ui_canvas_ = s.uuid(c);
    selection_ = {s.uuid(e)};
    return e;
}

namespace {

// Parent size in canvas units: from the last UI Editor render when the parent is there (the
// preview's layout), else from the scene's last layout.
std::optional<math::Vec2> parent_size_for(Scene& s, Entity e, const auto& rects) {
    const Entity p = gameui::parent_element(s, e);
    if (p.is_null()) return std::nullopt;
    const Uuid pid = s.uuid(p);
    for (const auto& r : rects)
        if (r.uuid == pid && r.scale > 0.0f) return math::Vec2{r.rect.w / r.scale, r.rect.h / r.scale};
    return gameui::parent_size(s, e);
}

} // namespace

std::optional<Rect> Editor::ui_element_rect(Entity e) {
    if (!scene_ || e.is_null()) return std::nullopt;
    Scene& s = active_scene();
    const auto* r = s.world().get<gameui::UiRectComponent>(e);
    if (!r) return std::nullopt;
    if (gameui::placed_by_layout(s, e)) {
        // The layout decides where it goes; report the size it asks for at its current place.
        const auto* st = s.world().get<gameui::UiStateComponent>(e);
        const Entity p = gameui::parent_element(s, e);
        const auto* ps = s.world().get<gameui::UiStateComponent>(p);
        if (!st || !ps || st->scale <= 0.0f) return Rect{0, 0, r->size.x, r->size.y};
        return Rect{(st->rect.x - ps->rect.x) / st->scale, (st->rect.y - ps->rect.y) / st->scale, r->size.x, r->size.y};
    }
    const auto psize = parent_size_for(s, e, ui_rects_);
    if (!psize) return std::nullopt;
    return gameui::local_rect(*r, *psize);
}

bool Editor::set_ui_element_rect(Entity e, const Rect& local) {
    if (!scene_ || play_state_ != PlayState::Editing || e.is_null()) return false;
    Scene& s = active_scene();
    auto* r = s.world().get<gameui::UiRectComponent>(e);
    if (!r) return false;
    const gameui::UiRectComponent old = *r;
    auto before = capture(s, {s.uuid(e)});
    if (gameui::placed_by_layout(s, e)) {
        r->size = {std::max(0.0f, local.w), std::max(0.0f, local.h)};
    } else {
        const auto psize = parent_size_for(s, e, ui_rects_);
        if (!psize) return false;
        gameui::set_local_rect(*r, *psize, local);
    }
    if (same_rect(old, *r)) return true;
    gameui::layout(s);
    record_edit("Move UI element", std::move(before));
    return true;
}

bool Editor::apply_ui_anchor_preset(Entity e, UiAnchorPreset preset) {
    if (!scene_ || play_state_ != PlayState::Editing || e.is_null()) return false;
    Scene& s = active_scene();
    auto* r = s.world().get<gameui::UiRectComponent>(e);
    if (!r || gameui::placed_by_layout(s, e)) return false;
    const auto psize = parent_size_for(s, e, ui_rects_);
    if (!psize) return false;
    const gameui::UiRectComponent old = *r;
    auto before = capture(s, {s.uuid(e)});
    math::Vec2 amin, amax, pivot;
    ui_anchor_preset_values(preset, amin, amax, pivot);
    gameui::set_anchors(*r, *psize, amin, amax, pivot);
    if (same_rect(old, *r)) return true;
    gameui::layout(s);
    record_edit(std::string("Anchor: ") + ui_anchor_preset_label(preset), std::move(before));
    return true;
}

std::optional<Rect> Editor::ui_editor_preview_rect(Entity e) const {
    if (!scene_ || e.is_null()) return std::nullopt;
    const Uuid id = const_cast<Editor*>(this)->active_scene().uuid(e);
    for (const auto& r : ui_rects_)
        if (r.uuid == id) return r.rect;
    return std::nullopt;
}

std::optional<math::Vec4> Editor::ui_editor_element_screen(Entity e) const {
    if (!scene_ || e.is_null()) return std::nullopt;
    const Uuid id = const_cast<Editor*>(this)->active_scene().uuid(e);
    for (const auto& [u, v] : ui_screen_)
        if (u == id) return v;
    return std::nullopt;
}

// ---- rendering -------------------------------------------------------------------------------------------

Status Editor::ensure_ui_texture(std::uint32_t w, std::uint32_t h) {
    if (ui_tex_ && w == ui_tex_w_ && h == ui_tex_h_) return {};
    if (ui_tex_) gpu_.device().destroy(ui_tex_);
    ui_tex_ = {};
    rhi::TextureDesc d;
    d.name = "ui_editor_preview";
    d.format = rhi::Format::RGBA8_UNORM;
    d.width = w;
    d.height = h;
    d.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    auto t = gpu_.device().create_texture(d);
    if (!t) return std::move(t).error();
    ui_tex_ = t.value();
    ui_tex_w_ = w;
    ui_tex_h_ = h;
    return {};
}

Status Editor::render_ui_editor(rhi::CommandBuffer& cmd) {
    if (!show_ui_editor_ || !ui_visible_ || !scene_ || !cache_) return {};
    VICTUS_PROFILE_SCOPE("editor.ui_editor");
    Scene& s = active_scene();
    const Entity canvas = ui_editor_canvas();
    if (canvas.is_null()) {
        ui_rects_.clear();
        return {};
    }
    const math::Vec2 size = ui_preview_size();
    const auto W = static_cast<std::uint32_t>(std::clamp(std::lround(size.x), 16L, 4096L));
    const auto H = static_cast<std::uint32_t>(std::clamp(std::lround(size.y), 16L, 4096L));
    VICTUS_TRY_STATUS(ensure_ui_texture(W, H));
    if (!ui_renderer_) {
        auto r = runtime::GameUiRenderer::create(gpu_, rhi::Format::RGBA8_UNORM);
        if (!r) return std::move(r).error();
        ui_renderer_ = std::move(r).value();
    }
    {
        rhi::RenderPassDesc d;
        rhi::ColorAttachment a;
        a.texture = ui_tex_;
        a.load = rhi::LoadOp::Clear;
        a.clear = {0.13f, 0.14f, 0.17f, 1.0f};
        d.colors.push_back(a);
        auto pass = cmd.begin_render_pass(d);
        pass.end();
    }
    // Only the edited canvas is drawn (even when it is hidden in the game); the others are
    // hidden for this render and restored right after.
    ecs::World& w = s.world();
    std::vector<std::pair<Entity, bool>> saved;
    w.each<gameui::UiCanvasComponent>([&](Entity e, gameui::UiCanvasComponent& c) {
        saved.emplace_back(e, c.visible);
        c.visible = e == canvas;
    });
    auto stats = ui_renderer_->render(cmd, s, *cache_, ui_tex_, W, H);
    ui_rects_.clear();
    preorder(s, canvas, [&](Entity e) {
        const auto* st = w.get<gameui::UiStateComponent>(e);
        if (!st || (e != canvas && !w.has<gameui::UiRectComponent>(e))) return;
        ui_rects_.push_back({s.uuid(e), st->rect, st->scale, st->order, st->visible});
    });
    for (const auto& [e, v] : saved)
        if (auto* c = w.get<gameui::UiCanvasComponent>(e)) c->visible = v;
    ui_rendered_size_ = {static_cast<float>(W), static_cast<float>(H)};
    if (!stats) return std::move(stats).error();
    return {};
}

// ---- window ----------------------------------------------------------------------------------------------

void Editor::draw_ui_editor() {
    ui_visible_ = false;
    if (!show_ui_editor_) return;
    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI Editor", &show_ui_editor_)) {
        ImGui::End();
        return;
    }
    if (!project_ || !scene_) {
        kit::empty_state(kit::Icon::Ui, "No project open", "Open or create a project to design its HUD and menus.");
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    ecs::World& w = s.world();
    const bool editing = play_state_ == PlayState::Editing;
    const Entity canvas = ui_editor_canvas();
    if (canvas.is_null()) {
        ui_rects_.clear();
        ui_screen_.clear();
        if (kit::empty_state(kit::Icon::Ui, "No UI canvas in this scene",
                             "A canvas holds a HUD or a menu: scores, health bars, buttons. Create one, then add text, "
                             "images and buttons to it from the Add menu.",
                             editing ? "Create Canvas" : nullptr))
            create_ui_element(UiElementKind::Canvas);
        ImGui::End();
        return;
    }
    ui_visible_ = true;
    ImGuiIO& io = ImGui::GetIO();

    // The selected element of this canvas (null when the selection is elsewhere or the canvas).
    Entity sel;
    if (!selection_.empty()) {
        const Entity e = s.find(selection_.front());
        if (!e.is_null() && gameui::canvas_of(s, e) == canvas && w.has<gameui::UiRectComponent>(e)) sel = e;
    }
    const bool sel_by_layout = !sel.is_null() && gameui::placed_by_layout(s, sel);

    // ---- toolbar ----
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##uicanvas", name_of(s, canvas).c_str())) {
        w.each<gameui::UiCanvasComponent>([&](Entity e, gameui::UiCanvasComponent&) {
            const std::string item = name_of(s, e) + "##" + s.uuid(e).to_string();
            if (ImGui::Selectable(item.c_str(), e == canvas)) set_ui_editor_canvas(e);
        });
        ImGui::EndCombo();
    }
    kit::tooltip("The canvas being edited");
    ImGui::SameLine();
    const math::Vec2 psize = ui_preview_size();
    std::string preview_label = std::format("{} x {}", static_cast<int>(psize.x), static_cast<int>(psize.y));
    if (ui_preview_w_ == 0) preview_label = "Reference " + preview_label;
    for (const auto& p : ui_preview_sizes())
        if (p.width == ui_preview_w_ && p.height == ui_preview_h_) preview_label = p.name;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##uipreview", preview_label.c_str())) {
        if (ImGui::Selectable("Canvas reference size", ui_preview_w_ == 0)) set_ui_preview_size(0, 0);
        ImGui::Separator();
        for (const auto& p : ui_preview_sizes())
            if (ImGui::Selectable(p.name, p.width == ui_preview_w_ && p.height == ui_preview_h_)) set_ui_preview_size(p.width, p.height);
        ImGui::EndCombo();
    }
    kit::tooltip("Screen size to preview: anchors and canvas scaling adapt the layout like the game will");
    ImGui::SameLine();
    if (kit::icon_text_button("Add", kit::Icon::Plus, false, editing)) ImGui::OpenPopup("##uiadd");
    if (ImGui::BeginPopup("##uiadd")) {
        ImGui::TextDisabled(sel.is_null() ? "Adds to the canvas" : "Adds inside the selected element");
        for (int k = static_cast<int>(UiElementKind::Panel); k <= static_cast<int>(UiElementKind::ScrollView); ++k) {
            const auto kind = static_cast<UiElementKind>(k);
            if (ImGui::MenuItem(ui_element_label(kind))) create_ui_element(kind, sel.is_null() ? canvas : sel);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Canvas")) create_ui_element(UiElementKind::Canvas);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool can_anchor = editing && !sel.is_null() && !sel_by_layout;
    if (kit::icon_text_button("Anchors", kit::Icon::Layout, false, can_anchor)) ImGui::OpenPopup("##uianchors");
    if (!sel.is_null() && sel_by_layout) kit::tooltip("This element is placed by its parent's Layout");
    if (ImGui::BeginPopup("##uianchors")) {
        ImGui::TextDisabled("Pin to the parent (keeps the element where it is)");
        const auto* rc = sel.is_null() ? nullptr : w.get<gameui::UiRectComponent>(sel);
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 16; ++i) {
            const auto preset = static_cast<UiAnchorPreset>(i);
            math::Vec2 amin, amax, pv;
            ui_anchor_preset_values(preset, amin, amax, pv);
            const bool current = rc && std::abs(rc->anchor_min.x - amin.x) < 1e-4f && std::abs(rc->anchor_min.y - amin.y) < 1e-4f &&
                                 std::abs(rc->anchor_max.x - amax.x) < 1e-4f && std::abs(rc->anchor_max.y - amax.y) < 1e-4f;
            if (i % 4 != 0) ImGui::SameLine();
            ImGui::PushID(i);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 box(52.0f, 40.0f);
            if (ImGui::InvisibleButton("##preset", box) && rc) {
                apply_ui_anchor_preset(sel, preset);
                ImGui::CloseCurrentPopup();
            }
            const bool hot = ImGui::IsItemHovered();
            kit::tooltip(ui_anchor_preset_label(preset));
            // Parent box with the anchored element drawn inside.
            const ImVec2 o(a.x + 6, a.y + 6), e(a.x + box.x - 6, a.y + box.y - 6);
            pdl->AddRectFilled(a, ImVec2(a.x + box.x, a.y + box.y), u32(current ? tk::kSurfaceHover : tk::kSurfaceRaised), 4.0f);
            pdl->AddRect(o, e, u32(hot ? tk::kAccent : tk::kBorder), 2.0f);
            const float bw = e.x - o.x, bh = e.y - o.y;
            const float cw = amin.x == amax.x ? bw * 0.3f : bw - 6.0f, ch = amin.y == amax.y ? bh * 0.35f : bh - 6.0f;
            const float cx = o.x + 3.0f + (bw - 6.0f) * pv.x - cw * pv.x;
            const float cy = o.y + 3.0f + (bh - 6.0f) * pv.y - ch * pv.y;
            pdl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), u32(current || hot ? tk::kAccent : tk::kTextMuted, 0.85f), 2.0f);
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &ui_snap_);
    kit::tooltip("Snap moves and resizes to the grid (hold Alt to place freely)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    ImGui::DragFloat("##uigrid", &ui_grid_, 0.5f, 1.0f, 200.0f, "%.0f");
    ui_grid_ = std::max(ui_grid_, 1.0f);
    kit::tooltip("Grid size in canvas units");
    ImGui::SameLine();
    const bool fit_now = kit::icon_button("##uifit", kit::Icon::Focus, "Fit the canvas in the window (F)");
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::lround(ui_zoom_shown_ * 100.0f)));
    if (!editing) {
        ImGui::SameLine();
        kit::pill("Playing: view only", ui::Tone::Warning);
    }

    // ---- canvas ----
    const float status_h = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 64.0f);
    avail.y = std::max(avail.y - status_h, 64.0f);
    ImGui::InvisibleButton("##uiarea", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                  ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const math::Vec2 ps = ui_rendered_size_.x > 0 ? ui_rendered_size_ : psize;
    const float fit = std::clamp(std::min((avail.x - 32.0f) / ps.x, (avail.y - 32.0f) / ps.y), 0.02f, 4.0f);
    if (fit_now || (focused && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
        ui_zoom_ = 0;
        ui_pan_x_ = ui_pan_y_ = 0;
    }
    float z = ui_zoom_ > 0 ? ui_zoom_ : fit;
    const auto image_origin = [&](float zoom) {
        return ImVec2(origin.x + (avail.x - ps.x * zoom) * 0.5f + ui_pan_x_, origin.y + (avail.y - ps.y * zoom) * 0.5f + ui_pan_y_);
    };
    if (hovered && io.MouseWheel != 0.0f) {
        const ImVec2 i0 = image_origin(z);
        const float nz = std::clamp(z * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 0.02f, 8.0f);
        const float mx = (io.MousePos.x - i0.x) / z, my = (io.MousePos.y - i0.y) / z; // preview px under cursor
        ui_zoom_ = nz;
        ui_pan_x_ = io.MousePos.x - mx * nz - origin.x - (avail.x - ps.x * nz) * 0.5f;
        ui_pan_y_ = io.MousePos.y - my * nz - origin.y - (avail.y - ps.y * nz) * 0.5f;
        z = nz;
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        ui_pan_x_ += io.MouseDelta.x;
        ui_pan_y_ += io.MouseDelta.y;
    }
    ui_zoom_shown_ = z;
    const ImVec2 i0 = image_origin(z), i1(i0.x + ps.x * z, i0.y + ps.y * z);
    const auto to_screen = [&](const Rect& r) { return math::Vec4{i0.x + r.x * z, i0.y + r.y * z, r.w * z, r.h * z}; };
    ui_screen_.clear();
    for (const auto& r : ui_rects_) ui_screen_.emplace_back(r.uuid, to_screen(r.rect));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, u32(tk::kSurfaceBase));
    if (ui_tex_) dl->AddImage(ImTextureRef(ui::UiContext::texture_id(ui_tex_)), i0, i1);
    dl->AddRect(ImVec2(i0.x - 1, i0.y - 1), ImVec2(i1.x + 1, i1.y + 1), u32(tk::kBorder));

    // Hover: the topmost visible element under the cursor.
    const ImVec2 mouse = io.MousePos;
    Uuid hover;
    std::uint32_t best = 0;
    for (std::size_t i = 1; i < ui_rects_.size(); ++i) {
        const auto& r = ui_rects_[i];
        const math::Vec4 v = ui_screen_[i].second;
        if (!r.visible || mouse.x < v.x || mouse.y < v.y || mouse.x >= v.x + v.z || mouse.y >= v.y + v.w) continue;
        if (r.order >= best) {
            best = r.order;
            hover = r.uuid;
        }
    }
    if (!hovered) hover = {};
    const auto screen_of = [&](const Uuid& id) -> std::optional<math::Vec4> {
        for (const auto& [u, v] : ui_screen_)
            if (u == id) return v;
        return std::nullopt;
    };
    // Faint outlines show every element, including empty and transparent ones.
    for (std::size_t i = 1; i < ui_rects_.size(); ++i) {
        if (!ui_rects_[i].visible) continue;
        const math::Vec4 v = ui_screen_[i].second;
        dl->AddRect(ImVec2(v.x, v.y), ImVec2(v.x + v.z, v.y + v.w), u32(tk::kTextDisabled, 0.35f));
    }
    if (!hover.is_nil() && (sel.is_null() || hover != s.uuid(sel)))
        if (const auto v = screen_of(hover))
            dl->AddRect(ImVec2(v->x, v->y), ImVec2(v->x + v->z, v->y + v->w), u32(tk::kAccent, 0.7f), 0.0f, 1.5f);

    // Selected element: outline, parent frame, anchor markers and resize handles.
    std::array<ImVec2, 9> handles{};
    std::optional<math::Vec4> sel_screen;
    if (!sel.is_null()) sel_screen = screen_of(s.uuid(sel));
    if (sel_screen) {
        const math::Vec4 v = *sel_screen;
        const Entity parent = gameui::parent_element(s, sel);
        const auto pv = parent.is_null() ? std::nullopt : screen_of(s.uuid(parent));
        if (pv && !sel_by_layout) {
            dl->AddRect(ImVec2(pv->x, pv->y), ImVec2(pv->x + pv->z, pv->y + pv->w), u32(tk::kWarning, 0.45f));
            const auto* rc = w.get<gameui::UiRectComponent>(sel);
            const float ax[2] = {pv->x + pv->z * rc->anchor_min.x, pv->x + pv->z * rc->anchor_max.x};
            const float ay[2] = {pv->y + pv->w * rc->anchor_min.y, pv->y + pv->w * rc->anchor_max.y};
            for (int yi = 0; yi < 2; ++yi)
                for (int xi = 0; xi < 2; ++xi) {
                    const ImVec2 c(ax[xi], ay[yi]);
                    const float k = 5.0f;
                    dl->AddQuadFilled(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x, c.y + k), ImVec2(c.x - k, c.y), u32(tk::kWarning));
                }
        }
        dl->AddRect(ImVec2(v.x, v.y), ImVec2(v.x + v.z, v.y + v.w), u32(tk::kAccent), 0.0f, 2.0f);
        const float cx = v.x + v.z * 0.5f, cy = v.y + v.w * 0.5f, x1 = v.x + v.z, y1 = v.y + v.w;
        handles = {ImVec2(0, 0),  ImVec2(cx, v.y), ImVec2(x1, v.y), ImVec2(x1, cy),
                   ImVec2(x1, y1), ImVec2(cx, y1), ImVec2(v.x, y1), ImVec2(v.x, cy), ImVec2(v.x, v.y)};
        if (editing)
            for (int i = 1; i <= 8; ++i) {
                const ImVec2 h = handles[static_cast<std::size_t>(i)];
                dl->AddRectFilled(ImVec2(h.x - 4, h.y - 4), ImVec2(h.x + 4, h.y + 4), u32(tk::kText));
                dl->AddRect(ImVec2(h.x - 4, h.y - 4), ImVec2(h.x + 4, h.y + 4), u32(tk::kAccent));
            }
        const std::string name = name_of(s, sel);
        dl->AddText(ImVec2(v.x, v.y - ImGui::GetTextLineHeight() - 3.0f), u32(tk::kAccent), name.c_str());
    }

    // ---- input ----
    const auto handle_at = [&](ImVec2 p) {
        if (!sel_screen || !editing) return -1;
        for (int i = 1; i <= 8; ++i) {
            const ImVec2 h = handles[static_cast<std::size_t>(i)];
            if (std::abs(p.x - h.x) <= 6.0f && std::abs(p.y - h.y) <= 6.0f) return i;
        }
        return -1;
    };
    if (hovered && ui_drag_mode_ < 0) {
        const int h = handle_at(mouse);
        if (h == 1 || h == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        else if (h == 3 || h == 7) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (h == 2 || h == 6) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
        else if (h == 4 || h == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int mode = handle_at(mouse);
        Entity target = mode > 0 ? sel : Entity{};
        if (mode < 0 && sel_screen && mouse.x >= sel_screen->x && mouse.y >= sel_screen->y && mouse.x < sel_screen->x + sel_screen->z &&
            mouse.y < sel_screen->y + sel_screen->w && (hover.is_nil() || hover == s.uuid(sel))) {
            mode = 0;
            target = sel;
        }
        if (mode < 0) {
            if (!hover.is_nil()) {
                target = s.find(hover);
                select(target);
                mode = 0;
            } else {
                selection_ = {s.uuid(canvas)};
            }
        }
        if (editing && mode >= 0 && !target.is_null() && w.has<gameui::UiRectComponent>(target)) {
            const auto start = ui_element_rect(target);
            const Uuid tid = s.uuid(target);
            float scale = 1.0f;
            for (const auto& r : ui_rects_)
                if (r.uuid == tid) scale = r.scale;
            if (start && scale > 0.0f) {
                ui_drag_mode_ = mode;
                ui_drag_live_ = false;
                ui_drag_target_ = tid;
                ui_drag_start_ = *start;
                ui_drag_start_size_ = w.get<gameui::UiRectComponent>(target)->size;
                ui_drag_scale_ = scale;
                ui_drag_before_ = capture(s, {tid});
            }
        }
    }
    if (ui_drag_mode_ >= 0) {
        const Entity e = s.find(ui_drag_target_);
        auto* rc = e.is_null() ? nullptr : w.get<gameui::UiRectComponent>(e);
        if (!rc || !editing) {
            ui_drag_mode_ = -1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (ui_drag_live_) (void)apply_states(s, ui_drag_before_);
            ui_drag_mode_ = -1;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= 9.0f) ui_drag_live_ = true;
            const bool by_layout = gameui::placed_by_layout(s, e);
            if (ui_drag_live_ && !(by_layout && ui_drag_mode_ == 0)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                const float k = 1.0f / (z * ui_drag_scale_);
                const float dx = d.x * k, dy = d.y * k;
                const bool snap = ui_snap_ && !io.KeyAlt;
                const auto sn = [&](float v) { return snap ? snap_to(v, ui_grid_) : v; };
                const int m = ui_drag_mode_;
                const bool north = m == 8 || m == 1 || m == 2, south = m == 4 || m == 5 || m == 6;
                const bool west = m == 6 || m == 7 || m == 8, east = m == 2 || m == 3 || m == 4;
                if (by_layout) {
                    math::Vec2 size = ui_drag_start_size_;
                    if (east) size.x = sn(size.x + dx);
                    if (west) size.x = sn(size.x - dx);
                    if (south) size.y = sn(size.y + dy);
                    if (north) size.y = sn(size.y - dy);
                    rc->size = {std::max(size.x, 0.0f), std::max(size.y, 0.0f)};
                } else if (const auto psz = parent_size_for(s, e, ui_rects_)) {
                    Rect r = ui_drag_start_;
                    if (m == 0) {
                        r.x = sn(r.x + dx);
                        r.y = sn(r.y + dy);
                    } else {
                        float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
                        if (west) x0 = std::min(sn(x0 + dx), x1);
                        if (east) x1 = std::max(sn(x1 + dx), x0);
                        if (north) y0 = std::min(sn(y0 + dy), y1);
                        if (south) y1 = std::max(sn(y1 + dy), y0);
                        r = {x0, y0, x1 - x0, y1 - y0};
                    }
                    gameui::set_local_rect(*rc, *psz, r);
                }
            }
            if (by_layout && ui_drag_mode_ == 0 && ui_drag_live_)
                kit::tooltip("Placed by its parent's Layout: change the order in the Hierarchy, or resize it with the handles");
        } else {
            if (ui_drag_live_) {
                auto now = capture(s, {ui_drag_target_});
                const bool changed = now.size() != ui_drag_before_.size() || (now.size() == 1 && now[0].snapshot != ui_drag_before_[0].snapshot);
                if (changed) record_edit(ui_drag_mode_ == 0 ? "Move UI element" : "Resize UI element", std::move(ui_drag_before_));
            }
            ui_drag_before_.clear();
            ui_drag_mode_ = -1;
        }
    }
    // Arrow keys nudge the selection (Shift: 10 units).
    if (editing && focused && !io.WantTextInput && ui_drag_mode_ < 0 && !sel.is_null() && !sel_by_layout) {
        const float step = io.KeyShift ? 10.0f : 1.0f;
        float dx = 0, dy = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += step;
        if (dx != 0 || dy != 0)
            if (auto r = ui_element_rect(sel)) set_ui_element_rect(sel, {r->x + dx, r->y + dy, r->w, r->h});
    }
    if (ui_rects_.size() <= 1 && editing) {
        const char* hint = "Empty canvas. Use Add to place text, images and buttons.";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, end.y - ts.y - 12.0f), u32(tk::kTextDisabled), hint);
    }
    dl->PopClipRect();

    // ---- status line ----
    if (!sel.is_null()) {
        if (const auto r = ui_element_rect(sel)) {
            const auto* rc = w.get<gameui::UiRectComponent>(sel);
            std::string anchors = "custom anchors";
            for (int i = 0; i < 16; ++i) {
                math::Vec2 amin, amax, pv;
                ui_anchor_preset_values(static_cast<UiAnchorPreset>(i), amin, amax, pv);
                if (std::abs(rc->anchor_min.x - amin.x) < 1e-4f && std::abs(rc->anchor_min.y - amin.y) < 1e-4f &&
                    std::abs(rc->anchor_max.x - amax.x) < 1e-4f && std::abs(rc->anchor_max.y - amax.y) < 1e-4f)
                    anchors = std::string("anchored ") + ui_anchor_preset_label(static_cast<UiAnchorPreset>(i));
            }
            if (sel_by_layout) anchors = "placed by the parent's Layout";
            ImGui::Text("%s   x %.0f  y %.0f   %.0f x %.0f   %s", name_of(s, sel).c_str(), r->x, r->y, r->w, r->h, anchors.c_str());
        }
    } else {
        ImGui::TextDisabled("Click an element to select it; drag to move, drag a handle to resize, arrows nudge.");
    }
    ImGui::End();
}

} // namespace victus::editor
