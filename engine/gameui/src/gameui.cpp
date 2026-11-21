// SPDX-License-Identifier: Apache-2.0
#include "victus/gameui/gameui.hpp"
#include "victus/gameui/localization.hpp"

#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace victus::gameui {

using scene::Entity;
using scene::Scene;

Rect Rect::intersect(const Rect& o) const noexcept {
    const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const float x1 = std::min(x + w, o.x + o.w), y1 = std::min(y + h, o.y + o.h);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

namespace {

struct UiFrame {
    math::Vec2 screen{1280, 720};
    std::vector<ClickEvent> events;
    std::size_t carry_from{0}; // events.size() at the end of the last update
    std::size_t delivered{0};  // events handed to scripts since the last update
    std::vector<Entity> order; // visible elements (and canvases) in draw order
    Entity focus, capture, hovered;
    // Drag-to-scroll in progress: the view, where the pointer went down and the offset then.
    Entity drag_view;
    math::Vec2 drag_start, drag_offset;
    bool dragging{false};
    bool pointer_over{false};
    std::uint32_t modal_order{0}; // elements at or below this order ignore the pointer
    Entity editing;               // text field taking keyboard text
    std::size_t caret{0};         // byte offset into the edited text
    std::uint32_t blink{0};       // updates since the last edit (caret blink phase)
    UiStats stats;
};

UiFrame& frame_of(Scene& scene) {
    if (auto* f = scene.world().resource<UiFrame>()) return *f;
    return scene.world().set_resource<UiFrame>();
}

float canvas_scale(const UiCanvasComponent& c, math::Vec2 screen) {
    if (c.scale_mode == ScaleMode::ConstantPixelSize) return 1.0f;
    const float rw = std::max(c.reference_size.x, 1.0f), rh = std::max(c.reference_size.y, 1.0f);
    const float m = std::clamp(c.match, 0.0f, 1.0f);
    const float lw = std::log2(std::max(screen.x, 1.0f) / rw), lh = std::log2(std::max(screen.y, 1.0f) / rh);
    return std::exp2(lw + (lh - lw) * m);
}

Rect place(const UiRectComponent& r, const Rect& parent, float scale) {
    const auto axis = [scale](float p0, float span, float amin, float amax, float pivot, float pos, float size, float& out_p,
                              float& out_s) {
        const float a0 = p0 + span * amin, a1 = p0 + span * amax;
        out_s = std::max(0.0f, (a1 - a0) + size * scale);
        const float pivot_point = a0 + (a1 - a0) * pivot + pos * scale;
        out_p = pivot_point - pivot * out_s;
    };
    Rect out;
    axis(parent.x, parent.w, r.anchor_min.x, r.anchor_max.x, r.pivot.x, r.position.x, r.size.x, out.x, out.w);
    axis(parent.y, parent.h, r.anchor_min.y, r.anchor_max.y, r.pivot.y, r.position.y, r.size.y, out.y, out.h);
    return out;
}

struct Walker {
    Scene& scene;
    UiFrame& frame;
    Entity canvas;
    float scale{1};
    std::uint32_t next_order{1};

    template <class Fn>
    void for_children(Entity e, Fn&& fn) {
        const auto* h = scene.world().get<scene::Hierarchy>(e);
        for (Entity c = h ? h->first_child : Entity{}; !c.is_null();) {
            const auto* ch = scene.world().get<scene::Hierarchy>(c);
            const Entity next = ch ? ch->next_sibling : Entity{};
            fn(c);
            c = next;
        }
    }

    // Lays out the element children of `parent` (with resolved rect `pr`).
    void children(Entity parent, const Rect& pr, const Rect& clip, bool visible, float opacity) {
        ecs::World& w = scene.world();
        const auto* lay = w.get<UiLayoutComponent>(parent);
        float cursor = lay ? lay->padding * scale : 0.0f;
        for_children(parent, [&](Entity c) {
            if (w.has<UiCanvasComponent>(c)) return; // nested canvases are their own roots
            const auto* r = w.get<UiRectComponent>(c);
            if (!r) { // grouping entity without a rect: transparent
                children(c, pr, clip, visible, opacity);
                return;
            }
            Rect rect;
            if (lay && r->visible) {
                const float pad = lay->padding * scale;
                const bool vertical = lay->direction == LayoutDirection::Vertical;
                const float main = (vertical ? r->size.y : r->size.x) * scale;
                const float cross_span = (vertical ? pr.w : pr.h) - 2 * pad;
                float cross = (vertical ? r->size.x : r->size.y) * scale, cross_pos = pad;
                switch (lay->align) {
                case LayoutAlign::Stretch: cross = std::max(0.0f, cross_span); break;
                case LayoutAlign::Start: break;
                case LayoutAlign::Center: cross_pos = pad + (cross_span - cross) * 0.5f; break;
                case LayoutAlign::End: cross_pos = pad + cross_span - cross; break;
                }
                if (vertical) rect = {pr.x + cross_pos, pr.y + cursor, cross, main};
                else rect = {pr.x + cursor, pr.y + cross_pos, main, cross};
                cursor += main + lay->spacing * scale;
            } else {
                rect = place(*r, pr, scale);
            }
            auto& st = *w.get<UiStateComponent>(c); // added up front by layout(): no structural changes here
            st.rect = rect;
            st.clip = clip;
            st.scale = scale;
            st.canvas = canvas;
            st.visible = visible && r->visible;
            st.opacity = opacity * std::clamp(r->opacity, 0.0f, 1.0f);
            ++frame.stats.elements;
            if (st.visible) {
                st.order = next_order++;
                frame.order.push_back(c);
                ++frame.stats.visible;
                if (w.has<UiButtonComponent>(c)) ++frame.stats.buttons;
                if (w.has<UiSliderComponent>(c) || w.has<UiToggleComponent>(c) || w.has<UiTextInputComponent>(c)) ++frame.stats.widgets;
            } else {
                st.order = 0;
            }
            if (auto* sc = w.get<UiScrollComponent>(c)) {
                scroll_view(c, *sc, st, rect, clip);
                return;
            }
            const Rect child_clip = r->clip_children ? clip.intersect(rect) : clip;
            children(c, rect, child_clip, st.visible, st.opacity);
        });
    }

    void scroll_view(Entity view, UiScrollComponent& sc, UiStateComponent& st, const Rect& rect, const Rect& clip) {
        ecs::World& w = scene.world();
        const Rect child_clip = clip.intersect(rect);
        const math::Vec2 used{sc.horizontal ? std::max(0.0f, sc.offset.x) : 0.0f, sc.vertical ? std::max(0.0f, sc.offset.y) : 0.0f};
        const Rect content{rect.x - used.x * scale, rect.y - used.y * scale, rect.w, rect.h};
        const std::size_t first = frame.order.size();
        children(view, content, child_clip, st.visible, st.opacity);
        // Content extent: the visible descendants' bounding box from the content origin.
        float extent_r = content.x, extent_b = content.y;
        for (std::size_t i = first; i < frame.order.size(); ++i) {
            const auto* ds = w.get<UiStateComponent>(frame.order[i]);
            extent_r = std::max(extent_r, ds->rect.x + ds->rect.w);
            extent_b = std::max(extent_b, ds->rect.y + ds->rect.h);
        }
        if (const auto* lay = w.get<UiLayoutComponent>(view)) {
            extent_r += lay->padding * scale;
            extent_b += lay->padding * scale;
        }
        const float s = std::max(scale, 1e-6f);
        st.scroll_max = {sc.horizontal ? std::max(0.0f, (extent_r - content.x) - rect.w) / s : 0.0f,
                         sc.vertical ? std::max(0.0f, (extent_b - content.y) - rect.h) / s : 0.0f};
        const math::Vec2 clamped{std::clamp(used.x, 0.0f, st.scroll_max.x), std::clamp(used.y, 0.0f, st.scroll_max.y)};
        if (sc.horizontal) sc.offset.x = clamped.x;
        if (sc.vertical) sc.offset.y = clamped.y;
        if (clamped.x != used.x || clamped.y != used.y) // content shrank or offset overshot: move it back now
            shift(view, (used.x - clamped.x) * scale, (used.y - clamped.y) * scale, child_clip);
    }

    // Moves the laid-out descendants of `e` by (dx, dy) pixels; clips inside them follow.
    void shift(Entity e, float dx, float dy, const Rect& view_clip) {
        for_children(e, [&](Entity c) {
            if (scene.world().has<UiCanvasComponent>(c)) return;
            if (auto* ds = scene.world().get<UiStateComponent>(c)) {
                ds->rect.x += dx;
                ds->rect.y += dy;
                if (ds->clip.x != view_clip.x || ds->clip.y != view_clip.y || ds->clip.w != view_clip.w || ds->clip.h != view_clip.h) {
                    ds->clip.x += dx;
                    ds->clip.y += dy;
                    ds->clip = ds->clip.intersect(view_clip);
                }
            }
            shift(c, dx, dy, view_clip);
        });
    }
};

// Buttons, toggles, sliders and text fields take the pointer and keyboard focus.
bool widget_enabled(const ecs::World& w, Entity e) {
    if (const auto* b = w.get<UiButtonComponent>(e)) return b->interactable;
    if (const auto* t = w.get<UiToggleComponent>(e)) return t->interactable;
    if (const auto* sl = w.get<UiSliderComponent>(e)) return sl->interactable;
    if (const auto* ti = w.get<UiTextInputComponent>(e)) return ti->interactable;
    return false;
}

bool is_interactable(Scene& scene, Entity e) {
    const auto* st = scene.world().get<UiStateComponent>(e);
    return st && st->visible && st->opacity > 0.0f && widget_enabled(scene.world(), e);
}

void emit(Scene& scene, Entity e, const std::string& action, UiEventKind kind, float value = 0, std::string text = {}) {
    frame_of(scene).events.push_back({e, action, kind, value, std::move(text)});
}

float snap(const UiSliderComponent& sl, float v) {
    const float lo = std::min(sl.min_value, sl.max_value), hi = std::max(sl.min_value, sl.max_value);
    if (sl.step > 0) v = sl.min_value + std::round((v - sl.min_value) / sl.step) * sl.step;
    return std::clamp(v, lo, hi);
}

bool set_slider(Scene& scene, Entity e, float v) {
    auto* sl = scene.world().get<UiSliderComponent>(e);
    if (!sl) return false;
    v = snap(*sl, v);
    if (v == sl->value) return false;
    sl->value = v;
    emit(scene, e, sl->action, UiEventKind::Changed, v);
    return true;
}

// Slider value under pointer `p` (pixels): the handle centre follows the pointer.
float slider_at(const UiSliderComponent& sl, const UiStateComponent& st, math::Vec2 p) {
    const float hs = sl.handle_size * st.scale;
    float t;
    if (sl.vertical) t = 1.0f - (p.y - st.rect.y - hs * 0.5f) / std::max(st.rect.h - hs, 1.0f);
    else t = (p.x - st.rect.x - hs * 0.5f) / std::max(st.rect.w - hs, 1.0f);
    return sl.min_value + std::clamp(t, 0.0f, 1.0f) * (sl.max_value - sl.min_value);
}

std::size_t codepoints(std::string_view s) {
    std::size_t n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// Applies this frame's typing to the edited field. Returns true when the text changed.
void edit_text(Scene& scene, UiFrame& f, const gameplay::InputSource& in) {
    auto* ti = scene.world().get<UiTextInputComponent>(f.editing);
    if (!ti) return;
    std::string& t = ti->text;
    std::size_t& c = f.caret;
    c = std::min(c, t.size());
    while (c > 0 && c < t.size() && (static_cast<unsigned char>(t[c]) & 0xC0) == 0x80) --c;
    bool changed = false, moved = false;
    const std::string_view typed = in.text_input();
    for (std::size_t i = 0; i < typed.size();) {
        std::size_t len = 1;
        const auto lead = static_cast<unsigned char>(typed[i]);
        if (lead >= 0xF0) len = 4;
        else if (lead >= 0xE0) len = 3;
        else if (lead >= 0xC0) len = 2;
        len = std::min(len, typed.size() - i);
        if (lead >= 0x20 && lead != 0x7F && codepoints(t) < ti->max_length) {
            t.insert(c, typed.substr(i, len));
            c += len;
            changed = true;
        }
        i += len;
    }
    auto prev = [&](std::size_t p) {
        if (p == 0) return p;
        --p;
        while (p > 0 && (static_cast<unsigned char>(t[p]) & 0xC0) == 0x80) --p;
        return p;
    };
    auto next = [&](std::size_t p) {
        if (p >= t.size()) return t.size();
        ++p;
        while (p < t.size() && (static_cast<unsigned char>(t[p]) & 0xC0) == 0x80) ++p;
        return p;
    };
    if (in.key_pressed("Backspace") && c > 0) {
        const std::size_t p = prev(c);
        t.erase(p, c - p);
        c = p;
        changed = true;
    }
    if (in.key_pressed("Delete") && c < t.size()) {
        t.erase(c, next(c) - c);
        changed = true;
    }
    if (in.key_pressed("Left")) c = prev(c), moved = true;
    if (in.key_pressed("Right")) c = next(c), moved = true;
    if (in.key_pressed("Home")) c = 0, moved = true;
    if (in.key_pressed("End")) c = t.size(), moved = true;
    if (changed || moved) f.blink = 0;
    const Entity field = f.editing;
    if (changed) emit(scene, field, ti->action, UiEventKind::Changed, 0, t);
    if (in.key_pressed("Enter")) {
        emit(scene, field, ti->action, UiEventKind::Submitted, 0, t);
        f.editing = {};
    } else if (in.key_pressed("Escape")) {
        f.editing = {};
    }
}

bool parse_hex(std::string_view h, math::Color& out) {
    if (h.size() != 6 && h.size() != 8) return false;
    float v[4]{0, 0, 0, 1};
    for (std::size_t i = 0; i < h.size() / 2; ++i) {
        int byte = 0;
        for (int k = 0; k < 2; ++k) {
            const char ch = h[i * 2 + static_cast<std::size_t>(k)];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return false;
            byte = byte * 16 + d;
        }
        v[i] = static_cast<float>(byte) / 255.0f;
    }
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

bool parse_color_name(std::string_view n, math::Color& out) {
    if (n.size() >= 2 && (n.front() == '"' || n.front() == '\'') && n.back() == n.front()) n = n.substr(1, n.size() - 2);
    if (!n.empty() && n.front() == '#') return parse_hex(n.substr(1), out);
    struct Named {
        std::string_view name;
        math::Color c;
    };
    static constexpr Named kNames[] = {
        {"white", {1, 1, 1, 1}},          {"black", {0, 0, 0, 1}},        {"red", {1, 0.23f, 0.19f, 1}},
        {"green", {0.2f, 0.84f, 0.29f, 1}}, {"blue", {0.25f, 0.5f, 1, 1}}, {"yellow", {1, 0.84f, 0.04f, 1}},
        {"orange", {1, 0.58f, 0, 1}},     {"gray", {0.56f, 0.56f, 0.58f, 1}}, {"grey", {0.56f, 0.56f, 0.58f, 1}},
        {"cyan", {0.39f, 0.82f, 1, 1}},   {"magenta", {0.75f, 0.35f, 0.95f, 1}},
    };
    for (const Named& k : kNames)
        if (k.name == n) {
            out = k.c;
            return true;
        }
    return false;
}

template <class Pred>
Entity pick_where(Scene& scene, const UiFrame& f, math::Vec2 p, Pred&& pred) {
    for (auto it = f.order.rbegin(); it != f.order.rend(); ++it) {
        const auto* st = scene.world().get<UiStateComponent>(*it);
        if (!st || !st->visible || st->order <= f.modal_order) continue;
        if (!st->rect.contains(p) || !st->clip.contains(p)) continue;
        if (pred(*it)) return *it;
    }
    return {};
}

bool blocks_pointer(Scene& scene, Entity e) {
    const ecs::World& w = scene.world();
    return w.has<UiPanelComponent>(e) || w.has<UiImageComponent>(e) || w.has<UiButtonComponent>(e) || w.has<UiSliderComponent>(e) ||
           w.has<UiToggleComponent>(e) || w.has<UiTextInputComponent>(e) || w.has<UiProgressBarComponent>(e);
}

bool scrollable(Scene& scene, Entity e) {
    const auto* sc = scene.world().get<UiScrollComponent>(e);
    const auto* st = scene.world().get<UiStateComponent>(e);
    return sc && st && (st->scroll_max.x > 0 || st->scroll_max.y > 0);
}

bool set_offset(Scene& scene, Entity view, math::Vec2 offset) {
    auto* sc = scene.world().get<UiScrollComponent>(view);
    const auto* st = scene.world().get<UiStateComponent>(view);
    if (!sc || !st) return false;
    const math::Vec2 o{sc->horizontal ? std::clamp(offset.x, 0.0f, st->scroll_max.x) : sc->offset.x,
                       sc->vertical ? std::clamp(offset.y, 0.0f, st->scroll_max.y) : sc->offset.y};
    if (o.x == sc->offset.x && o.y == sc->offset.y) return false;
    sc->offset = o;
    return true;
}

// Mouse wheel over a scroll view and drag-to-scroll. Returns true when an offset changed.
bool scroll_input(Scene& scene, UiFrame& f, const gameplay::InputSource& in, math::Vec2 p) {
    ecs::World& w = scene.world();
    bool changed = false;
    if (const float wheel = in.mouse_wheel(); wheel != 0) {
        const Entity v = pick_where(scene, f, p, [&](Entity e) { return scrollable(scene, e); });
        if (const auto* sc = v.is_null() ? nullptr : w.get<UiScrollComponent>(v)) {
            const math::Vec2 o = sc->offset;
            const bool vertical = sc->vertical && w.get<UiStateComponent>(v)->scroll_max.y > 0;
            changed = set_offset(scene, v, vertical ? math::Vec2{o.x, o.y - wheel * sc->wheel_step}
                                                    : math::Vec2{o.x - wheel * sc->wheel_step, o.y});
        }
    }
    if (in.mouse_pressed(0) && !w.has<UiSliderComponent>(f.capture)) {
        f.drag_view = pick_where(scene, f, p, [&](Entity e) {
            const auto* sc = w.get<UiScrollComponent>(e);
            return sc && sc->drag_to_scroll && scrollable(scene, e);
        });
        f.dragging = false;
        f.drag_start = p;
        if (const auto* sc = f.drag_view.is_null() ? nullptr : w.get<UiScrollComponent>(f.drag_view)) f.drag_offset = sc->offset;
    }
    if (!f.drag_view.is_null()) {
        const auto* st = w.get<UiStateComponent>(f.drag_view);
        if (!in.mouse_down(0) || !st || !w.has<UiScrollComponent>(f.drag_view)) {
            f.drag_view = {};
            f.dragging = false;
        } else {
            const math::Vec2 d{p.x - f.drag_start.x, p.y - f.drag_start.y};
            constexpr float kDragThreshold = 8.0f; // pixels before a press becomes a drag
            if (!f.dragging && d.x * d.x + d.y * d.y > kDragThreshold * kDragThreshold) {
                f.dragging = true;
                f.capture = {}; // the press started a drag, not a click
            }
            if (f.dragging) {
                const float s = std::max(st->scale, 1e-6f);
                changed = set_offset(scene, f.drag_view, {f.drag_offset.x - d.x / s, f.drag_offset.y - d.y / s}) || changed;
            }
        }
    }
    return changed;
}

// Scrolls the innermost scroll view around `e` so `e` is fully visible (when it fits).
bool scroll_into_view(Scene& scene, Entity e) {
    ecs::World& w = scene.world();
    const auto* target = w.get<UiStateComponent>(e);
    if (!target) return false;
    for (Entity a = scene.parent(e); !a.is_null(); a = scene.parent(a)) {
        if (w.has<UiCanvasComponent>(a)) return false;
        const auto* sc = w.get<UiScrollComponent>(a);
        const auto* st = w.get<UiStateComponent>(a);
        if (!sc || !st) continue;
        const float s = std::max(st->scale, 1e-6f);
        math::Vec2 o = sc->offset;
        const Rect& v = st->rect;
        const Rect& t = target->rect;
        if (sc->vertical) {
            if (t.y < v.y) o.y -= (v.y - t.y) / s;
            else if (t.y + t.h > v.y + v.h) o.y += std::min(t.y + t.h - (v.y + v.h), t.y - v.y) / s;
        }
        if (sc->horizontal) {
            if (t.x < v.x) o.x -= (v.x - t.x) / s;
            else if (t.x + t.w > v.x + v.w) o.x += std::min(t.x + t.w - (v.x + v.w), t.x - v.x) / s;
        }
        return set_offset(scene, a, o);
    }
    return false;
}

math::Color with_opacity(math::Color c, float opacity) {
    c.a *= opacity;
    return c;
}

} // namespace

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<UiCanvasComponent>("UiCanvas", "UI")
            .field(&UiCanvasComponent::visible, "visible")
            .enumeration(&UiCanvasComponent::scale_mode, "scale_mode", {"constant_pixel_size", "scale_with_screen"})
            .field(&UiCanvasComponent::reference_size, "reference_size", {.min = 1, .max = 16384, .speed = 1, .tooltip = "Resolution the UI is authored at"})
            .field(&UiCanvasComponent::match, "match", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "0 = scale with width, 1 = with height"})
            .field(&UiCanvasComponent::sort_order, "sort_order", {.min = -1000, .max = 1000, .tooltip = "Higher draws on top"})
            .field(&UiCanvasComponent::keyboard_navigation, "keyboard_navigation", {.tooltip = "Up/Down (W/S) move focus, Enter/Space click"})
            .field(&UiCanvasComponent::blocks_input, "blocks_input", {.tooltip = "Modal: the pointer is always over UI"})
            .commit();
        scene::Schema<UiRectComponent>("UiRect", "UI")
            .field(&UiRectComponent::anchor_min, "anchor_min", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Fraction of the parent (0,0 = top-left)"})
            .field(&UiRectComponent::anchor_max, "anchor_max", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&UiRectComponent::pivot, "pivot", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&UiRectComponent::position, "position", {.speed = 1, .tooltip = "Canvas units, +Y down"})
            .field(&UiRectComponent::size, "size", {.speed = 1, .tooltip = "Canvas units; added to the span on stretched axes"})
            .field(&UiRectComponent::visible, "visible")
            .field(&UiRectComponent::opacity, "opacity", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&UiRectComponent::clip_children, "clip_children")
            .commit();
        scene::Schema<UiPanelComponent>("UiPanel", "UI")
            .field(&UiPanelComponent::color, "color")
            .field(&UiPanelComponent::corner_radius, "corner_radius", {.min = 0, .max = 1000, .speed = 0.5f})
            .field(&UiPanelComponent::border_color, "border_color")
            .field(&UiPanelComponent::border_width, "border_width", {.min = 0, .max = 100, .speed = 0.1f})
            .commit();
        scene::Schema<UiImageComponent>("UiImage", "UI")
            .field(&UiImageComponent::texture, "texture", {.hint = "texture"})
            .field(&UiImageComponent::tint, "tint")
            .field(&UiImageComponent::preserve_aspect, "preserve_aspect")
            .field(&UiImageComponent::corner_radius, "corner_radius", {.min = 0, .max = 1000, .speed = 0.5f})
            .commit();
        scene::Schema<UiTextComponent>("UiText", "UI")
            .field(&UiTextComponent::text, "text")
            .field(&UiTextComponent::key, "key", {.tooltip = "Localization key (assets/localization/<locale>.vstrings); empty = show text"})
            .field(&UiTextComponent::font_size, "font_size", {.min = 1, .max = 1000, .speed = 0.5f})
            .field(&UiTextComponent::color, "color")
            .enumeration(&UiTextComponent::align, "align", {"left", "center", "right"})
            .enumeration(&UiTextComponent::vertical_align, "vertical_align", {"top", "middle", "bottom"})
            .field(&UiTextComponent::wrap, "wrap")
            .field(&UiTextComponent::bold, "bold", {.tooltip = "Built-in font only"})
            .field(&UiTextComponent::shadow_color, "shadow_color")
            .field(&UiTextComponent::shadow_offset, "shadow_offset", {.speed = 0.25f})
            .field(&UiTextComponent::font, "font", {.tooltip = "Font asset path under assets/ (.ttf/.otf); empty = built-in"})
            .field(&UiTextComponent::rich, "rich", {.tooltip = "Markup: <b>bold</b>, <color=#ff8800>colour</color>"})
            .commit();
        scene::Schema<UiButtonComponent>("UiButton", "UI")
            .field(&UiButtonComponent::interactable, "interactable")
            .field(&UiButtonComponent::action, "action", {.tooltip = "Passed to on_click(button, action)"})
            .field(&UiButtonComponent::normal, "normal")
            .field(&UiButtonComponent::hovered, "hovered")
            .field(&UiButtonComponent::pressed, "pressed")
            .field(&UiButtonComponent::disabled, "disabled")
            .commit();
        scene::Schema<UiScrollComponent>("UiScroll", "UI")
            .field(&UiScrollComponent::vertical, "vertical")
            .field(&UiScrollComponent::horizontal, "horizontal")
            .field(&UiScrollComponent::offset, "offset", {.min = 0, .speed = 1, .tooltip = "Canvas units scrolled; clamped to the content"})
            .field(&UiScrollComponent::wheel_step, "wheel_step", {.min = 0, .max = 10000, .speed = 1, .tooltip = "Canvas units per wheel notch"})
            .field(&UiScrollComponent::drag_to_scroll, "drag_to_scroll", {.tooltip = "Dragging the content (touch or mouse) scrolls it"})
            .commit();
        scene::Schema<UiSliderComponent>("UiSlider", "UI")
            .field(&UiSliderComponent::value, "value", {.speed = 0.01f})
            .field(&UiSliderComponent::min_value, "min_value", {.speed = 0.01f})
            .field(&UiSliderComponent::max_value, "max_value", {.speed = 0.01f})
            .field(&UiSliderComponent::step, "step", {.min = 0, .speed = 0.01f, .tooltip = "Snap increment; 0 = continuous"})
            .field(&UiSliderComponent::vertical, "vertical", {.tooltip = "Bottom is the minimum"})
            .field(&UiSliderComponent::interactable, "interactable")
            .field(&UiSliderComponent::action, "action", {.tooltip = "Passed to on_value_changed(slider, action, value)"})
            .field(&UiSliderComponent::track, "track")
            .field(&UiSliderComponent::fill, "fill")
            .field(&UiSliderComponent::handle, "handle")
            .field(&UiSliderComponent::track_thickness, "track_thickness", {.min = 0, .max = 200, .speed = 0.25f})
            .field(&UiSliderComponent::handle_size, "handle_size", {.min = 0, .max = 400, .speed = 0.25f})
            .commit();
        scene::Schema<UiToggleComponent>("UiToggle", "UI")
            .field(&UiToggleComponent::on, "on")
            .field(&UiToggleComponent::interactable, "interactable")
            .field(&UiToggleComponent::action, "action", {.tooltip = "Passed to on_value_changed(toggle, action, on)"})
            .field(&UiToggleComponent::off_color, "off_color")
            .field(&UiToggleComponent::on_color, "on_color")
            .field(&UiToggleComponent::knob, "knob")
            .commit();
        scene::Schema<UiTextInputComponent>("UiTextInput", "UI")
            .field(&UiTextInputComponent::text, "text")
            .field(&UiTextInputComponent::placeholder, "placeholder")
            .field(&UiTextInputComponent::max_length, "max_length", {.min = 1, .max = 100000, .tooltip = "Characters"})
            .field(&UiTextInputComponent::password, "password", {.tooltip = "Show * per character"})
            .field(&UiTextInputComponent::interactable, "interactable")
            .field(&UiTextInputComponent::action, "action", {.tooltip = "Passed to on_value_changed and on_submit"})
            .field(&UiTextInputComponent::font_size, "font_size", {.min = 1, .max = 1000, .speed = 0.5f})
            .field(&UiTextInputComponent::font, "font", {.tooltip = "Font asset path under assets/; empty = built-in"})
            .field(&UiTextInputComponent::color, "color")
            .field(&UiTextInputComponent::placeholder_color, "placeholder_color")
            .field(&UiTextInputComponent::background, "background")
            .field(&UiTextInputComponent::border, "border")
            .field(&UiTextInputComponent::focused_border, "focused_border")
            .field(&UiTextInputComponent::corner_radius, "corner_radius", {.min = 0, .max = 1000, .speed = 0.5f})
            .field(&UiTextInputComponent::padding, "padding", {.min = 0, .max = 1000, .speed = 0.5f})
            .commit();
        scene::Schema<UiProgressBarComponent>("UiProgressBar", "UI")
            .field(&UiProgressBarComponent::value, "value", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&UiProgressBarComponent::vertical, "vertical", {.tooltip = "Fill bottom to top"})
            .field(&UiProgressBarComponent::background, "background")
            .field(&UiProgressBarComponent::fill, "fill")
            .field(&UiProgressBarComponent::corner_radius, "corner_radius", {.min = 0, .max = 1000, .speed = 0.5f})
            .commit();
        scene::Schema<UiLayoutComponent>("UiLayout", "UI")
            .enumeration(&UiLayoutComponent::direction, "direction", {"vertical", "horizontal"})
            .field(&UiLayoutComponent::spacing, "spacing", {.min = 0, .max = 1000, .speed = 0.5f})
            .field(&UiLayoutComponent::padding, "padding", {.min = 0, .max = 1000, .speed = 0.5f})
            .enumeration(&UiLayoutComponent::align, "align", {"start", "center", "end", "stretch"})
            .commit();
    });
}

