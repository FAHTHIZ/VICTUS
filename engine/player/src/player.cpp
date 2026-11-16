// SPDX-License-Identifier: Apache-2.0
#include "victus/gameui/gameui.hpp"
#include "victus/player/player.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/pack.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/export.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/script.hpp"

namespace victus::player {

namespace {
// Keeps an exported game's pack mounted over its folder for the duration of a run.
struct MountGuard {
    fs::Path root;
    ~MountGuard() {
        if (!root.empty()) fs::unmount(root);
    }
};
} // namespace

Result<PlayerReport> run(const PlayerOptions& o) {
    // Exported games ship game.vpak instead of loose files: mount it over the game folder so the
    // project, scenes, scripts, audio and cooked assets are read from the pack.
    MountGuard mount;
    const fs::Path pack_path = o.project / std::string(kPackFile);
    std::error_code ec;
    const bool packaged = std::filesystem::is_regular_file(pack_path, ec);
    if (packaged) {
        auto pk = pack::Pack::open(pack_path);
        if (!pk) return std::move(pk).error();
        fs::mount(o.project, std::move(pk).value());
        mount.root = o.project;
    }
    auto proj = project::Project::open(o.project);
    if (!proj) return std::move(proj).error();
    project::register_components();
    std::unique_ptr<assets::AssetDatabase> db_ptr;
    if (packaged) {
        auto opened = assets::AssetDatabase::open_packaged(proj->root());
        if (!opened) return std::move(opened).error();
        db_ptr = std::move(opened).value();
    } else {
        auto opened = assets::AssetDatabase::open(proj->root());
        if (!opened) return std::move(opened).error();
        db_ptr = std::move(opened).value();
        const auto imported = db_ptr->import_all();
        for (const auto& m : imported.messages) VICTUS_LOG_WARN("player", "{}", m);
    }
    auto* db = &db_ptr;

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
    const std::string quality_name = o.quality.empty() ? proj->settings().render_quality : o.quality;
    const auto tier = render::parse_quality_tier(quality_name);
    if (!tier) return make_error(ErrorCode::InvalidArgument, "unknown quality tier '" + quality_name + "'");
    render::RenderSettings quality = render::RenderSettings::preset(*tier);
    if (proj->settings().render_scale > 0.0f) quality.render_scale = proj->settings().render_scale;
    (*view)->renderer().set_settings(quality);

    script::HostOptions so;
    so.assets_root = (*db)->assets_dir();
    so.watch_files = o.hot_reload && !packaged;
    auto scripts = script::ScriptHost::create(so);
    if (!scripts) return std::move(scripts).error();
    // Worker threads for physics (and future parallel systems); 0 = hardware_concurrency - 1.
    jobs::JobSystem workers(0);
    physics::PhysicsSettings ps;
    ps.jobs = &workers;
    auto physics_world = physics::PhysicsWorld::create(ps);
    if (!physics_world) return std::move(physics_world).error();
    PlatformInput input((*plat)->input());
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    gameui::add_ui_systems(sim);
    script::add_script_systems(sim, **scripts);
    physics::add_physics_systems(sim, **physics_world);
    animation::AnimationSystem animations({.provider = assets::animation_provider(**db), .jobs = &workers});
    animation::add_animation_systems(sim, animations);
    particles::ParticleSystem particle_system({.jobs = &workers});
    particles::add_particle_systems(sim, particle_system);
    navigation::NavigationSystem nav;
    navigation::add_navigation_systems(sim, nav);
    // Audio is optional: without a device (or with VICTUS_NO_AUDIO=1) the game runs silently.
    audio::EngineOptions ao;
    ao.assets_root = (*db)->assets_dir();
    ao.null_device = audio::audio_disabled_by_env();
    auto audio_engine = audio::AudioEngine::create(ao);
    if (audio_engine) audio::add_audio_systems(sim, **audio_engine);
    else VICTUS_LOG_WARN("player", "audio disabled: {}", audio_engine.error().message);
    sim.input = &input;
    // Multiplayer: join a server; it spawns this player's character from the scene's template.
    std::unique_ptr<net::NetClient> client;
    if (!o.connect.empty()) {
        auto addr = net::parse_address(o.connect);
        if (!addr) return std::move(addr).error();
        auto transport = net::create_udp_transport({});
        if (!transport) return std::move(transport).error();
        std::unique_ptr<net::Transport> t = std::move(*transport);
        if (!o.net_conditions.empty()) {
            auto c = net::parse_conditions(o.net_conditions);
            if (!c) return std::move(c).error();
            t = net::with_conditions(std::move(t), *c);
        }
        net::ClientOptions co;
        co.host = addr->first;
        co.port = addr->second;
        co.name = o.player_name;
        co.game_id = net::make_game_id(proj->settings().name, rel);
        client = std::make_unique<net::NetClient>(std::move(t), co);
        client->install(sim);
        VICTUS_LOG_INFO("player", "connecting to {}:{} as '{}'", co.host, co.port, co.name);
    }
    PlayerReport report;
    report.backend = dev.capabilities().driver_name;
    report.packaged = packaged;
    report.quality = std::string(render::quality_tier_name(*tier));
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
        (*view)->renderer().set_frame_time(dt);

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
    if (client) {
        report.net_status = client->diagnostics().status;
        client->disconnect(sim.time().time);
    }
    dev.wait_idle();
    if (frame) dev.destroy(frame);
    report.seconds = platform::Platform::seconds() - start;
    report.script_errors = (*scripts)->stats().errors;
    if (!result.ok()) return std::move(result).error();
    return report;
}

} // namespace victus::player
