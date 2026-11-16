// SPDX-License-Identifier: Apache-2.0
// UI layer on the GPU: an offscreen ImGui frame (font atlas creation, text, widgets, a user
// texture via ImGui::Image, scissor clipping) rendered through the VICTUS RHI and pixel-probed.
#include "gpu_fixture.hpp"
#include "victus/ui/ui.hpp"

using namespace victus;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

TEST_CASE("gpu: ImGui frame renders through the VICTUS RHI") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    constexpr std::uint32_t W = 320, H = 200;
    const auto target = test::make_target(W, H);
    REQUIRE(target.valid());

    auto ui = ui::UiContext::create(ctx, rhi::Format::RGBA8_UNORM, nullptr, nullptr);
    REQUIRE(ui.ok());
    auto image = ctx.create_texture(render::make_checker(4, 2, 0xff0000ffu, 0xff0000ffu), {false, false, "red"});
    REQUIRE(image.ok());

    for (int frame = 0; frame < 2; ++frame) {
        (*ui)->new_frame(1.0f / 60.0f, {static_cast<float>(W), static_cast<float>(H)});
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowSize(ImVec2(200, 150));
        ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::TextUnformatted("VICTUS editor UI");
        ImGui::Button("Run", ImVec2(80, 24));
        ImGui::Image(ImTextureRef(ui::UiContext::texture_id(*image)), ImVec2(40, 40));
        ImGui::End();
        auto cmd = dev.begin_commands();
        REQUIRE((*ui)->render_to(cmd, target, true, {0, 0, 0, 1}).ok());
        cmd.submit_and_wait();
    }
    const auto& stats = (*ui)->stats();
    CHECK(stats.textures >= 1);   // font atlas
    CHECK(stats.vertices > 100);
    CHECK(stats.draw_commands >= 2); // atlas + user image

    auto rgba = dev.read_texture(target);
    REQUIRE(rgba.ok());
    test::save_output("ui_frame", W, H, *rgba);

    // Outside the window: untouched clear colour.
    const auto outside = pixel(*rgba, W, 300, 190);
    CHECK(channel(outside, 0) == 0);
    CHECK(channel(outside, 1) == 0);
    // Window body: the theme's panel colour (dark grey, non-black).
    const auto body = pixel(*rgba, W, 180, 140);
    CHECK(channel(body, 0) > 20);
    CHECK(channel(body, 0) < 80);
    // Text and widgets produce bright pixels in the window's first rows.
    int bright = 0;
    for (std::uint32_t y = 30; y < 60; ++y)
        for (std::uint32_t x = 15; x < 200; ++x)
            if (channel(pixel(*rgba, W, x, y), 1) > 180) ++bright;
    CHECK(bright > 30);
    // The user image: find a pure red pixel inside the window.
    int red = 0;
    for (std::uint32_t y = 60; y < 160; ++y)
        for (std::uint32_t x = 15; x < 120; ++x) {
            const auto p = pixel(*rgba, W, x, y);
            if (channel(p, 0) > 240 && channel(p, 1) < 15 && channel(p, 2) < 15) ++red;
        }
    CHECK(red > 1000); // ~40x40 px image

    (*ui).reset();
    dev.destroy(*image);
    dev.destroy(target);
}