void set_screen_size(Scene& scene, math::Vec2 size) {
    frame_of(scene).screen = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};
}
math::Vec2 screen_size(Scene& scene) { return frame_of(scene).screen; }

void layout(Scene& scene) {
    VICTUS_PROFILE_SCOPE("ui.layout");
    ecs::World& w = scene.world();
    UiFrame& f = frame_of(scene);
    f.order.clear();
    const std::uint64_t clicks = f.stats.clicks;
    f.stats = {};
    f.stats.clicks = static_cast<std::uint32_t>(clicks);
    f.modal_order = 0;

    // Detached or removed elements lose their state; everything else is recomputed below.
    std::vector<Entity> stale;
    w.each<UiStateComponent>([&](Entity e, UiStateComponent& st) {
        st.visible = false;
        if (!w.has<UiRectComponent>(e) && !w.has<UiCanvasComponent>(e)) stale.push_back(e);
    });
    for (const Entity e : stale) w.remove<UiStateComponent>(e);
    // Add missing state before walking, so component pointers stay valid during the walk.
    std::vector<Entity> missing;
    w.each_excluding<UiRectComponent>(ecs::World::Exclude<UiStateComponent>{}, [&](Entity e, UiRectComponent&) { missing.push_back(e); });
    w.each_excluding<UiCanvasComponent>(ecs::World::Exclude<UiStateComponent>{}, [&](Entity e, UiCanvasComponent&) { missing.push_back(e); });
    for (const Entity e : missing)
        if (!w.has<UiStateComponent>(e)) w.add<UiStateComponent>(e);

    struct Root {
        Entity e;
        std::int32_t sort;
        std::uint32_t index;
    };
    std::vector<Root> roots;
    w.each<UiCanvasComponent>([&](Entity e, UiCanvasComponent& c) {
        roots.push_back({e, c.sort_order, static_cast<std::uint32_t>(roots.size())});
    });
    std::stable_sort(roots.begin(), roots.end(), [](const Root& a, const Root& b) { return a.sort < b.sort; });
    Walker walker{scene, f, {}, 1, 1};
    const Rect screen{0, 0, f.screen.x, f.screen.y};
    for (const Root& r : roots) {
        const auto& c = *w.get<UiCanvasComponent>(r.e);
        auto& st = *w.get<UiStateComponent>(r.e);
        walker.canvas = r.e;
        walker.scale = canvas_scale(c, f.screen);
        st.rect = screen;
        st.clip = screen;
        st.scale = walker.scale;
        st.opacity = 1;
        st.canvas = r.e;
        st.visible = c.visible;
        ++f.stats.canvases;
        if (c.visible) {
            st.order = walker.next_order++;
            f.order.push_back(r.e);
            if (c.blocks_input) f.modal_order = st.order; // only this canvas and those above take the pointer
        } else {
            st.order = 0;
        }
        walker.children(r.e, screen, screen, c.visible, 1.0f);
    }
}

