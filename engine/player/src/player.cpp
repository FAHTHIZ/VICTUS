// SPDX-License-Identifier: Apache-2.0
#include "victus/player/player.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/script.hpp"

namespace victus::player {

Result<PlayerReport> run(const PlayerOptions& o) {
    auto proj = project::Project::open(o.project);
    if (!proj) return std::move(proj).error();
    project::register_components();
    auto db = assets::AssetDatabase::open(proj->root());
    if (!db) return std::move(db).error();
    const auto imported = (*db)->import_all();
    for (const auto& m : imported.messages) VICTUS_LOG_WARN("player", "{}", m);

    scene::Scene scene;
    const std::string rel = o.scene.empty() ? proj->settings().startup_scene : o.scene;
    auto path = proj->resolve(rel);
    if (!path) return std::move(path).error();
    VICTUS_TRY_STATUS(scene.load_file(*path));

    auto plat = platform::Platform::create({});
    if (!plat) return std::move(plat).error();
    platform::WindowDesc wd;
    wd.title = proj->settings().name;
    wd.width = o.width > 0 ? o.width : proj->settings().window_width;
    wd.height = o.height > 0 ? o.height : proj->settings().window_height;
    wd.fullscreen = o.fullscreen;
    auto window = (*plat)->create_window(wd);
    if (!window) return std::move(window).error();
    auto device = rhi::Device::create({});
    if (!device) return std::move(device).error();
    rhi::Device& dev = **device;
    VICTUS_TRY_STATUS(dev.attach_window(**window, proj->settings().vsync ? rhi::PresentMode::VSync : rhi::PresentMode::Immediate));
    auto gpu = render::GpuContext::create(dev);
    if (!gpu) return std::move(gpu).error();
    runtime::GpuAssetCache cache(**gpu, db->get());
    auto view = runtime::SceneView::create(**gpu, rhi::Format::RGBA8_UNORM);
    if (!view) return std::move(view).error();

    script::HostOptions so;
    so.assets_root = (*db)->assets_dir();
    so.watch_files = o.hot_reload;
    auto scripts = script::ScriptHost::create(so);
    if (!scripts) return std::move(scripts).error();
    PlatformInput input((*plat)->input());
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    script::add_script_systems(sim, **scripts);
    sim.input = &input;
    PlayerReport report;
    report.backend = dev.capabilities().driver_name;
    VICTUS_LOG_INFO("player", "{}: {} on {}", proj->settings().name, rel, report.backend);

    rhi::TextureHandle frame;
    std::uint32_t fw = 0, fh = 0;
    const double start = platform::Platform::seconds();
    double last = start;
    Status result;
    for (;;) {
        (*plat)->poll_events();
        if ((*plat)->quit_requested()) break;
        if ((*plat)->input().key_pressed(platform::Key::Escape) && !o.fullscreen && o.max_frames == 0) break;
        const double now = platform::Platform::seconds();
        const float dt = o.max_frames ? 1.0f / 60.0f : static_cast<float>(now - last);
        last = now;
        sim.tick(scene, dt);

        auto cmd = dev.begin_commands();
        std::uint32_t sw = 0, sh = 0;
        const rhi::TextureHandle swap = cmd.acquire_swapchain(**window, sw, sh);
        if (!swap) {
            cmd.submit();
            continue;
        }
        if (!frame || fw != sw || fh != sh) {
            if (frame) dev.destroy(frame);
            rhi::TextureDesc d;
            d.name = "player_frame";
            d.width = fw = sw;
            d.height = fh = sh;
            d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
            auto t = dev.create_texture(d);
            if (!t) {
                result = std::move(t).error();
                cmd.submit();
                break;
            }
            frame = *t;
        }
        if (auto st = (*view)->render(cmd, scene, cache, frame, fw, fh); !st) {
            result = std::move(st).error();
            cmd.submit();
            break;
        }
        cmd.blit(frame, swap);
        ++report.frames;
        const bool last_frame = o.max_frames && report.frames >= o.max_frames;
        if (!last_frame) {
            cmd.submit();
            continue;
        }
        cmd.submit_and_wait();
        if (!o.screenshot.empty()) {
            auto px = dev.read_texture(frame);
            if (!px) {
                result = std::move(px).error();
            } else {
                render::Image img{fw, fh, std::move(px).value()};
                for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
                result = render::save_png(o.screenshot, img);
            }
        }
        break;
    }
    dev.wait_idle();
    if (frame) dev.destroy(frame);
    report.seconds = platform::Platform::seconds() - start;
    report.script_errors = (*scripts)->stats().errors;
    if (!result.ok()) return std::move(result).error();
    return report;
}

} // namespace victus::player
