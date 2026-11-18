// SPDX-License-Identifier: Apache-2.0
// In-game UI rendering (Slice 15): canvases drawn by SceneView over 2D and 3D frames.
#include "gpu_fixture.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/runtime/scene_view.hpp"

#include <imgui.h>

using namespace victus;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 320, H = 180;

scene::Entity rect(scene::Scene& s, scene::Entity parent, const char* name, math::Vec2 pos, math::Vec2 size) {
    const scene::Entity e = s.create(name, parent);
    gameui::UiRectComponent r;
    r.anchor_min = r.anchor_max = {0, 0};
    r.pivot = {0, 0};
    r.position = pos;
    r.size = size;
    s.world().add<gameui::UiRectComponent>(e, r);
    return e;
}

void build_hud(scene::Scene& s) {
    const scene::Entity canvas = s.create("HUD");
    s.world().add<gameui::UiCanvasComponent>(canvas).reference_size = {static_cast<float>(W), static_cast<float>(H)};
    const scene::Entity red = rect(s, canvas, "Red", {10, 10}, {80, 40});
    auto& p = s.world().add<gameui::UiPanelComponent>(red);
    p.color = {1, 0, 0, 1};
    p.corner_radius = 6;
    // White bold text on a dark-blue panel.
    const scene::Entity label = rect(s, canvas, "Label", {120, 60}, {180, 60});
    auto& lp = s.world().add<gameui::UiPanelComponent>(label);
    lp.color = {0, 0, 0.5f, 1};
    auto& t = s.world().add<gameui::UiTextComponent>(label);
    t.text = "SCORE 42";
    t.font_size = 32;
    t.bold = true;
    t.color = {1, 1, 1, 1};
    // Hidden element: must not draw.
    const scene::Entity hidden = rect(s, canvas, "Hidden", {10, 120}, {60, 40});
    s.world().add<gameui::UiPanelComponent>(hidden).color = {0, 1, 0, 1};
    s.world().get<gameui::UiRectComponent>(hidden)->visible = false;
}

int bright_pixels(const std::vector<std::uint8_t>& px, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    int n = 0;
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) {
            const auto p = test::pixel(px, W, x, y);
            if (channel(p, 0) > 200 && channel(p, 1) > 200) ++n;
        }
    return n;
}

void check_hud(const std::vector<std::uint8_t>& px) {
    const auto red = test::pixel(px, W, 50, 30);
    CHECK(channel(red, 0) > 240);
    CHECK(channel(red, 1) < 20);
    // Rounded corner: the very corner pixel is not (fully) red.
    CHECK(channel(test::pixel(px, W, 10, 10), 0) < 200);
    // Text produced white glyph pixels inside the label, and the label background is blue.
    CHECK(bright_pixels(px, 120, 60, 300, 120) > 150);
    const auto bg = test::pixel(px, W, 123, 63);
    CHECK(channel(bg, 2) > 100);
    CHECK(channel(bg, 0) < 30);
    // Hidden element absent.
    const auto h = test::pixel(px, W, 40, 140);
    CHECK(channel(h, 1) < channel(h, 0) + 30);
}

} // namespace