void update(Scene& scene, const gameplay::InputSource* input) {
    VICTUS_PROFILE_SCOPE("ui.update");
    layout(scene);
    ecs::World& w = scene.world();
    UiFrame& f = frame_of(scene);
    // Events from the previous update (or delivered to scripts) are dropped; clicks made later
    // through click() (scripts, tools) survive into this frame's list.
    const std::size_t cut = std::min(f.events.size(), std::max(f.carry_from, f.delivered));
    f.events.erase(f.events.begin(), f.events.begin() + static_cast<std::ptrdiff_t>(cut));
    f.delivered = 0;
    w.each<UiStateComponent>([](Entity, UiStateComponent& st) {
        st.hovered = false;
        st.pressed = false;
        st.focused = false;
    });
    if (!f.focus.is_null() && !is_interactable(scene, f.focus)) f.focus = {};
    if (!f.capture.is_null() && !is_interactable(scene, f.capture)) f.capture = {};
    if (!f.editing.is_null() && (!is_interactable(scene, f.editing) || !w.has<UiTextInputComponent>(f.editing))) f.editing = {};
    ++f.blink;

    Entity hovered;
    const Entity focus_before = f.focus;
    bool scrolled = false;
    f.pointer_over = f.modal_order > 0;
    if (input) {
        const math::Vec2 p = input->mouse_position();
        hovered = pick_where(scene, f, p, [&](Entity e) { return is_interactable(scene, e); });
        if (!f.pointer_over) f.pointer_over = !pick_where(scene, f, p, [&](Entity e) { return blocks_pointer(scene, e); }).is_null();
        if (!hovered.is_null() && hovered != f.hovered) {
            const auto* st = w.get<UiStateComponent>(hovered);
            const auto* c = st ? w.get<UiCanvasComponent>(st->canvas) : nullptr;
            if (c && c->keyboard_navigation) f.focus = hovered;
        }
        if (input->mouse_pressed(0)) {
            if (!f.editing.is_null() && hovered != f.editing) f.editing = {}; // click elsewhere ends editing
            if (!hovered.is_null()) f.capture = hovered;
        }
        scrolled = scroll_input(scene, f, *input, p);
        if (!f.capture.is_null()) {
            const bool slider = w.has<UiSliderComponent>(f.capture);
            if (slider && input->mouse_down(0)) { // dragging: the value follows the pointer anywhere
                (void)set_slider(scene, f.capture, slider_at(*w.get<UiSliderComponent>(f.capture), *w.get<UiStateComponent>(f.capture), p));
                w.get<UiStateComponent>(f.capture)->pressed = true;
            } else if (!input->mouse_down(0)) {
                if (f.capture == hovered && !slider) (void)click(scene, f.capture);
                f.capture = {};
            } else if (f.capture == hovered) {
                w.get<UiStateComponent>(hovered)->pressed = true;
            }
        }

        // Text editing takes the keyboard; otherwise focused sliders step with Left/Right.
        const bool typing = !f.editing.is_null();
        if (typing) edit_text(scene, f, *input);
        if (!typing && !f.focus.is_null()) {
            if (const auto* sl = w.get<UiSliderComponent>(f.focus)) {
                const float step = sl->step > 0 ? sl->step : (sl->max_value - sl->min_value) / 20.0f;
                const int dir = (input->key_pressed("Right") || input->key_pressed("D") ? 1 : 0) -
                                (input->key_pressed("Left") || input->key_pressed("A") ? 1 : 0);
                if (dir != 0) (void)set_slider(scene, f.focus, sl->value + static_cast<float>(dir) * step);
            }
        }

        // Keyboard navigation on the topmost navigable canvas the pointer may reach.
        Entity nav_canvas;
        std::uint32_t nav_order = 0;
        w.each<UiCanvasComponent, UiStateComponent>([&](Entity e, UiCanvasComponent& c, UiStateComponent& st) {
            if (c.visible && c.keyboard_navigation && st.order >= f.modal_order && st.order > nav_order) {
                nav_canvas = e;
                nav_order = st.order;
            }
        });
        if (!nav_canvas.is_null() && !typing) {
            std::vector<Entity> buttons;
            for (const Entity e : f.order) {
                const auto* st = w.get<UiStateComponent>(e);
                if (st && st->canvas == nav_canvas && is_interactable(scene, e)) buttons.push_back(e);
            }
            if (!buttons.empty()) {
                const auto it = std::find(buttons.begin(), buttons.end(), f.focus);
                const bool up = input->key_pressed("Up") || input->key_pressed("W");
                const bool down = input->key_pressed("Down") || input->key_pressed("S");
                if (it == buttons.end()) {
                    if (up) f.focus = buttons.back();
                    else if (down) f.focus = buttons.front();
                } else if (up) {
                    f.focus = it == buttons.begin() ? buttons.back() : *(it - 1);
                } else if (down) {
                    f.focus = it + 1 == buttons.end() ? buttons.front() : *(it + 1);
                }
                if (!f.focus.is_null() && (input->key_pressed("Enter") || input->key_pressed("Space")))
                    (void)click(scene, f.focus);
            }
        }
    }
    if (f.focus != focus_before && !f.focus.is_null()) scrolled = scroll_into_view(scene, f.focus) || scrolled;
    if (scrolled) {
        layout(scene); // draw this frame's positions; hover follows the moved content next update
        if (!hovered.is_null() && !is_interactable(scene, hovered)) hovered = {};
    }
    f.hovered = hovered;
    if (!hovered.is_null()) w.get<UiStateComponent>(hovered)->hovered = true;
    if (!f.focus.is_null()) w.get<UiStateComponent>(f.focus)->focused = true;
    f.carry_from = f.events.size();
}

