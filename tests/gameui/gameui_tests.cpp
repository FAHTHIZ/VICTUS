// SPDX-License-Identifier: Apache-2.0
// In-game UI tests (Slice 15): layout, scaling, visibility, interaction, navigation, draw lists.
#include "victus/gameui/gameui.hpp"
#include "victus/core/json.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

using namespace victus;
using namespace victus::gameui;
using math::Vec2;
using scene::Entity;
using scene::Scene;

namespace {

Entity canvas(Scene& s, const char* name = "Canvas") {
    const Entity e = s.create(name);
    s.world().add<UiCanvasComponent>(e);
    return e;
}

Entity element(Scene& s, Entity parent, const char* name, UiRectComponent r) {
    const Entity e = s.create(name, parent);
    s.world().add<UiRectComponent>(e, r);
    return e;
}

UiRectComponent corner(Vec2 anchor, Vec2 pivot, Vec2 pos, Vec2 size) {
    UiRectComponent r;
    r.anchor_min = r.anchor_max = anchor;
    r.pivot = pivot;
    r.position = pos;
    r.size = size;
    return r;
}

const Rect& rect(Scene& s, Entity e) { return s.world().get<UiStateComponent>(e)->rect; }

void check_rect(const Rect& r, float x, float y, float w, float h) {
    CHECK(r.x == doctest::Approx(x));
    CHECK(r.y == doctest::Approx(y));
    CHECK(r.w == doctest::Approx(w));
    CHECK(r.h == doctest::Approx(h));
}

Entity button(Scene& s, Entity parent, const char* name, UiRectComponent r, const char* action) {
    const Entity e = element(s, parent, name, r);
    s.world().add<UiButtonComponent>(e).action = action;
    return e;
}

} // namespace