TEST_CASE("gpu ui: SceneView draws canvases over 2D and 3D frames") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    gameui::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    auto& cc = s.world().add<scene::CameraComponent>(cam);
    cc.projection = scene::CameraComponent::Projection::Orthographic;
    build_hud(s);
    s.update_transforms();

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto view = runtime::SceneView::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(view.ok());
    auto target = test::make_target(W, H);
    ImGuiContext* before = ImGui::GetCurrentContext();

    runtime::SceneViewOptions o;
    o.clear_2d = {0, 0, 0, 1};
    auto cmd = dev.begin_commands();
    auto st = (*view)->render(cmd, s, assets, target, W, H, o);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
    cmd.submit_and_wait();
    CHECK(st->mode == runtime::ViewMode::TwoD);
    CHECK(st->ui.items == 3); // red panel, label panel and label text (the hidden panel is skipped)
    CHECK(st->ui.vertices > 0);
    CHECK(ImGui::GetCurrentContext() == before);
    auto px = dev.read_texture(target).value();
    test::save_output("game_ui_2d", W, H, px);
    check_hud(px);

    // 3D path: same canvases over the tonemapped scene.
    s.world().get<scene::CameraComponent>(cam)->projection = scene::CameraComponent::Projection::Perspective;
    auto cmd3 = dev.begin_commands();
    auto st3 = (*view)->render(cmd3, s, assets, target, W, H, {});
    REQUIRE_MESSAGE(st3.ok(), (st3.ok() ? "" : st3.error().message));
    cmd3.submit_and_wait();
    CHECK(st3->mode == runtime::ViewMode::ThreeD);
    auto px3 = dev.read_texture(target).value();
    test::save_output("game_ui_3d", W, H, px3);
    check_hud(px3);

    // Disabled UI leaves the frame untouched.
    o.draw_ui = false;
    s.world().get<scene::CameraComponent>(cam)->projection = scene::CameraComponent::Projection::Orthographic;
    auto cmd4 = dev.begin_commands();
    auto st4 = (*view)->render(cmd4, s, assets, target, W, H, o);
    REQUIRE(st4.ok());
    cmd4.submit_and_wait();
    CHECK(st4->ui.items == 0);
    auto px4 = dev.read_texture(target).value();
    CHECK(channel(test::pixel(px4, W, 50, 30), 0) < 20);
    dev.destroy(target);
}

// Localized strings arrive as UTF-8: glyphs outside ASCII (accented Latin, Cyrillic, Greek) must
// come from the embedded Inter font, not its fallback glyph. "????" draws ImGui's fallback, so
// a missing glyph would produce the same pixels.
TEST_CASE("gpu ui: non-ASCII UTF-8 text draws real glyphs") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    gameui::register_components();
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto view = runtime::SceneView::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(view.ok());
    auto target = test::make_target(W, H);
    const auto render_text = [&](const char* text) {
        scene::Scene s;
        const scene::Entity cam = s.create("Camera");
        s.world().add<scene::CameraComponent>(cam).projection = scene::CameraComponent::Projection::Orthographic;
        const scene::Entity canvas = s.create("HUD");
        s.world().add<gameui::UiCanvasComponent>(canvas).reference_size = {static_cast<float>(W), static_cast<float>(H)};
        const scene::Entity label = rect(s, canvas, "Label", {0, 0}, {static_cast<float>(W), static_cast<float>(H)});
        auto& t = s.world().add<gameui::UiTextComponent>(label);
        t.text = text;
        t.font_size = 48;
        t.color = {1, 1, 1, 1};
        t.shadow_color = {0, 0, 0, 0};
        s.update_transforms();
        runtime::SceneViewOptions o;
        o.clear_2d = {0, 0, 0, 1};
        auto cmd = dev.begin_commands();
        auto st = (*view)->render(cmd, s, assets, target, W, H, o);
        REQUIRE(st.ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        return std::pair{bright_pixels(px, 0, 0, W, H), px};
    };
    const auto [fallback, fpx] = render_text("????");
    const auto [cyrillic, cpx] = render_text("ЖЖЖЖ");
    const auto [greek, gpx] = render_text("ΩΩΩΩ");
    const auto [latin, lpx] = render_text("ÉÉÉÉ");
    test::save_output("game_ui_utf8", W, H, cpx);
    MESSAGE("bright pixels: ???? " << fallback << ", ЖЖЖЖ " << cyrillic << ", ΩΩΩΩ " << greek << ", ÉÉÉÉ " << latin);
    CHECK(fallback > 0);
    CHECK(cyrillic != fallback);
    CHECK(greek != fallback);
    CHECK(latin != fallback);
    CHECK(cpx != fpx);
    CHECK(gpx != fpx);
    CHECK(lpx != fpx);
    dev.destroy(target);
}