void add_ui_systems(gameplay::Simulation& sim) {
    register_components();
    const gameplay::Simulation* s = &sim;
    sim.add_system("ui.update", gameplay::Phase::PreUpdate, [s](Scene& scene, const gameplay::FrameTime&) { update(scene, s->input); });
}

const std::vector<ClickEvent>& events(Scene& scene) { return frame_of(scene).events; }
void mark_events_delivered(Scene& scene) {
    UiFrame& f = frame_of(scene);
    f.delivered = f.events.size();
}
const UiStats& stats(Scene& scene) { return frame_of(scene).stats; }
bool pointer_over_ui(Scene& scene) { return frame_of(scene).pointer_over; }

bool scroll_to(Scene& scene, Entity view, math::Vec2 offset) {
    auto* sc = scene.world().get<UiScrollComponent>(view);
    if (!sc) return false;
    sc->offset = {std::max(0.0f, offset.x), std::max(0.0f, offset.y)};
    layout(scene); // clamps to the range and moves the content
    return true;
}

Entity pick(Scene& scene, math::Vec2 point, bool buttons_only) {
    const UiFrame& f = frame_of(scene);
    return pick_where(scene, f, point, [&](Entity e) {
        return buttons_only ? is_interactable(scene, e) : scene.world().has<UiRectComponent>(e);
    });
}

