// SPDX-License-Identifier: Apache-2.0
// In-game UI benchmarks: one gameui::update (layout + pointer/keyboard interaction) and one
// draw-list build for a menu-heavy screen of N panels, each holding a button with a text label
// (three elements per item, items are elements), stacked by UiLayout columns.
#include "bench_harness.hpp"

#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"

#include <string>
#include <vector>

using namespace victus;

namespace {

void build(scene::Scene& s, int columns, int rows) {
    const scene::Entity canvas = s.create("Canvas");
    s.world().add<gameui::UiCanvasComponent>(canvas).keyboard_navigation = true;
    for (int c = 0; c < columns; ++c) {
        const scene::Entity col = s.create("Column " + std::to_string(c), canvas);
        gameui::UiRectComponent r;
        r.anchor_min = r.anchor_max = {0, 0};
        r.pivot = {0, 0};
        r.position = {static_cast<float>(c) * 160.0f, 0};
        r.size = {150, 720};
        s.world().add<gameui::UiRectComponent>(col, r);
        s.world().add<gameui::UiLayoutComponent>(col).spacing = 2;
        s.world().add<gameui::UiPanelComponent>(col);
        for (int i = 0; i < rows; ++i) {
            const scene::Entity b = s.create("Button", col);
            gameui::UiRectComponent br;
            br.size = {0, 20};
            s.world().add<gameui::UiRectComponent>(b, br);
            s.world().add<gameui::UiButtonComponent>(b).action = "pick";
            const scene::Entity t = s.create("Label", b);
            gameui::UiRectComponent tr;
            tr.anchor_min = {0, 0};
            tr.anchor_max = {1, 1};
            tr.size = {0, 0};
            s.world().add<gameui::UiRectComponent>(t, tr);
            s.world().add<gameui::UiTextComponent>(t).text = "Item " + std::to_string(i);
        }
    }
}

// An options screen: rows of slider, toggle, text field and progress bar (four widgets per row).
void build_widgets(scene::Scene& s, int rows) {
    const scene::Entity canvas = s.create("Options");
    s.world().add<gameui::UiCanvasComponent>(canvas).keyboard_navigation = true;
    for (int i = 0; i < rows; ++i) {
        const auto widget = [&](float x, float w) {
            const scene::Entity e = s.create("Widget", canvas);
            gameui::UiRectComponent r;
            r.anchor_min = r.anchor_max = {0, 0};
            r.pivot = {0, 0};
            r.position = {x, static_cast<float>(i) * 24.0f};
            r.size = {w, 20};
            s.world().add<gameui::UiRectComponent>(e, r);
            return e;
        };
        s.world().add<gameui::UiSliderComponent>(widget(0, 300)).value = static_cast<float>(i % 10) / 10.0f;
        s.world().add<gameui::UiToggleComponent>(widget(310, 40)).on = (i % 2) == 0;
        s.world().add<gameui::UiTextInputComponent>(widget(360, 300)).text = "Player " + std::to_string(i);
        s.world().add<gameui::UiProgressBarComponent>(widget(670, 300)).value = 0.5f;
    }
}

} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    gameui::register_components();
    for (const int columns : {2, 8}) {
        scene::Scene s;
        build(s, columns, 32);
        gameui::set_screen_size(s, {1920, 1080});
        gameplay::ManualInput input;
        input.set_mouse_position({70, 300});
        gameui::update(s, &input);
        const std::uint32_t elements = gameui::stats(s).elements;
        if (elements < static_cast<std::uint32_t>(columns * 32 * 2)) return 2;
        const std::string n = std::to_string(elements);
        h.run("ui/update_" + n + "_elements", elements, [&] { gameui::update(s, &input); });
        std::vector<gameui::DrawItem> items;
        h.run("ui/draw_list_" + n + "_elements", elements, [&] { gameui::build_draw_list(s, items); });
    }
    {
        scene::Scene s;
        build_widgets(s, 64);
        gameui::set_screen_size(s, {1920, 1080});
        gameplay::ManualInput input;
        input.set_mouse_position({150, 10});
        gameui::update(s, &input);
        const std::uint32_t widgets = gameui::stats(s).widgets;
        if (widgets != 192) return 3; // progress bars are not interactive
        h.run("ui/update_64_option_rows", 256, [&] { gameui::update(s, &input); });
        std::vector<gameui::DrawItem> items;
        h.run("ui/draw_list_64_option_rows", 256, [&] { gameui::build_draw_list(s, items); });
    }
    return 0;
}
