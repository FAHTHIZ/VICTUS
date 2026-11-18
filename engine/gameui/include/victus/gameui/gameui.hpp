// SPDX-License-Identifier: Apache-2.0
// In-game UI: HUDs, menus and buttons built from scene entities.
//
// A `UiCanvas` entity is the root of a screen-space UI tree; its descendants with a `UiRect`
// are UI elements laid out relative to their parent element (anchors + pivot + offset + size,
// like a RectTransform). Visual components (`UiPanel`, `UiImage`, `UiText`) and the interactive
// `UiButton` sit on the same entities, and `UiLayout` stacks an element's children vertically
// or horizontally. Canvases scale from a reference resolution, so a HUD authored at 1280x720
// keeps its proportions on any screen.
//
// The module is headless: layout, pointer/keyboard interaction and click events run without a
// GPU (tests, servers), and `build_draw_list` produces renderer-independent draw items that the
// runtime rasterizes on top of the frame. Coordinates are in target pixels with the origin at
// the top-left and +Y down.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/math.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace victus::gameui {

enum class ScaleMode : std::uint8_t { ConstantPixelSize, ScaleWithScreen };
enum class TextAlign : std::uint8_t { Left, Center, Right };
enum class VerticalAlign : std::uint8_t { Top, Middle, Bottom };
enum class LayoutDirection : std::uint8_t { Vertical, Horizontal };
enum class LayoutAlign : std::uint8_t { Start, Center, End, Stretch };

struct UiCanvasComponent {
    bool visible{true};
    ScaleMode scale_mode{ScaleMode::ScaleWithScreen};
    math::Vec2 reference_size{1280, 720}; // ScaleWithScreen: the resolution the UI is authored at
    float match{1.0f};                     // 0 = scale with width, 1 = with height (log blend between)
    std::int32_t sort_order{0};            // higher canvases draw on top and get input first
    bool keyboard_navigation{false};       // Up/Down (W/S) move focus, Enter/Space click
    bool blocks_input{false};              // modal: reports the pointer as over UI everywhere
};

// Element rectangle relative to the parent element (the canvas for top-level elements), in
// canvas units. Anchors are fractions of the parent (0,0 top-left .. 1,1 bottom-right). Along
// an axis where anchor_min == anchor_max the element is `size` wide, positioned so its `pivot`
// sits at the anchor point plus `position`; where they differ it stretches between the anchor
// points, with `size` added to the span and `position` shifting it.
struct UiRectComponent {
    math::Vec2 anchor_min{0.5f, 0.5f};
    math::Vec2 anchor_max{0.5f, 0.5f};
    math::Vec2 pivot{0.5f, 0.5f};
    math::Vec2 position{0, 0};
    math::Vec2 size{200, 60};
    bool visible{true};   // false hides the element and its children
    float opacity{1.0f};  // multiplied down the tree
    bool clip_children{false};
};

// Filled rounded rectangle with an optional border.
struct UiPanelComponent {
    math::Color color{0.06f, 0.07f, 0.1f, 0.75f};
    float corner_radius{10};
    math::Color border_color{1, 1, 1, 0.15f};
    float border_width{0};
};

struct UiImageComponent {
    Uuid texture;            // texture asset; nil draws a solid tint
    math::Color tint{1, 1, 1, 1};
    bool preserve_aspect{true};
    float corner_radius{0};
};

struct UiTextComponent {
    std::string text{"Text"};
    // Localization key (e.g. "menu.start"): when set and the scene's string tables have it, the
    // translation is shown instead of `text`, which stays the fallback shown in the editor.
    std::string key;
    float font_size{28};             // canvas units
    math::Color color{1, 1, 1, 1};
    TextAlign align{TextAlign::Center};
    VerticalAlign vertical_align{VerticalAlign::Middle};
    bool wrap{false};
    bool bold{false};                // built-in font weight (Inter Medium / Bold)
    math::Color shadow_color{0, 0, 0, 0.45f}; // alpha 0 = no shadow
    math::Vec2 shadow_offset{0, 2};
    std::string font;                // project font asset (.ttf/.otf under assets/); empty = built-in
};

// Clickable element: draws its background in the state colour (keeping a UiPanel's radius and
// border) and sends `on_click(button, action)` to scripts on the button and its ancestors.
struct UiButtonComponent {
    bool interactable{true};
    std::string action;
    math::Color normal{0.16f, 0.2f, 0.3f, 0.92f};
    math::Color hovered{0.24f, 0.32f, 0.5f, 0.95f};
    math::Color pressed{0.1f, 0.13f, 0.2f, 1.0f};
    math::Color disabled{0.2f, 0.2f, 0.22f, 0.5f};
};

// Arranges the element's visible child elements in order along one axis, starting at the
// padding; their anchors and positions are ignored (their size along the axis is kept).
struct UiLayoutComponent {
    LayoutDirection direction{LayoutDirection::Vertical};
    float spacing{8};
    float padding{12};
    LayoutAlign align{LayoutAlign::Stretch}; // cross-axis placement
};