Entity focused(Scene& scene) { return frame_of(scene).focus; }

void focus(Scene& scene, Entity button) {
    UiFrame& f = frame_of(scene);
    f.focus = button.is_null() || is_interactable(scene, button) ? button : Entity{};
}

bool click(Scene& scene, Entity button) {
    if (!scene.world().alive(button) || !is_interactable(scene, button)) return false;
    ecs::World& w = scene.world();
    UiFrame& f = frame_of(scene);
    if (auto* t = w.get<UiToggleComponent>(button); t && !w.has<UiButtonComponent>(button)) {
        t->on = !t->on;
        emit(scene, button, t->action, UiEventKind::Changed, t->on ? 1.0f : 0.0f);
    } else if (w.has<UiTextInputComponent>(button) && !w.has<UiButtonComponent>(button)) {
        return begin_editing(scene, button);
    } else if (const auto* b = w.get<UiButtonComponent>(button)) {
        f.events.push_back({button, b->action});
    } else {
        return false; // sliders have no click
    }
    ++w.get<UiStateComponent>(button)->clicks;
    ++f.stats.clicks;
    return true;
}

bool set_value(Scene& scene, Entity widget, float value) {
    ecs::World& w = scene.world();
    if (!w.alive(widget)) return false;
    if (w.has<UiSliderComponent>(widget)) return set_slider(scene, widget, value);
    if (auto* t = w.get<UiToggleComponent>(widget)) {
        const bool on = value != 0.0f;
        if (t->on == on) return false;
        t->on = on;
        emit(scene, widget, t->action, UiEventKind::Changed, on ? 1.0f : 0.0f);
        return true;
    }
    return false;
}

