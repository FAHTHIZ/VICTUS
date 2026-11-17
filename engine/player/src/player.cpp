// SPDX-License-Identifier: Apache-2.0
#include "victus/gameui/gameui.hpp"
#include "victus/player/player.hpp"
#include "victus/runtime/hot_reload.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/pack.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/export.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/render/temporal.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/script.hpp"

#include <algorithm>
#include <format>
#include <optional>

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
    // Dynamic resolution holds the project's target frame rate in interactive runs (fixed-step,
    // recorded and replayed runs keep a constant scale so captures stay deterministic).
    std::optional<render::DynamicResolution> dynamic_resolution;
    if (proj->settings().target_fps > 0 && o.max_frames == 0 && o.replay.empty() && o.record.empty()) {
        render::DynamicResolutionSettings ds;
        ds.target_frame_ms = 1000.0f / static_cast<float>(proj->settings().target_fps);
        ds.min_scale = std::min(proj->settings().dynamic_resolution_min, quality.render_scale);
        ds.max_scale = quality.render_scale;
        dynamic_resolution.emplace(ds, quality.render_scale);
    }
    if (*tier == render::QualityTier::Low) cache.set_environment_bake(render::EnvironmentBakeSettings::low());

    // Regression captures: the replayed capture fixes the script seed; a recording picks one.
    if ((!o.record.empty() || !o.replay.empty()) && (!o.connect.empty() || o.host))
        return make_error(ErrorCode::InvalidArgument, "record/replay captures work offline only (not with --connect or --host)");
    if (o.host && !o.connect.empty()) return make_error(ErrorCode::InvalidArgument, "choose either host or connect");
    if (!o.record.empty() && !o.replay.empty()) return make_error(ErrorCode::InvalidArgument, "choose either record or replay");
    std::optional<gameplay::Capture> replay_capture;
    if (!o.replay.empty()) {
        auto c = gameplay::Capture::load(o.replay);
        if (!c) return std::move(c).error();
        replay_capture = std::move(*c);
    }
    gameplay::Capture recording;
    if (!o.record.empty()) recording.seed = Random(static_cast<std::uint64_t>(platform::Platform::seconds() * 1e9)).next_u64() | 1;
    script::HostOptions so;
    so.random_seed = replay_capture ? replay_capture->seed : recording.seed;
    so.assets_root = (*db)->assets_dir();
    so.watch_files = o.hot_reload && !packaged;
    so.input_map = proj->settings().input;
    so.save_version = proj->settings().save_version;
    so.save_directory = !o.saves.empty() ? o.saves
                        : packaged      ? gameplay::default_save_directory(proj->settings().name)
                                        : proj->dev_save_directory();
    // Exported games have no .meta files; prefab UUIDs come from the asset index.
    so.prefab_loader = [assets = db->get()](std::string_view prefab_path) -> Result<script::PrefabSource> {
        auto src = script::read_prefab(assets->assets_dir(), prefab_path);
        if (src && src->asset.is_nil())
            if (const auto* rec = assets->find_path(prefab_path)) src->asset = rec->uuid;
        return src;
    };
    so.asset_resolver = [assets = db->get()](std::string_view asset_path) -> Result<Uuid> {
        if (const auto* rec = assets->find_path(asset_path)) return rec->uuid;
        return make_error(ErrorCode::NotFound, std::format("no asset at '{}'", asset_path));
    };
    so.project_root = proj->root();
    so.bridge_launcher = [project = &*proj, saves = so.save_directory](std::string_view name) {
        return project::bridge_launch(*project, name, saves);
    };
    // Scene transitions start the next scene with a fresh physics world and silence.
    physics::PhysicsWorld* physics_for_scenes = nullptr;
    audio::AudioEngine* audio_for_scenes = nullptr;
    so.on_scene_loaded = [&physics_for_scenes, &audio_for_scenes](scene::Scene&, std::string_view) {
        if (physics_for_scenes) physics_for_scenes->clear();
        if (audio_for_scenes) audio_for_scenes->stop_all();
    };
    auto scripts = script::ScriptHost::create(so);
    if (!scripts) return std::move(scripts).error();
    (*scripts)->set_current_scene(rel);
    // Worker threads for physics (and future parallel systems); 0 = hardware_concurrency - 1.
    jobs::JobSystem workers(0);
    physics::PhysicsSettings ps;
    ps.jobs = &workers;
    auto physics_world = physics::PhysicsWorld::create(ps);
    if (!physics_world) return std::move(physics_world).error();
    physics_for_scenes = physics_world->get();
    PlatformInput input((*plat)->input());
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    gameui::add_ui_systems(sim);
    script::add_script_systems(sim, **scripts);
    physics::add_physics_systems(sim, **physics_world);
    animation::AnimationSystem animations(
        {.provider = assets::animation_provider(**db), .sheets = assets::sprite_sheet_provider(**db), .jobs = &workers});
    animation::add_animation_systems(sim, animations);
    tilemap::TilemapSystem tilemaps(assets::tileset_provider(**db));
    tilemap::add_tilemap_systems(sim, tilemaps);
    particles::ParticleSystem particle_system({.jobs = &workers});
    particles::add_particle_systems(sim, particle_system);
    navigation::NavigationSystem nav;
    navigation::add_navigation_systems(sim, nav);
    // Audio is optional: without a device (or with VICTUS_NO_AUDIO=1) the game runs silently.
    audio::EngineOptions ao;
    ao.assets_root = (*db)->assets_dir();
    ao.null_device = audio::audio_disabled_by_env();
    auto audio_engine = audio::AudioEngine::create(ao);
    if (audio_engine) {
        audio::add_audio_systems(sim, **audio_engine);
        audio_for_scenes = audio_engine->get();
    }
    else VICTUS_LOG_WARN("player", "audio disabled: {}", audio_engine.error().message);
    sim.input = &input;
    // Captures drive the simulation through a ReplayInput in both directions, so a recording sees
    // exactly the input states its replay will feed back.
    gameplay::ReplayInput captured_input;
    std::optional<gameplay::CaptureReplay> replay;
    if (replay_capture) {
        replay.emplace(*replay_capture);
        sim.input = &replay->input();
        if (!replay_capture->scene.empty() && replay_capture->scene != rel)
            VICTUS_LOG_WARN("player", "capture was recorded in {}, replaying in {}", replay_capture->scene, rel);
    } else if (!o.record.empty()) {
        recording.project = proj->settings().name;
        recording.scene = rel;
        sim.input = &captured_input;
    }
    // Development runs reload changed textures, models and audio while the game runs.
    std::unique_ptr<runtime::AssetHotReloader> reloader;
    if (o.hot_reload && !packaged) reloader = std::make_unique<runtime::AssetHotReloader>(**db);
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
        if (!o.insecure) {
            net::SecureOptions sec;
            if (!o.server_key.empty()) {
                auto k = net::key_from_hex(o.server_key);
                if (!k) return make_error(ErrorCode::InvalidArgument, "server key: " + k.error().message);
                sec.expected_server_key = *k;
            }
            sec.password = o.net_password;
            t = net::with_encryption(std::move(t), std::move(sec));
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
    // Listen server: this player hosts the session and plays in it.
    std::unique_ptr<net::NetServer> server;
    if (o.host) {
        net::UdpOptions uo;
        uo.port = o.host_port ? o.host_port : net::kDefaultPort;
        uo.max_peers = o.max_clients + 4u; // room to tell extra peers the server is full
        auto transport = net::create_udp_transport(uo);
        if (!transport) return std::move(transport).error();
        std::unique_ptr<net::Transport> t = std::move(*transport);
        if (!o.net_conditions.empty()) {
            auto c = net::parse_conditions(o.net_conditions);
            if (!c) return std::move(c).error();
            t = net::with_conditions(std::move(t), *c);
        }
        std::string security = "unencrypted (insecure): only use on networks you trust";
        if (o.insecure) {
            if (!o.net_password.empty()) return make_error(ErrorCode::InvalidArgument, "a password needs encryption; remove insecure");
        } else {
            fs::Path key_file = o.host_key;
            if (key_file.empty()) {
                if (packaged) {
                    const fs::Path sd = gameplay::default_save_directory(proj->settings().name);
                    key_file = sd.parent_path() / (sd.filename().string() + ".server.key");
                } else {
                    key_file = proj->root() / ".victus" / "server.key";
                }
            }
            auto kp = net::load_or_create_key(key_file);
            if (!kp) return std::move(kp).error();
            net::SecureOptions sec;
            sec.server_secret = kp->secret;
            sec.password = o.net_password;
            t = net::with_encryption(std::move(t), std::move(sec));
            security = "encrypted; server key " + net::key_to_hex(kp->public_key);
        }
        net::ServerOptions so2;
        so2.game_id = net::make_game_id(proj->settings().name, rel);
        so2.max_clients = o.max_clients;
        so2.host_player = true;
        so2.host_name = o.player_name;
        server = std::make_unique<net::NetServer>(std::move(t), so2);
        server->install(sim);
        VICTUS_LOG_INFO("player", "hosting on UDP port {} as '{}' ({})", uo.port, o.player_name, security);
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
        if (reloader) {
            const auto changes = reloader->poll(now);
            if (!changes.empty()) {
                runtime::AssetHotReloader::apply(changes, &cache, &animations, &tilemaps);
                if (audio_engine)
                    for (const auto& clip : changes.audio) (void)(*audio_engine)->reload_clip(clip);
                if (!changes.prefabs.empty()) (*scripts)->forget_prefabs(); // new instances use the new version
                report.assets_reloaded += changes.reimported.size() + changes.audio.size() + changes.prefabs.size() + changes.materials.size() + changes.shaders.size() + changes.sprite_sheets.size() + changes.tilesets.size();
            }
        }
        if (dynamic_resolution) {
            const float before = dynamic_resolution->scale();
            const float scale = dynamic_resolution->update(static_cast<float>(now - last) * 1000.0f);
            if (scale != before) {
                render::RenderSettings rs = (*view)->renderer().settings();
                rs.render_scale = scale;
                (*view)->renderer().set_settings(rs);
            }
        }
        float dt = o.max_frames ? 1.0f / 60.0f : std::clamp(static_cast<float>(now - last), 1e-4f, 0.25f);
        last = now;
        if (replay) {
            if (replay->done()) break;
            dt = replay->begin_tick();
        } else if (!o.record.empty()) {
            const auto state = gameplay::sample_input(input);
            recording.add_frame(state, dt);
            captured_input.set(state);
        }
        sim.tick(scene, dt);
        if (replay) {
            if (!replay->end_tick(scene)) VICTUS_LOG_WARN("player", "replay diverged at {}", replay->mismatch());
        } else if (!o.record.empty()) {
            captured_input.end_frame();
            recording.after_tick(scene);
        }
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
    if (server) {
        const auto joined = server->diagnostics().clients;
        report.net_status = "hosted the session; " + std::to_string(joined) + (joined == 1 ? " client" : " clients") + " connected at exit";
        server->shutdown(sim.time().time);
    }
    if (client) {
        report.net_status = client->diagnostics().status;
        client->disconnect(sim.time().time);
    }
    if (replay) {
        report.digests_checked = replay->digests_checked();
        report.replay_mismatch = replay->mismatch();
    }
    if (!o.record.empty() && !recording.frames.empty()) {
        recording.finish(scene);
        if (auto st = recording.save(o.record); !st && result.ok()) result = std::move(st).error();
    }
    dev.wait_idle();
    if (frame) dev.destroy(frame);
    report.seconds = platform::Platform::seconds() - start;
    report.script_errors = (*scripts)->stats().errors;
    report.scene_loads = (*scripts)->stats().scene_loads;
    report.final_scene = (*scripts)->current_scene();
    if (!result.ok()) return std::move(result).error();
    return report;
}

} // namespace victus::player