// Scroll view: the element's children are laid out in a content area that moves inside the
// element's rect, which always clips them. The content extent is the bounding box of the visible
// descendants (plus UiLayout padding), so the scroll range follows the content. The mouse wheel
// over the view, a drag that starts on it (touch or mouse) and keyboard focus moving to a button
// inside it scroll it; a drag past a few pixels cancels the button press it started on.
struct UiScrollComponent {
    bool vertical{true};
    bool horizontal{false};
    math::Vec2 offset{0, 0};   // canvas units scrolled from the top-left; layout clamps it to the range
    float wheel_step{60};      // canvas units per wheel notch
    bool drag_to_scroll{true};
};

struct Rect {
    float x{0}, y{0}, w{0}, h{0};
    [[nodiscard]] bool contains(math::Vec2 p) const noexcept { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    [[nodiscard]] Rect intersect(const Rect& o) const noexcept;
};

// Runtime state written by layout/update (not serialized).
struct UiStateComponent {
    Rect rect;             // target pixels
    Rect clip;             // visible area after ancestor clipping
    float scale{1};        // canvas units -> pixels
    float opacity{1};      // effective
    bool visible{false};   // effective (canvas and every ancestor visible)
    bool hovered{false};
    bool pressed{false};
    bool focused{false};
    std::uint32_t order{0}; // draw order (higher on top)
    scene::Entity canvas;   // owning canvas
    std::uint64_t clicks{0};
    math::Vec2 scroll_max{0, 0}; // scroll views: largest offset per axis (canvas units)
};

struct ClickEvent {
    scene::Entity button;
    std::string action;
};

struct DrawItem {
    enum class Kind : std::uint8_t { Rect, Image, Text };
    Kind kind{Kind::Rect};
    scene::Entity entity;
    Rect rect;
    Rect clip;
    math::Color color;
    float corner_radius{0};
    math::Color border_color;
    float border_width{0};
    Uuid texture;               // Image
    bool preserve_aspect{false};
    std::string text;           // Text
    std::string font;
    float font_size{0};         // pixels
    TextAlign align{TextAlign::Center};
    VerticalAlign vertical_align{VerticalAlign::Middle};
    bool wrap{false};
    bool bold{false};
    math::Color shadow_color;
    math::Vec2 shadow_offset;   // pixels
};

struct UiStats {
    std::uint32_t canvases{0}, elements{0}, visible{0}, buttons{0}, clicks{0};
};

// Registers "UiCanvas", "UiRect", "UiPanel", "UiImage", "UiText", "UiButton", "UiLayout"
// (inspector category "UI"). Idempotent.
void register_components();

// Size of the target the UI is laid out for (pixels). The renderer sets it every frame; headless
// hosts set it themselves. Defaults to 1280x720.
void set_screen_size(scene::Scene& scene, math::Vec2 size);
[[nodiscard]] math::Vec2 screen_size(scene::Scene& scene);

// Computes every element's UiState rect, scale, opacity, visibility and order.
void layout(scene::Scene& scene);
// layout() + pointer and keyboard interaction; replaces the click events of the previous update.
void update(scene::Scene& scene, const gameplay::InputSource* input);
// Installs "ui.update" (Phase::PreUpdate: click events reach scripts in the same frame).
void add_ui_systems(gameplay::Simulation& sim);

// Clicks since the last update: those it produced plus any made afterwards through click() that
// were not yet delivered. Each event is reported by exactly one update.
[[nodiscard]] const std::vector<ClickEvent>& events(scene::Scene& scene);
// Called by event consumers (the script host) after handling events(): those are not repeated.
void mark_events_delivered(scene::Scene& scene);
[[nodiscard]] const UiStats& stats(scene::Scene& scene);
// True when the pointer was over a visible panel, image or button (or a modal canvas) in the
// last update: games use it to ignore clicks meant for the UI.
[[nodiscard]] bool pointer_over_ui(scene::Scene& scene);
// Topmost visible element under `point` (pixels, last layout); `buttons_only` skips non-buttons.
[[nodiscard]] scene::Entity pick(scene::Scene& scene, math::Vec2 point, bool buttons_only = false);
[[nodiscard]] scene::Entity focused(scene::Scene& scene);
void focus(scene::Scene& scene, scene::Entity button);
// Clicks a button as if the user did (keyboard activation, scripts, tests). False when it is
// not a visible, interactable button.
bool click(scene::Scene& scene, scene::Entity button);

// Draw items for every visible element in draw order (requires a current layout()).
void build_draw_list(scene::Scene& scene, std::vector<DrawItem>& out);

// Sets a scroll view's offset (canvas units, clamped at the next layout) and lays out again.
// False when `view` has no UiScroll.
bool scroll_to(scene::Scene& scene, scene::Entity view, math::Vec2 offset);

} // namespace victus::gameui