Entity editing(Scene& scene) { return frame_of(scene).editing; }

bool begin_editing(Scene& scene, Entity field) {
    const auto* ti = scene.world().alive(field) ? scene.world().get<UiTextInputComponent>(field) : nullptr;
    if (!ti || !is_interactable(scene, field)) return false;
    UiFrame& f = frame_of(scene);
    f.editing = field;
    f.focus = field;
    f.caret = ti->text.size();
    f.blink = 0;
    return true;
}

void end_editing(Scene& scene) { frame_of(scene).editing = {}; }

std::vector<TextRun> parse_rich_text(std::string_view text, math::Color base, bool bold, float opacity) {
    std::vector<TextRun> runs;
    std::vector<math::Color> colors{base};
    const int bold_floor = bold ? 1 : 0;
    int bold_depth = bold_floor;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        if (!runs.empty() && runs.back().bold == (bold_depth > 0) && runs.back().color.r == colors.back().r &&
            runs.back().color.g == colors.back().g && runs.back().color.b == colors.back().b && runs.back().color.a == colors.back().a)
            runs.back().text += cur;
        else runs.push_back({cur, colors.back(), bold_depth > 0});
        cur.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const std::size_t j = text.find('>', i);
            if (j != std::string_view::npos) {
                const std::string_view tag = text.substr(i + 1, j - i - 1);
                math::Color c;
                bool handled = true;
                if (tag == "b") flush(), ++bold_depth;
                else if (tag == "/b") flush(), bold_depth = std::max(bold_floor, bold_depth - 1);
                else if (tag == "/color") {
                    flush();
                    if (colors.size() > 1) colors.pop_back();
                } else if (tag.size() > 6 && tag.substr(0, 6) == "color=" && parse_color_name(tag.substr(6), c)) {
                    flush();
                    c.a *= opacity;
                    colors.push_back(c);
                } else handled = false;
                if (handled) {
                    i = j + 1;
                    continue;
                }
            }
        }
        cur += text[i++];
    }
    flush();
    return runs;
}

