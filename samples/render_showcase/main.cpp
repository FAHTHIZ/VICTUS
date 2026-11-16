// SPDX-License-Identifier: Apache-2.0
// VICTUS render showcase: windowed swapchain rendering of a PBR scene with shadows,
// point lights and a 2D HUD overlay. Orbit with the mouse (LMB drag), zoom with the wheel,
// Space pauses animation, Esc quits.
//   victus_render_showcase [--frames N] [--screenshot out.png] [--width W] [--height H]
#include "victus/core/cli.hpp"
#include "victus/core/log.hpp"
#include "victus/platform/platform.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"

#include <cmath>
#include <cstdio>
#include <deque>

using namespace victus;
using namespace victus::math;

int main(int argc, char** argv) {
    cli::Parser args("victus_render_showcase", "VICTUS rendering showcase");
    args.option("frames", "exit after N frames (0 = run until closed)", "0")
        .option("screenshot", "write the last frame to this PNG path")
        .option("width", "window width", "1280")
        .option("height", "window height", "720");
    if (auto st = args.parse(argc, argv); !st) {
        std::fprintf(stderr, "%s\n%s", st.error().message.c_str(), args.help().c_str());
        return 2;
    }
    const auto max_frames = static_cast<std::uint64_t>(args.get_number("frames", 0));

    auto plat = platform::Platform::create({});
    if (!plat) return std::fprintf(stderr, "platform: %s\n", plat.error().message.c_str()), 1;
    platform::WindowDesc wd;
    wd.title = "VICTUS Render Showcase";
    wd.width = static_cast<int>(args.get_number("width", 1280));
    wd.height = static_cast<int>(args.get_number("height", 720));
    auto window = plat.value()->create_window(wd);
    if (!window) return std::fprintf(stderr, "window: %s\n", window.error().message.c_str()), 1;
    auto device = rhi::Device::create({});
    if (!device) return std::fprintf(stderr, "gpu: %s\n", device.error().message.c_str()), 1;
    auto& dev = *device.value();
    if (auto st = dev.attach_window(*window.value()); !st) return std::fprintf(stderr, "%s\n", st.error().message.c_str()), 1;
    VICTUS_LOG_INFO("showcase", "GPU backend: {}", dev.capabilities().driver_name);

    auto ctx_r = render::GpuContext::create(dev);
    if (!ctx_r) return std::fprintf(stderr, "%s\n", ctx_r.error().message.c_str()), 1;
    auto& ctx = *ctx_r.value();
    auto scene_r = render::SceneRenderer::create(ctx);
    if (!scene_r) return std::fprintf(stderr, "%s\n", scene_r.error().message.c_str()), 1;
    auto& scene = *scene_r.value();
    auto hud_r = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    if (!hud_r) return std::fprintf(stderr, "%s\n", hud_r.error().message.c_str()), 1;
    auto& hud = *hud_r.value();

    auto sphere = ctx.create_mesh(render::make_uv_sphere(0.45f, 48, 24), "sphere").value();
    auto cube = ctx.create_mesh(render::make_cube(), "cube").value();
    auto cylinder = ctx.create_mesh(render::make_cylinder(0.3f, 2.0f), "pillar").value();
    auto ground = ctx.create_mesh(render::make_plane({30, 30}, 1, 15), "ground").value();
    auto checker = ctx.create_texture(render::make_checker(256, 8, 0xFFB0B0B0u, 0xFF707070u), {true, true, "checker"}).value();

    float yaw = 0.6f, pitch = 0.45f, dist = 11.0f, t = 0.0f;
    bool paused = false;
    rhi::TextureHandle frame;
    std::uint32_t fw = 0, fh = 0;
    std::deque<float> frame_times;
    double last = platform::Platform::seconds();

    for (std::uint64_t n = 0; !plat.value()->quit_requested(); ++n) {
        plat.value()->poll_events();
        auto& in = plat.value()->input();
        if (in.key_pressed(platform::Key::Escape)) break;
        if (in.key_pressed(platform::Key::Space)) paused = !paused;
        if (in.mouse_down(platform::MouseButton::Left)) {
            yaw -= in.mouse_delta().x * 0.006f;
            pitch = clamp(pitch + in.mouse_delta().y * 0.006f, -0.2f, 1.45f);
        }
        dist = clamp(dist - in.wheel() * 0.8f, 3.0f, 40.0f);
        const double now = platform::Platform::seconds();
        const float dt = static_cast<float>(now - last);
        last = now;
        if (!paused) t += max_frames ? 1.0f / 60.0f : dt; // deterministic when frame-limited
        frame_times.push_back(dt * 1000.0f);
        if (frame_times.size() > 120) frame_times.pop_front();

        auto cmd = dev.begin_commands();
        std::uint32_t sw = 0, sh = 0;
        rhi::TextureHandle swap = cmd.acquire_swapchain(*window.value(), sw, sh);
        if (!swap) {
            cmd.submit();
            continue;
        }
        if (!frame || fw != sw || fh != sh) {
            if (frame) dev.destroy(frame);
            rhi::TextureDesc d;
            d.name = "showcase_frame";
            d.format = rhi::Format::RGBA8_UNORM;
            d.width = fw = sw;
            d.height = fh = sh;
            d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
            frame = dev.create_texture(d).value();
        }

        const Vec3 eye{std::cos(yaw) * std::cos(pitch) * dist, std::sin(pitch) * dist + 0.5f, std::sin(yaw) * std::cos(pitch) * dist};
        render::Camera3D cam = render::Camera3D::look_at(eye, {0, 0.8f, 0});
        render::DirectionalLight sun;
        sun.direction = normalize(Vec3{-0.5f, -1.0f, -0.35f});
        render::Environment env;
        scene.begin(cam, sun, env);

        render::Material floor;
        floor.base_color_texture = checker;
        floor.roughness = 0.85f;
        scene.submit(ground, floor, Mat4::identity());
        // Metallic (rows) x roughness (columns) sphere grid.
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 5; ++col) {
                render::Material m;
                m.base_color = row == 0 ? Color{0.95f, 0.64f, 0.54f} : row == 1 ? Color{0.9f, 0.9f, 0.92f} : Color{0.2f, 0.45f, 0.9f};
                m.metallic = row < 2 ? 1.0f : 0.0f;
                m.roughness = 0.1f + 0.2f * static_cast<float>(col);
                scene.submit(sphere, m, Mat4::translation({(static_cast<float>(col) - 2.0f) * 1.2f, 0.5f + 1.05f * static_cast<float>(row), -2.0f}));
            }
        }
        for (int i = 0; i < 4; ++i) {
            const float a = t * 0.7f + static_cast<float>(i) * kTau / 4.0f;
            render::Material m;
            m.base_color = Color::srgb(i % 2 ? 0xE04040 : 0x40C070);
            m.roughness = 0.35f;
            scene.submit(cube, m, Mat4::trs({std::cos(a) * 3.5f, 0.5f, std::sin(a) * 3.5f + 1.5f}, Quat::axis_angle({0, 1, 0}, a * 2.0f), {0.8f, 0.8f, 0.8f}));
        }
        render::Material stone;
        stone.base_color = {0.6f, 0.58f, 0.55f};
        stone.roughness = 0.7f;
        for (float x : {-4.5f, 4.5f}) scene.submit(cylinder, stone, Mat4::translation({x, 1.0f, -2.0f}));
        render::Material glow;
        glow.unlit = true;
        for (int i = 0; i < 3; ++i) {
            const float a = t * 1.3f + static_cast<float>(i) * 2.1f;
            const Vec3 p{std::cos(a) * 5.0f, 1.2f, std::sin(a) * 2.5f + 1.0f};
            const Color c = i == 0 ? Color{1, 0.4f, 0.1f} : i == 1 ? Color{0.2f, 0.5f, 1} : Color{0.9f, 0.2f, 1};
            scene.add_point_light({p, c, 25.0f, 7.0f});
            glow.base_color = c;
            scene.submit(sphere, glow, Mat4::trs(p, {}, {0.25f, 0.25f, 0.25f}));
        }
        if (auto st = scene.render(cmd, frame, fw, fh, rhi::Format::RGBA8_UNORM); !st) {
            VICTUS_LOG_ERROR("showcase", "render failed: {}", st.error().message);
            cmd.submit();
            break;
        }

        // 2D HUD in pixel coordinates (origin bottom-left): frame-time graph.
        render::Camera2D hud_cam;
        hud_cam.position = {static_cast<float>(fw) * 0.5f, static_cast<float>(fh) * 0.5f};
        hud.begin(hud_cam.view_projection(static_cast<float>(fw), static_cast<float>(fh)));
        hud.draw_rect({140, 50}, {260, 80}, {0, 0, 0, 0.55f});
        float x = 14.0f;
        for (float ms : frame_times) {
            const float h = clamp(ms * 2.0f, 1.0f, 70.0f);
            hud.draw_rect({x, 14.0f + h * 0.5f}, {1.6f, h}, ms < 17.0f ? Color{0.3f, 0.9f, 0.4f} : Color{1, 0.4f, 0.2f}, 1);
            x += 2.0f;
        }
        hud.draw_line({10, 14 + 33.3f}, {270, 14 + 33.3f}, 1.0f, {1, 1, 1, 0.5f}, 2); // 16.7 ms line
        if (auto st = hud.prepare(cmd); !st) VICTUS_LOG_ERROR("showcase", "{}", st.error().message);
        rhi::RenderPassDesc rp;
        rp.colors.push_back({frame, 0, 0, rhi::LoadOp::Load, rhi::StoreOp::Store, {}, {}});
        auto pass = cmd.begin_render_pass(rp);
        pass.set_viewport({0, 0, static_cast<float>(fw), static_cast<float>(fh), 0, 1});
        hud.render(pass, cmd);
        pass.end();

        cmd.blit(frame, swap);
        const bool last_frame = max_frames && n + 1 >= max_frames;
        if (last_frame) cmd.submit_and_wait();
        else cmd.submit();
        if (n % 120 == 0)
            VICTUS_LOG_INFO("showcase", "frame {} {}x{} visible {} culled {} casters {} draws, {:.2f} ms", n, fw, fh,
                            scene.stats().visible, scene.stats().culled, scene.stats().shadow_casters, dt * 1000.0f);
        if (last_frame) {
            if (std::string path = args.get("screenshot"); !path.empty()) {
                auto px = dev.read_texture(frame);
                if (px) {
                    render::Image img{fw, fh, std::move(px).value()};
                    for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
                    if (auto st = render::save_png(path, img); st) VICTUS_LOG_INFO("showcase", "wrote {}", path);
                }
            }
            break;
        }
    }
    dev.wait_idle();
    if (frame) dev.destroy(frame);
    dev.destroy(checker);
    for (auto* m : {&sphere, &cube, &cylinder, &ground}) ctx.destroy(*m);
    return 0;
}