TEST_CASE("ui: anchors, pivots, stretching and nesting at the reference resolution") {
    register_components();
    Scene s;
    set_screen_size(s, {1280, 720});
    const Entity c = canvas(s);
    // Top-left corner, 20 px in.
    const Entity tl = element(s, c, "tl", corner({0, 0}, {0, 0}, {20, 20}, {200, 50}));
    // Bottom-right, pivot at its own bottom-right.
    const Entity br = element(s, c, "br", corner({1, 1}, {1, 1}, {-10, -10}, {100, 40}));
    // Centred.
    const Entity mid = element(s, c, "mid", corner({0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {300, 100}));
    // Full-width bar at the top, 60 high, 16 px margins (size.x is added to the span).
    UiRectComponent bar;
    bar.anchor_min = {0, 0};
    bar.anchor_max = {1, 0};
    bar.pivot = {0.5f, 0};
    bar.position = {0, 0};
    bar.size = {-32, 60};
    const Entity top = element(s, c, "bar", bar);
    // Child stretched over its parent with 5 px insets.
    UiRectComponent fill;
    fill.anchor_min = {0, 0};
    fill.anchor_max = {1, 1};
    fill.size = {-10, -10};
    const Entity inner = element(s, mid, "inner", fill);
    layout(s);
    check_rect(rect(s, tl), 20, 20, 200, 50);
    check_rect(rect(s, br), 1280 - 10 - 100, 720 - 10 - 40, 100, 40);
    check_rect(rect(s, mid), 490, 310, 300, 100);
    check_rect(rect(s, top), 16, 0, 1248, 60);
    check_rect(rect(s, inner), 495, 315, 290, 90);
    CHECK(stats(s).canvases == 1);
    CHECK(stats(s).elements == 5);
    CHECK(stats(s).visible == 5);
}

TEST_CASE("ui: canvases scale with the screen by width, height or a blend") {
    register_components();
    Scene s;
    const Entity c = canvas(s);
    const Entity e = element(s, c, "e", corner({0, 0}, {0, 0}, {10, 10}, {100, 50}));
    set_screen_size(s, {2560, 1440}); // 2x the reference
    layout(s);
    check_rect(rect(s, e), 20, 20, 200, 100);
    CHECK(s.world().get<UiStateComponent>(e)->scale == doctest::Approx(2.0f));
    // Ultrawide 2560x720, matching height: scale 1; matching width: scale 2; 0.5: sqrt(2).
    set_screen_size(s, {2560, 720});
    layout(s);
    CHECK(s.world().get<UiStateComponent>(e)->scale == doctest::Approx(1.0f));
    s.world().get<UiCanvasComponent>(c)->match = 0;
    layout(s);
    CHECK(s.world().get<UiStateComponent>(e)->scale == doctest::Approx(2.0f));
    s.world().get<UiCanvasComponent>(c)->match = 0.5f;
    layout(s);
    CHECK(s.world().get<UiStateComponent>(e)->scale == doctest::Approx(std::sqrt(2.0f)));
    s.world().get<UiCanvasComponent>(c)->scale_mode = ScaleMode::ConstantPixelSize;
    layout(s);
    check_rect(rect(s, e), 10, 10, 100, 50);
}

TEST_CASE("ui: vertical and horizontal layouts stack visible children") {
    register_components();
    Scene s;
    const Entity c = canvas(s);
    const Entity menu = element(s, c, "menu", corner({0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {300, 400}));
    auto& lay = s.world().add<UiLayoutComponent>(menu);
    lay.spacing = 10;
    lay.padding = 20;
    const Entity a = element(s, menu, "a", corner({0, 0}, {0, 0}, {999, 999}, {100, 50}));
    const Entity hidden = element(s, menu, "hidden", corner({0, 0}, {0, 0}, {0, 0}, {100, 50}));
    s.world().get<UiRectComponent>(hidden)->visible = false;
    const Entity b = element(s, menu, "b", corner({0, 0}, {0, 0}, {0, 0}, {100, 70}));
    layout(s);
    // Menu at (490, 160) 300x400: stretch on the cross axis, stacked from the padding.
    check_rect(rect(s, a), 510, 180, 260, 50);
    check_rect(rect(s, b), 510, 240, 260, 70);
    CHECK_FALSE(s.world().get<UiStateComponent>(hidden)->visible);
    auto* lay2 = s.world().get<UiLayoutComponent>(menu); // layout() added UiState: re-fetch
    lay2->align = LayoutAlign::Center;
    layout(s);
    check_rect(rect(s, a), 590, 180, 100, 50);
    lay2 = s.world().get<UiLayoutComponent>(menu);
    lay2->align = LayoutAlign::End;
    lay2->direction = LayoutDirection::Horizontal;
    layout(s);
    check_rect(rect(s, a), 510, 560 - 20 - 50, 100, 50);
    check_rect(rect(s, b), 620, 560 - 20 - 70, 100, 70);
}

TEST_CASE("ui: visibility and opacity propagate, clipping intersects, sort order draws on top") {
    register_components();
    Scene s;
    const Entity back = canvas(s, "back");
    const Entity front = canvas(s, "front");
    s.world().get<UiCanvasComponent>(front)->sort_order = 5;
    s.world().get<UiCanvasComponent>(back)->sort_order = -1;
    const Entity f = element(s, front, "f", corner({0, 0}, {0, 0}, {0, 0}, {100, 100}));
    s.world().add<UiPanelComponent>(f);
    const Entity panel = element(s, back, "panel", corner({0, 0}, {0, 0}, {0, 0}, {100, 100}));
    s.world().add<UiPanelComponent>(panel).color = {1, 0, 0, 1};
    s.world().get<UiRectComponent>(panel)->opacity = 0.5f;
    s.world().get<UiRectComponent>(panel)->clip_children = true;
    const Entity label = element(s, panel, "label", corner({0, 0}, {0, 0}, {50, 50}, {100, 100}));
    auto& t = s.world().add<UiTextComponent>(label);
    t.text = "Score";
    t.font_size = 20;
    s.world().get<UiRectComponent>(label)->opacity = 0.5f;
    layout(s);
    const auto* ls = s.world().get<UiStateComponent>(label);
    CHECK(ls->opacity == doctest::Approx(0.25f));
    check_rect(ls->clip, 0, 0, 100, 100);
    CHECK(s.world().get<UiStateComponent>(f)->order > ls->order); // front canvas after back
    std::vector<DrawItem> items;
    build_draw_list(s, items);
    REQUIRE(items.size() == 3);
    CHECK(items[0].entity == panel);
    CHECK(items[0].color.a == doctest::Approx(0.5f));
    CHECK(items[1].kind == DrawItem::Kind::Text);
    CHECK(items[1].text == "Score");
    CHECK(items[1].font_size == doctest::Approx(20));
    CHECK(items[1].color.a == doctest::Approx(0.25f));
    CHECK(items[2].entity == f);
    // Hidden parent hides the subtree; hidden canvas hides everything under it.
    s.world().get<UiRectComponent>(panel)->visible = false;
    layout(s);
    CHECK_FALSE(s.world().get<UiStateComponent>(label)->visible);
    s.world().get<UiCanvasComponent>(front)->visible = false;
    layout(s);
    build_draw_list(s, items);
    CHECK(items.empty());
    // Elements outside any canvas and removed components lose their state.
    s.world().remove<UiRectComponent>(f);
    layout(s);
    CHECK_FALSE(s.world().has<UiStateComponent>(f));
}

TEST_CASE("ui: pointer hover, press and click on release, with modal canvases") {
    register_components();
    Scene s;
    const Entity c = canvas(s);
    const Entity play = button(s, c, "Play", corner({0, 0}, {0, 0}, {100, 100}, {200, 60}), "play");
    const Entity label = element(s, play, "Label", corner({0, 0}, {0, 0}, {0, 0}, {200, 60}));
    s.world().add<UiTextComponent>(label).text = "Play";
    gameplay::ManualInput in;
    in.set_mouse_position({150, 120});
    update(s, &in);
    CHECK(s.world().get<UiStateComponent>(play)->hovered);
    CHECK(pointer_over_ui(s));
    CHECK(pick(s, {150, 120}) == label);          // topmost element
    CHECK(pick(s, {150, 120}, true) == play);     // topmost button
    std::vector<DrawItem> items;
    build_draw_list(s, items);
    REQUIRE(!items.empty());
    const auto& btn = s.world().get<UiButtonComponent>(play);
    CHECK(items[0].color.r == doctest::Approx(btn->hovered.r));
    // Press, release over it: one click.
    in.end_frame();
    in.set_mouse(0, true);
    update(s, &in);
    CHECK(s.world().get<UiStateComponent>(play)->pressed);
    CHECK(events(s).empty());
    in.end_frame();
    in.set_mouse(0, false);
    update(s, &in);
    REQUIRE(events(s).size() == 1);
    CHECK(events(s)[0].button == play);
    CHECK(events(s)[0].action == "play");
    in.end_frame();
    update(s, &in);
    CHECK(events(s).empty()); // events last one update
    // Press, drag off, release: no click.
    in.set_mouse(0, true);
    update(s, &in);
    in.end_frame();
    in.set_mouse_position({600, 600});
    update(s, &in);
    CHECK_FALSE(s.world().get<UiStateComponent>(play)->pressed);
    CHECK_FALSE(pointer_over_ui(s));
    in.end_frame();
    in.set_mouse(0, false);
    update(s, &in);
    CHECK(events(s).empty());
    // Disabled buttons do not react.
    s.world().get<UiButtonComponent>(play)->interactable = false;
    in.set_mouse_position({150, 120});
    in.end_frame();
    update(s, &in);
    CHECK_FALSE(s.world().get<UiStateComponent>(play)->hovered);
    CHECK_FALSE(click(s, play));
    s.world().get<UiButtonComponent>(play)->interactable = true;
    // A modal canvas on top blocks the button and reports the pointer over UI everywhere.
    const Entity modal = canvas(s, "Modal");
    s.world().get<UiCanvasComponent>(modal)->sort_order = 10;
    s.world().get<UiCanvasComponent>(modal)->blocks_input = true;
    in.end_frame();
    update(s, &in);
    CHECK_FALSE(s.world().get<UiStateComponent>(play)->hovered);
    in.set_mouse_position({1200, 700});
    in.end_frame();
    update(s, &in);
    CHECK(pointer_over_ui(s));
    CHECK(s.world().get<UiStateComponent>(play)->clicks == 1);
}

TEST_CASE("ui: keyboard navigation cycles focus and activates buttons") {
    register_components();
    Scene s;
    const Entity c = canvas(s);
    s.world().get<UiCanvasComponent>(c)->keyboard_navigation = true;
    const Entity menu = element(s, c, "Menu", corner({0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {300, 300}));
    s.world().add<UiLayoutComponent>(menu);
    const Entity resume = button(s, menu, "Resume", corner({0, 0}, {0, 0}, {0, 0}, {0, 50}), "resume");
    const Entity off = button(s, menu, "Off", corner({0, 0}, {0, 0}, {0, 0}, {0, 50}), "off");
    s.world().get<UiButtonComponent>(off)->interactable = false;
    const Entity quit = button(s, menu, "Quit", corner({0, 0}, {0, 0}, {0, 0}, {0, 50}), "quit");
    gameplay::ManualInput in;
    in.set_mouse_position({-100, -100});
    update(s, &in);
    CHECK(focused(s).is_null());
    in.set_key("Down", true);
    update(s, &in);
    CHECK(focused(s) == resume);
    CHECK(s.world().get<UiStateComponent>(resume)->focused);
    in.end_frame();
    in.set_key("Down", false);
    in.end_frame();
    in.set_key("S", true); // skips the disabled button
    update(s, &in);
    CHECK(focused(s) == quit);
    in.end_frame();
    in.set_key("S", false);
    in.set_key("Down", true);
    update(s, &in);
    CHECK(focused(s) == resume); // wraps
    in.end_frame();
    in.set_key("Down", false);
    in.set_key("Up", true);
    update(s, &in);
    CHECK(focused(s) == quit);
    in.end_frame();
    in.set_key("Up", false);
    in.set_key("Enter", true);
    update(s, &in);
    REQUIRE(events(s).size() == 1);
    CHECK(events(s)[0].action == "quit");
    // Hovering a button on a navigable canvas moves the focus to it.
    in.end_frame();
    in.set_key("Enter", false);
    const Rect r = rect(s, resume);
    in.set_mouse_position({r.x + 5, r.y + 5});
    update(s, &in);
    CHECK(focused(s) == resume);
    // Hiding the focused button clears the focus.
    s.world().get<UiRectComponent>(resume)->visible = false;
    in.end_frame();
    update(s, &in);
    CHECK(focused(s).is_null());
}

TEST_CASE("ui: components serialize through their schemas and draw items carry styles") {
    register_components();
    for (const char* name : {"UiCanvas", "UiRect", "UiPanel", "UiImage", "UiText", "UiButton", "UiLayout"}) {
        CAPTURE(name);
        const auto* schema = scene::find_schema(name);
        REQUIRE(schema);
        CHECK(schema->category == "UI");
    }
    Scene s;
    const Entity c = canvas(s);
    const Entity e = element(s, c, "Badge", corner({1, 0}, {1, 0}, {-8, 8}, {120, 40}));
    auto& p = s.world().add<UiPanelComponent>(e);
    p.corner_radius = 12;
    p.border_width = 2;
    auto& t = s.world().add<UiTextComponent>(e);
    t.text = "Coins 2 / 3";
    t.align = TextAlign::Right;
    t.vertical_align = VerticalAlign::Top;
    t.wrap = true;
    t.font = "fonts/title.ttf";
    auto& img = s.world().add<UiImageComponent>(e);
    img.tint = {0.5f, 0.5f, 1, 1};
    const json::Value saved = s.save();
    Scene loaded;
    REQUIRE(loaded.load(saved).ok());
    const Entity le = loaded.find_by_name("Badge");
    REQUIRE_FALSE(le.is_null());
    const auto* lt = loaded.world().get<UiTextComponent>(le);
    REQUIRE(lt);
    CHECK(lt->text == "Coins 2 / 3");
    CHECK(lt->align == TextAlign::Right);
    CHECK(lt->vertical_align == VerticalAlign::Top);
    CHECK(lt->font == "fonts/title.ttf");
    CHECK(loaded.world().get<UiPanelComponent>(le)->corner_radius == doctest::Approx(12));
    set_screen_size(loaded, {2560, 1440});
    layout(loaded);
    std::vector<DrawItem> items;
    build_draw_list(loaded, items);
    REQUIRE(items.size() == 3); // panel, image, text in that order
    CHECK(items[0].kind == DrawItem::Kind::Rect);
    CHECK(items[0].corner_radius == doctest::Approx(24));
    CHECK(items[0].border_width == doctest::Approx(4));
    CHECK(items[1].kind == DrawItem::Kind::Image);
    CHECK(items[1].color.r == doctest::Approx(0.5f));
    CHECK(items[2].kind == DrawItem::Kind::Text);
    CHECK(items[2].font_size == doctest::Approx(56));
    CHECK(items[2].shadow_offset.y == doctest::Approx(4));
    check_rect(items[2].rect, 2560 - 16 - 240, 16, 240, 80);
}

TEST_CASE("ui: the simulation system updates UI before scripts run") {
    register_components();
    Scene s;
    const Entity c = canvas(s);
    const Entity b = button(s, c, "B", corner({0, 0}, {0, 0}, {0, 0}, {100, 100}), "go");
    gameplay::Simulation sim;
    add_ui_systems(sim);
    gameplay::ManualInput in;
    sim.input = &in;
    std::size_t seen = 0;
    sim.add_system("observer", gameplay::Phase::Update, [&](Scene& scene, const gameplay::FrameTime&) { seen += events(scene).size(); });
    in.set_mouse_position({50, 50});
    in.set_mouse(0, true);
    sim.tick(s, 1.0f / 60.0f);
    in.end_frame();
    in.set_mouse(0, false);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(seen == 1);
    CHECK(s.world().get<UiStateComponent>(b)->clicks == 1);
    REQUIRE(sim.find("ui.update"));
    CHECK(sim.find("ui.update")->phase == gameplay::Phase::PreUpdate);
}