void build_draw_list(Scene& scene, std::vector<DrawItem>& out) {
    out.clear();
    ecs::World& w = scene.world();
    const UiFrame& f = frame_of(scene);
    const Localization* loc = localization_of(scene);
    for (const Entity e : f.order) {
        const auto* st = w.get<UiStateComponent>(e);
        if (!st || !st->visible || st->opacity <= 0.0f || w.has<UiCanvasComponent>(e)) continue;
        if (st->clip.w <= 0 || st->clip.h <= 0) continue;
        const auto* panel = w.get<UiPanelComponent>(e);
        const auto* button = w.get<UiButtonComponent>(e);
        if (panel || button) {
            DrawItem d;
            d.kind = DrawItem::Kind::Rect;
            d.entity = e;
            d.rect = st->rect;
            d.clip = st->clip;
            d.corner_radius = (panel ? panel->corner_radius : 10.0f) * st->scale;
            if (panel) {
                d.border_color = with_opacity(panel->border_color, st->opacity);
                d.border_width = panel->border_width * st->scale;
            }
            math::Color c = panel ? panel->color : math::Color{};
            if (button) {
                if (!button->interactable) c = button->disabled;
                else if (st->pressed) c = button->pressed;
                else if (st->hovered || st->focused) c = button->hovered;
                else c = button->normal;
            }
            d.color = with_opacity(c, st->opacity);
            if (d.color.a > 0 || (d.border_width > 0 && d.border_color.a > 0)) out.push_back(std::move(d));
        }
        auto rect_item = [&](Rect r, math::Color c, float radius, math::Color border = {}, float border_width = 0) {
            DrawItem d;
            d.kind = DrawItem::Kind::Rect;
            d.entity = e;
            d.rect = r;
            d.clip = st->clip;
            d.color = with_opacity(c, st->opacity);
            d.corner_radius = radius;
            d.border_color = with_opacity(border, st->opacity);
            d.border_width = border_width;
            if (d.color.a > 0 || (d.border_width > 0 && d.border_color.a > 0)) out.push_back(std::move(d));
        };
        const Rect& R = st->rect;
        const bool emphasised = st->hovered || st->focused || st->pressed;
        if (const auto* pb = w.get<UiProgressBarComponent>(e)) {
            const float v = std::clamp(pb->value, 0.0f, 1.0f), r = pb->corner_radius * st->scale;
            rect_item(R, pb->background, r);
            if (v > 0) rect_item(pb->vertical ? Rect{R.x, R.y + R.h * (1 - v), R.w, R.h * v} : Rect{R.x, R.y, R.w * v, R.h}, pb->fill, r);
        }
        if (const auto* sl = w.get<UiSliderComponent>(e)) {
            const float range = sl->max_value - sl->min_value;
            const float t = range != 0 ? std::clamp((sl->value - sl->min_value) / range, 0.0f, 1.0f) : 0.0f;
            const float hs = sl->handle_size * st->scale, th = sl->track_thickness * st->scale;
            const float alpha = sl->interactable ? 1.0f : 0.45f;
            auto dim = [&](math::Color c) { return math::Color{c.r, c.g, c.b, c.a * alpha}; };
            if (sl->vertical) {
                const float cx = R.x + R.w * 0.5f, hy = R.y + hs * 0.5f + (1 - t) * std::max(R.h - hs, 0.0f);
                rect_item({cx - th * 0.5f, R.y + hs * 0.5f, th, std::max(R.h - hs, 0.0f)}, dim(sl->track), th * 0.5f);
                rect_item({cx - th * 0.5f, hy, th, R.y + R.h - hs * 0.5f - hy}, dim(sl->fill), th * 0.5f);
                rect_item({cx - hs * 0.5f, hy - hs * 0.5f, hs, hs}, dim(sl->handle), hs * 0.5f, sl->fill, emphasised ? 2 * st->scale : 0);
            } else {
                const float cy = R.y + R.h * 0.5f, hx = R.x + hs * 0.5f + t * std::max(R.w - hs, 0.0f);
                rect_item({R.x + hs * 0.5f, cy - th * 0.5f, std::max(R.w - hs, 0.0f), th}, dim(sl->track), th * 0.5f);
                rect_item({R.x + hs * 0.5f, cy - th * 0.5f, hx - (R.x + hs * 0.5f), th}, dim(sl->fill), th * 0.5f);
                rect_item({hx - hs * 0.5f, cy - hs * 0.5f, hs, hs}, dim(sl->handle), hs * 0.5f, sl->fill, emphasised ? 2 * st->scale : 0);
            }
        }
        if (const auto* tg = w.get<UiToggleComponent>(e)) {
            const float alpha = tg->interactable ? 1.0f : 0.45f;
            math::Color track = tg->on ? tg->on_color : tg->off_color;
            track.a *= alpha;
            rect_item(R, track, R.h * 0.5f, math::Color{1, 1, 1, 0.6f}, emphasised ? 2 * st->scale : 0);
            const float inset = std::max(2.0f, R.h * 0.1f), k = R.h - 2 * inset;
            math::Color knob = tg->knob;
            knob.a *= alpha;
            rect_item({tg->on ? R.x + R.w - inset - k : R.x + inset, R.y + inset, k, k}, knob, k * 0.5f);
        }
        if (const auto* ti = w.get<UiTextInputComponent>(e)) {
            const bool is_editing = f.editing == e;
            rect_item(R, ti->background, ti->corner_radius * st->scale, is_editing || st->focused ? ti->focused_border : ti->border,
                      (is_editing ? 2.0f : 1.0f) * st->scale);
            const float pad = ti->padding * st->scale;
            DrawItem d;
            d.kind = DrawItem::Kind::Text;
            d.entity = e;
            d.rect = {R.x + pad, R.y, std::max(0.0f, R.w - 2 * pad), R.h};
            d.clip = st->clip.intersect(d.rect);
            d.font = ti->font;
            d.font_size = std::max(1.0f, ti->font_size * st->scale);
            d.align = TextAlign::Left;
            d.vertical_align = VerticalAlign::Middle;
            d.shadow_color = {0, 0, 0, 0};
            d.opacity = st->opacity;
            d.scroll_to_caret = true;
            const std::size_t caret = std::min(f.caret, ti->text.size());
            if (ti->text.empty()) {
                d.text = is_editing ? std::string{} : ti->placeholder;
                d.color = with_opacity(ti->placeholder_color, st->opacity);
                if (is_editing) d.color = with_opacity(ti->color, st->opacity);
            } else if (ti->password) {
                d.text.assign(codepoints(ti->text), '*');
                d.color = with_opacity(ti->color, st->opacity);
            } else {
                d.text = ti->text;
                d.color = with_opacity(ti->color, st->opacity);
            }
            if (is_editing && (f.blink / 30) % 2 == 0)
                d.caret = static_cast<int>(ti->password ? codepoints(std::string_view(ti->text).substr(0, caret)) : caret);
            if ((!d.text.empty() || d.caret >= 0) && d.color.a > 0) out.push_back(std::move(d));
        }
        if (const auto* img = w.get<UiImageComponent>(e)) {
            DrawItem d;
            d.kind = DrawItem::Kind::Image;
            d.entity = e;
            d.rect = st->rect;
            d.clip = st->clip;
            d.color = with_opacity(img->tint, st->opacity);
            d.texture = img->texture;
            d.preserve_aspect = img->preserve_aspect;
            d.corner_radius = img->corner_radius * st->scale;
            if (d.color.a > 0) out.push_back(std::move(d));
        }
        const UiTextComponent* txt = w.get<UiTextComponent>(e);
        const std::string* shown = txt ? &txt->text : nullptr;
        if (txt && !txt->key.empty() && loc)
            if (const std::string* translated = loc->find(txt->key)) shown = translated;
        if (txt && !shown->empty()) {
            DrawItem d;
            d.kind = DrawItem::Kind::Text;
            d.entity = e;
            d.rect = st->rect;
            d.clip = st->clip;
            d.color = with_opacity(txt->color, st->opacity);
            d.text = *shown;
            d.font = txt->font;
            d.font_size = std::max(1.0f, txt->font_size * st->scale);
            d.align = txt->align;
            d.vertical_align = txt->vertical_align;
            d.wrap = txt->wrap;
            d.bold = txt->bold;
            d.shadow_color = with_opacity(txt->shadow_color, st->opacity);
            d.shadow_offset = {txt->shadow_offset.x * st->scale, txt->shadow_offset.y * st->scale};
            d.rich = txt->rich;
            d.opacity = st->opacity;
            if (d.color.a > 0 || d.rich) out.push_back(std::move(d));
        }
    }
}

