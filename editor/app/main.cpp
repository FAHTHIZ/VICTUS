// SPDX-License-Identifier: Apache-2.0
// VICTUS Editor executable.
//   victus_editor [project-folder]
//   victus_editor --new <folder> [--template 3d|2d|simulation|multiplayer] [--name "My Game"]
// Automation (CI, screenshots, docs):
//   --frames N        exit after N frames
//   --screenshot PNG  write the final frame
//   --play            enter play mode after opening the project
//   --show WINDOW     open a window after opening (input-actions, network)
//   --host            enter play mode as a listen server (--port, default 27960)
//   --select NAME     select an entity by name after opening
//   --data-dir DIR    use DIR instead of the user data directory (recent list, layout)
#include "victus/core/build_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/editor.hpp"
#include "victus/platform/platform.hpp"
#include "victus/render/image.hpp"

#include <cstdio>
#include <filesystem>

using namespace victus;

namespace {
int fail(const char* what, const Error& e) {
    std::fprintf(stderr, "victus_editor: %s: %s\n", what, e.message.c_str());
    return 1;
}
} // namespace

int main(int argc, char** argv) {
    cli::Parser args("victus_editor", "VICTUS Editor");
    args.positional("project", "project folder to open")
        .option("new", "create a project in this folder, then open it")
        .option("template", "template for --new (3d, 2d, simulation)", "3d")
        .option("name", "project name for --new (default: folder name)")
        .option("frames", "exit after N frames (0 = run until closed)", "0")
        .option("screenshot", "write the last frame to this PNG")
        .option("width", "window width", "1600")
        .option("height", "window height", "900")
        .option("data-dir", "override the user data directory")
        .option("select", "select the entity with this name after opening")
        .option("show", "open a window after opening: input-actions, network")
        .option("port", "UDP port for --host", "27960")
        .flag("host", "enter play mode as a listen server after opening")
        .flag("play", "enter play mode after opening");
    if (auto st = args.parse(argc, argv); !st) {
        std::fprintf(stderr, "%s\n%s", st.error().message.c_str(), args.help().c_str());
        return 2;
    }
    const auto max_frames = static_cast<std::uint64_t>(args.get_number("frames", 0));
    const fs::Path data_dir = args.has("data-dir") ? fs::Path(args.get("data-dir")) : fs::user_data_dir();
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    auto plat = platform::Platform::create({});
    if (!plat) return fail("platform", plat.error());
    platform::WindowDesc wd;
    wd.title = "VICTUS Editor";
    wd.width = static_cast<int>(args.get_number("width", 1600));
    wd.height = static_cast<int>(args.get_number("height", 900));
    wd.maximized = max_frames == 0;
    auto window = plat.value()->create_window(wd);
    if (!window) return fail("window", window.error());
    auto device = rhi::Device::create({});
    if (!device) return fail("gpu", device.error());
    auto& dev = *device.value();
    if (auto st = dev.attach_window(*window.value()); !st) return fail("swapchain", st.error());
    VICTUS_LOG_INFO("editor", "VICTUS Editor {} on {} ({})", build::version(), dev.capabilities().driver_name,
                    plat.value()->video_driver());
    auto gpu = render::GpuContext::create(dev);
    if (!gpu) return fail("renderer", gpu.error());

    ui::UiOptions uo;
    uo.ini_path = (data_dir / "editor_layout.ini").string();
    auto ui = ui::UiContext::create(*gpu.value(), rhi::Format::RGBA8_UNORM, plat.value().get(), window.value().get(), uo);
    if (!ui) return fail("ui", ui.error());

    editor::EditorOptions eo;
    eo.recent_file = data_dir / "recent_projects.json";
    auto ed_r = editor::Editor::create(*gpu.value(), *ui.value(), plat.value().get(), window.value().get(), eo);
    if (!ed_r) return fail("editor", ed_r.error());
    auto& ed = *ed_r.value();

    if (args.has("new")) {
        const fs::Path dir = args.get("new");
        const std::string name = args.has("name") ? args.get("name") : dir.filename().string();
        if (auto st = ed.create_project(dir, name, args.get("template")); !st.ok()) return fail("create project", st.error());
    } else if (const std::string p = args.get("project"); !p.empty()) {
        if (auto st = ed.open_project(p); !st.ok()) return fail("open project", st.error());
    }
    if (args.has("select") && ed.has_project()) {
        const auto e = ed.edit_scene().find_by_name(args.get("select"));
        if (!e.is_null()) ed.select(e);
    }
    if (args.has("show")) {
        if (args.get("show") == "input-actions") ed.set_show_input_actions(true);
        else if (args.get("show") == "network") ed.set_show_network(true);
        else std::fprintf(stderr, "victus_editor: unknown window '%s' for --show\n", args.get("show").c_str());
    }
    if (args.has("host") && ed.has_project()) {
        editor::NetPlaySettings ns = ed.net_settings();
        ns.port = static_cast<std::uint16_t>(args.get_number("port", net::kDefaultPort));
        if (auto st = ed.set_net_settings(ns); !st.ok()) return fail("network settings", st.error());
        ed.play_host();
        if (!ed.play_server()) return fail("host", make_error(ErrorCode::InvalidArgument, "could not host (see log)"));
    } else if (args.has("play") && ed.has_project()) {
        ed.play();
    }

    rhi::TextureHandle frame;
    std::uint32_t fw = 0, fh = 0;
    double last = platform::Platform::seconds();
    int exit_code = 0;
    for (std::uint64_t n = 0;; ++n) {
        plat.value()->poll_events();
        if (plat.value()->quit_requested() && !ed.quit_confirmed()) {
            if (!ed.request_quit()) plat.value()->cancel_quit();
        }
        if (plat.value()->quit_requested() && ed.quit_confirmed()) break;
        const double now = platform::Platform::seconds();
        const float dt = max_frames ? 1.0f / 60.0f : static_cast<float>(now - last);
        last = now;

        ui.value()->new_frame(dt);
        ed.frame(dt);

        auto cmd = dev.begin_commands();
        std::uint32_t sw = 0, sh = 0;
        rhi::TextureHandle swap = cmd.acquire_swapchain(*window.value(), sw, sh);
        if (auto st = ed.render(cmd); !st.ok()) VICTUS_LOG_ERROR("editor", "viewport: {}", st.error().message);
        if (!swap) {
            // Minimised: still finish the UI frame so ImGui state stays consistent.
            (void)ui.value()->prepare(cmd);
            cmd.submit();
            continue;
        }
        if (!frame || fw != sw || fh != sh) {
            if (frame) dev.destroy(frame);
            rhi::TextureDesc d;
            d.name = "editor_frame";
            d.format = rhi::Format::RGBA8_UNORM;
            d.width = fw = sw;
            d.height = fh = sh;
            d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
            auto t = dev.create_texture(d);
            if (!t) return fail("frame", t.error());
            frame = t.value();
        }
        if (auto st = ui.value()->render_to(cmd, frame, true, {0.06f, 0.06f, 0.07f, 1}); !st.ok())
            VICTUS_LOG_ERROR("editor", "ui: {}", st.error().message);
        cmd.blit(frame, swap);
        const bool last_frame = max_frames && n + 1 >= max_frames;
        if (last_frame) cmd.submit_and_wait();
        else cmd.submit();
        if (last_frame) {
            if (const std::string path = args.get("screenshot"); !path.empty()) {
                auto px = dev.read_texture(frame);
                if (!px) {
                    exit_code = fail("screenshot", px.error());
                } else {
                    render::Image img{fw, fh, std::move(px).value()};
                    for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
                    if (auto st = render::save_png(path, img); !st) exit_code = fail("screenshot", st.error());
                    else VICTUS_LOG_INFO("editor", "wrote {}", path);
                }
            }
            break;
        }
    }
    dev.wait_idle();
    if (frame) dev.destroy(frame);
    return exit_code;
}