// ---- authoring helpers -------------------------------------------------------------------------------

Rect local_rect(const UiRectComponent& r, math::Vec2 parent_size) noexcept {
    return place(r, {0, 0, parent_size.x, parent_size.y}, 1.0f);
}

void set_local_rect(UiRectComponent& r, math::Vec2 parent_size, const Rect& rect) noexcept {
    // Inverse of place() at scale 1: width = span + size, pivot point = a0 + span * pivot + position.
    const auto axis = [](float span_total, float amin, float amax, float pivot, float x, float w, float& pos, float& size) {
        w = std::max(0.0f, w);
        const float a0 = span_total * amin, span = span_total * (amax - amin);
        size = w - span;
        pos = (x + pivot * w) - (a0 + span * pivot);
    };
    axis(parent_size.x, r.anchor_min.x, r.anchor_max.x, r.pivot.x, rect.x, rect.w, r.position.x, r.size.x);
    axis(parent_size.y, r.anchor_min.y, r.anchor_max.y, r.pivot.y, rect.y, rect.h, r.position.y, r.size.y);
}

void set_anchors(UiRectComponent& r, math::Vec2 parent_size, math::Vec2 anchor_min, math::Vec2 anchor_max,
                 math::Vec2 pivot) noexcept {
    const Rect keep = local_rect(r, parent_size);
    const auto c01 = [](math::Vec2 v) { return math::Vec2{std::clamp(v.x, 0.0f, 1.0f), std::clamp(v.y, 0.0f, 1.0f)}; };
    r.anchor_min = c01(anchor_min);
    r.anchor_max = c01(anchor_max);
    r.anchor_max = {std::max(r.anchor_max.x, r.anchor_min.x), std::max(r.anchor_max.y, r.anchor_min.y)};
    r.pivot = c01(pivot);
    set_local_rect(r, parent_size, keep);
}

Entity canvas_of(const Scene& scene, Entity e) {
    const ecs::World& w = scene.world();
    for (Entity c = e; !c.is_null() && w.alive(c);) {
        if (w.has<UiCanvasComponent>(c)) return c;
        const auto* h = w.get<scene::Hierarchy>(c);
        c = h ? h->parent : Entity{};
    }
    return {};
}

Entity parent_element(const Scene& scene, Entity e) {
    const ecs::World& w = scene.world();
    if (e.is_null() || !w.alive(e) || w.has<UiCanvasComponent>(e)) return {};
    const auto* h = w.get<scene::Hierarchy>(e);
    for (Entity c = h ? h->parent : Entity{}; !c.is_null();) {
        if (w.has<UiRectComponent>(c) || w.has<UiCanvasComponent>(c)) return c;
        const auto* ch = w.get<scene::Hierarchy>(c);
        c = ch ? ch->parent : Entity{};
    }
    return {};
}

bool placed_by_layout(const Scene& scene, Entity e) {
    const Entity p = parent_element(scene, e);
    return !p.is_null() && !scene.world().has<UiCanvasComponent>(p) && scene.world().has<UiLayoutComponent>(p) &&
           scene.world().has<UiRectComponent>(e);
}

std::optional<math::Vec2> parent_size(Scene& scene, Entity e) {
    const Entity p = parent_element(scene, e);
    if (p.is_null()) return std::nullopt;
    const auto* st = scene.world().get<UiStateComponent>(p);
    if (!st || st->scale <= 0.0f) return std::nullopt;
    return math::Vec2{st->rect.w / st->scale, st->rect.h / st->scale};
}

} // namespace victus::gameui
