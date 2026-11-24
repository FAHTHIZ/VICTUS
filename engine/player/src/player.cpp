// SPDX-License-Identifier: Apache-2.0
#include "victus/gameui/gameui.hpp"
#include "victus/player/player.hpp"
#include "victus/plugin/plugin.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/physics_ground_probe.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/pack.hpp"
#include "victus/core/log.hpp"
#include "victus/core/memory.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/ai/ai.hpp"
#include "victus/procgen/city.hpp"
#include "victus/traffic/traffic.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/export.hpp"
#include "victus/player/mobile.hpp"
#include "victus/player/platform_input.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/render/temporal.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/runtime/world_streaming.hpp"
#include "victus/script/script.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <thread>

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
        auto po = pack_open_options(o.project, o.pack_key);
        if (!po) return std::move(po).error();
        auto pk = pack::Pack::open(pack_path, *po);
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

    // A mistyped --quality is reported before any window or GPU device exists: failing after the
    // device is up raced the Vulkan driver's worker threads at exit (a crash instead of the error).
    if (!o.quality.empty() && o.quality != "auto" && !render::parse_quality_tier(o.quality))
        return make_error(ErrorCode::InvalidArgument, "unknown quality tier '" + o.quality + "'");

    auto plat = platform::Platform::create({});
    if (!plat) return std::move(plat).error();
    platform::WindowDesc wd;
    wd.title = proj->settings().name;
    wd.width = o.width > 0 ? o.width : proj->settings().window_width;
    wd.height = o.height > 0 ? o.height : proj->settings().window_height;
    wd.fullscreen = o.fullscreen;
    // The device comes first (ADR-0144 phase 3): without Vulkan/Metal/D3D12 it falls back to
    // OpenGL ES 3.0, whose window must then be created with SDL_WINDOW_OPENGL.
    rhi::DeviceDesc device_desc;
    device_desc.gles_fallback = true;
    auto created_device = rhi::Device::create(device_desc);
    if (!created_device) return std::move(created_device).error();
    // Owned through pointers: after a device loss they are replaced by new objects (ADR-0157).
    std::unique_ptr<rhi::Device> device = std::move(created_device).value();
    rhi::Device* dev = device.get();
    wd.opengl = dev->capabilities().backend == rhi::Backend::GLES;
    auto window = (*plat)->create_window(wd);
    if (!window) return std::move(window).error();
    VICTUS_TRY_STATUS(dev->attach_window(**window, proj->settings().vsync ? rhi::PresentMode::VSync : rhi::PresentMode::Immediate));
    auto created_gpu = render::GpuContext::create(*dev);
    if (!created_gpu) return std::move(created_gpu).error();
    std::unique_ptr<render::GpuContext> gpu = std::move(created_gpu).value();
    runtime::GpuAssetCache cache(*gpu, db->get());
    const bool mobile = platform::Platform::is_mobile();
    const std::string quality_name = !o.quality.empty() ? o.quality
                                     : mobile           ? proj->settings().mobile_quality
                                                        : proj->settings().render_quality;
    // "auto" (ADR-0127): pick the tier from the GPU family and RAM; Medium on unknown phones,
    // High on unknown desktops.
    std::optional<render::QualityTier> tier;
    std::string device_class;
    if (quality_name == "auto") {
        render::DeviceProfile profile;
        profile.gpu = dev->capabilities().driver_name;
        profile.mobile = mobile;
        profile.ram_mb = platform::Platform::system_ram_mb();
        profile.compute = dev->capabilities().compute;
        const render::DeviceClass dc = render::classify_device(profile, mobile ? render::QualityTier::Medium : render::QualityTier::High);
        tier = dc.tier;
        device_class = dc.reason;
        VICTUS_LOG_INFO("player", "quality auto -> {} ({})", render::quality_tier_name(dc.tier), dc.reason);
    } else {
        tier = render::parse_quality_tier(quality_name);
    }
    if (!tier) return make_error(ErrorCode::InvalidArgument, "unknown quality tier '" + quality_name + "'");
    // Packed R11G11B10F scene colour (ADR-0050, ADR-0126) halves HDR bandwidth where bandwidth is
    // the limit: phones and the Low/Medium tiers. Fixed for the run (the pipelines are built for it).
    render::SceneRendererOptions renderer_options;
    renderer_options.compact_hdr = o.compact_hdr.value_or(compact_hdr_by_default(*tier, mobile));
    renderer_options.half_precision = o.half_precision.value_or(half_precision_by_default(*tier, mobile));
    auto created_view = runtime::SceneView::create(*gpu, rhi::Format::RGBA8_UNORM, renderer_options);
    if (!created_view) return std::move(created_view).error();
    std::unique_ptr<runtime::SceneView> view = std::move(created_view).value();
    const char* hdr_format = view->renderer().hdr_format() == rhi::Format::R11G11B10_FLOAT ? "R11G11B10F" : "RGBA16F";
    render::RenderSettings quality = render::RenderSettings::preset(*tier);
    if (proj->settings().render_scale > 0.0f) quality.render_scale = proj->settings().render_scale;
    quality.temporal_upscale = proj->settings().temporal_upscale;
    if (o.render_scale) quality.render_scale = *o.render_scale;
    if (o.taa) quality.taa = *o.taa;
    if (o.taa && !*o.taa) quality.taa_below_scale = 0.0f; // --taa off: no switch at low scales either
    if (o.reactive_mask) quality.reactive_mask = *o.reactive_mask;
    if (o.msaa_samples) quality.msaa_samples = *o.msaa_samples;
    quality = quality.sanitized();
    view->renderer().set_settings(quality);
    // Dynamic resolution holds the project's target frame rate in interactive runs (fixed-step,
    // recorded and replayed runs keep a constant scale so captures stay deterministic).
    std::optional<render::DynamicResolution> dynamic_resolution;
    const int target_fps = mobile ? proj->settings().mobile_target_fps : proj->settings().target_fps;
    if (target_fps > 0 && o.max_frames == 0 && o.replay.empty() && o.record.empty()) {
        render::DynamicResolutionSettings ds;
        ds.target_frame_ms = 1000.0f / static_cast<float>(target_fps);
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
    if (!o.record.empty())
        recording.seed = o.random_seed ? o.random_seed : Random(static_cast<std::uint64_t>(platform::Platform::seconds() * 1e9)).next_u64() | 1;
    script::HostOptions so;
    if (replay_capture) so.random_seed = replay_capture->seed;
    else if (!o.record.empty()) so.random_seed = recording.seed;
    else so.random_seed = o.random_seed ? o.random_seed : (o.max_frames ? kFixedRunSeed : 0);
    so.assets_root = (*db)->assets_dir();
    so.watch_files = o.hot_reload && !packaged;
    so.input_map = proj->settings().input;
    so.save_version = proj->settings().save_version;
    so.default_locale = proj->settings().default_locale;
    so.script_api = proj->settings().script_api;
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
    bool world_changed = false; // a scene transition replaces the streamed world (6h)
    // A hosting player's transitions take its clients along (level travel, ADR-0259).
    net::NetServer* server_for_scenes = nullptr;
    script::ScriptHost* scripts_for_scenes = nullptr;
    so.on_scene_loaded = [&physics_for_scenes, &audio_for_scenes, &world_changed, &server_for_scenes,
                          &scripts_for_scenes](scene::Scene& s, std::string_view loaded) {
        world_changed = true;
        if (physics_for_scenes) physics_for_scenes->clear();
        if (audio_for_scenes) audio_for_scenes->stop_all();
        if (server_for_scenes && scripts_for_scenes)
            if (auto st = server_for_scenes->travel(s, loaded, scripts_for_scenes->scene_params()); !st.ok())
                VICTUS_LOG_ERROR("player", "level travel to {}: {}", loaded, st.error().message);
    };
    // Native plugins (ADR-0078) load before scripts so Luau can call their functions from the
    // first frame; their systems join the simulation after the engine's own.
    plugin::PluginManager plugins({.target = "player"});
    plugins.load_directory(proj->root() / "plugins");
    if (!plugins.function_names().empty()) {
        so.plugin_call = [&plugins](std::string_view name, std::string_view args) { return plugins.call(name, args); };
        so.plugin_has = [&plugins](std::string_view name) { return plugins.has_function(name); };
    }
    auto scripts = script::ScriptHost::create(so);
    if (!scripts) return std::move(scripts).error();
    (*scripts)->set_current_scene(rel);
    scripts_for_scenes = scripts->get();
    // Worker threads for physics (and future parallel systems); 0 = hardware_concurrency - 1.
    jobs::JobSystem workers(0);
    // Per-draw visibility of large frames runs on the same pool (ADR-0228); the renderer only uses
    // it inside render(), which never runs after `workers` is destroyed.
    view->renderer().set_job_system(&workers);
    // Declared after `workers`, so the loader detaches (waiting for decodes) before the pool dies.
    struct DetachLoader {
        runtime::GpuAssetCache& cache;
        ~DetachLoader() { cache.set_async_loading(nullptr); }
    } detach_loader{cache};
    if (proj->settings().asset_budget_mb > 0 || proj->settings().texture_streaming)
        cache.set_residency({.budget_bytes = static_cast<std::size_t>(proj->settings().asset_budget_mb) << 20,
                             .mip_streaming = proj->settings().texture_streaming,
                             .screen_height = static_cast<std::uint32_t>(std::max(wd.height, 1))});
    if (o.async_assets && o.max_frames == 0 && o.replay.empty() && o.record.empty()) cache.set_async_loading(&workers);
    // World streaming (6h, ADR-0090): a WorldStreaming entity in the scene names the .vworld whose
    // cells load around streaming sources (the camera by default). Deterministic runs (frame
    // counts, captures) read cells synchronously so every run streams the same cells each frame.
    const bool deterministic = o.max_frames != 0 || !o.replay.empty() || !o.record.empty();
    std::unique_ptr<world::WorldStreamer> streamer;
    std::uint32_t peak_cells = 0;
    auto start_world = [&]() -> Status {
        streamer.reset();
        std::string world_path;
        scene.world().each<world::WorldStreamingComponent>([&](const world::WorldStreamingComponent& w) {
            if (world_path.empty()) world_path = w.world;
        });
        if (world_path.empty()) return {};
        auto file = proj->resolve(world_path);
        if (!file) return std::move(file).error();
        auto desc = world::load_world_file(*file);
        if (!desc) return std::move(desc).error();
        VICTUS_LOG_INFO("player", "streaming world '{}' ({} cells)", desc->name, desc->cells.size());
        streamer = std::make_unique<world::WorldStreamer>(std::move(*desc), proj->root(), deterministic ? nullptr : &workers,
                                                          runtime::world_dependencies(cache));
        return {};
    };
    VICTUS_TRY_STATUS(start_world());
    auto stream_world = [&] {
        if (world_changed) {
            world_changed = false;
            if (auto st = start_world(); !st) VICTUS_LOG_ERROR("player", "world streaming: {}", st.error().message);
        }
        if (!streamer) return;
        VICTUS_PROFILE_SCOPE("world.stream");
        scene.update_transforms();
        std::optional<math::Vec3> camera;
        scene.world().each<scene::CameraComponent, scene::WorldTransform>(
            [&](const scene::CameraComponent&, const scene::WorldTransform& t) {
                if (!camera) camera = t.matrix.transform_point({});
            });
        const auto sources = world::gather_sources(scene, camera ? &*camera : nullptr);
        streamer->update(scene, sources);
        peak_cells = std::max(peak_cells, streamer->stats().active);
    };
    physics::PhysicsSettings ps;
    ps.jobs = &workers;
    auto physics_world = physics::PhysicsWorld::create(ps);
    if (!physics_world) return std::move(physics_world).error();
    physics_for_scenes = physics_world->get();
    (*physics_world)->set_mesh_provider(assets::collision_mesh_provider(**db));
    PlatformInput input((*plat)->input());
    bool text_input_on = false;
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    gameui::add_ui_systems(sim);
    script::add_script_systems(sim, **scripts);
    physics::add_physics_systems(sim, **physics_world);
    animation::AnimationSystem animations(
        {.provider = assets::animation_provider(**db),
         .controllers = animation::directory_loader((*db)->assets_dir()),
         .sheets = assets::sprite_sheet_provider(**db), .jobs = &workers,
         .ground = runtime::physics_ground_probe(**physics_world)});
    animation::add_animation_systems(sim, animations);
    tilemap::TilemapSystem tilemaps(assets::tileset_provider(**db));
    tilemap::add_tilemap_systems(sim, tilemaps);
    terrain::TerrainSystem terrains(assets::terrain_provider(**db));
    terrain::add_terrain_systems(sim, terrains);
    particles::ParticleSystem particle_system({.jobs = &workers});
    particles::add_particle_systems(sim, particle_system);
    navigation::NavigationSystem nav;
    navigation::add_navigation_systems(sim, nav);
    ai::AiSystem ai_system(ai::directory_loader((*db)->assets_dir()));
    ai::add_ai_systems(sim, ai_system);
    procgen::add_procgen_systems(sim);
    traffic::add_traffic_systems(sim, traffic::directory_loader((*db)->assets_dir()));
    sequencer::SequencerSystem sequencer_system(sequencer::directory_loader((*db)->assets_dir()));
    sequencer::add_sequencer_systems(sim, sequencer_system);
    plugins.attach(sim);
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
    for (const auto& d : sim.schedule_diagnostics()) VICTUS_LOG_WARN("player", "schedule: {}", d);
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
    if (o.hot_reload && !packaged) {
        reloader = std::make_unique<runtime::AssetHotReloader>(**db);
        reloader->set_before_refresh([&cache] { cache.wait_loads(); }); // decodes read the records refresh() rewrites
    }
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
        // The server's level travel loads the same scene here (ADR-0259).
        co.on_travel = [&scene, host = scripts->get()](std::string_view level, const json::Value& params) {
            return host->load_scene(scene, level, params);
        };
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
        server_for_scenes = server.get();
        VICTUS_LOG_INFO("player", "hosting on UDP port {} as '{}' ({})", uo.port, o.player_name, security);
    }
    PlayerReport report;
    report.backend = dev->capabilities().driver_name;
    report.packaged = packaged;
    report.plugins_loaded = plugins.loaded_count();
    report.quality = std::string(render::quality_tier_name(*tier));
    report.hdr_format = hdr_format;
    report.device_class = device_class;
    VICTUS_LOG_INFO("player", "{}: {} on {}", proj->settings().name, rel, report.backend);

    rhi::TextureHandle frame;
    std::uint32_t fw = 0, fh = 0;
    const double start = platform::Platform::seconds();
    double last = start;
    // Frame cap (interactive runs with a target frame rate): sleep the rest of each frame period
    // instead of rendering at the panel's vsync rate. A 90 Hz phone would otherwise render 90
    // frames a second it cannot sustain, heat up and throttle; holding the target steadily is
    // what keeps a low-end GPU cool. Dynamic resolution is fed the frame's busy time (excluding
    // this sleep) so the cap does not read as a slow frame.
    const bool frame_cap = target_fps > 0 && o.max_frames == 0 && o.replay.empty() && o.record.empty();
    const double frame_period = frame_cap ? 1.0 / static_cast<double>(target_fps) : 0.0;
    double frame_begin = start;
    float busy_ms = 0.0f;
    Status result;
    LifecycleGate lifecycle;
    std::optional<profiler::FrameProfiler> frame_profiler;
    // Last rendered frame, for the deterministic counters in the --profile report.
    runtime::SceneViewStats last_view;
    std::uint64_t last_view_draws = 0;
    std::uint32_t last_view_passes = 0;
    if (!o.profile.empty() || !o.trace.empty()) {
        frame_profiler.emplace(600);
        if (!o.trace.empty()) frame_profiler->record(600);
    }
    // GPU device loss (ADR-0157): the scene, scripts, physics, audio and network session live on
    // the CPU and survive; only GPU objects are rebuilt. Recovery runs at the top of a frame, when
    // no command buffer of the lost device exists any more.
    const rhi::PresentMode present_mode = proj->settings().vsync ? rhi::PresentMode::VSync : rhi::PresentMode::Immediate;
    std::uint32_t recoveries_left = o.device_recoveries;
    bool device_lost = false;
    const auto recover_device = [&]() -> Status {
        const std::string reason = dev->lost_reason();
        VICTUS_LOG_WARN("player", "{}; recreating the GPU device ({} of {} recoveries left)", reason, recoveries_left - 1,
                        o.device_recoveries);
        --recoveries_left;
        const render::RenderSettings settings = view->renderer().settings(); // includes the dynamic resolution scale
        // Old objects go first, on the old device: the view, the frame target, every cached asset.
        view.reset();
        if (frame) dev->destroy(frame);
        frame = {};
        fw = fh = 0;
        cache.wait_loads();
        cache.clear();
        gpu.reset();
        dev->detach_window(**window);
        device.reset();
        dev = nullptr;
        // A driver reset can take a few seconds: keep trying for about ten.
        rhi::DeviceDesc again = device_desc;
        again.env_fault_injection = false;
        std::string create_error;
        for (int attempt = 0; attempt < 20 && !device; ++attempt) {
            if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto d = rhi::Device::create(again);
            if (d) device = std::move(d).value();
            else create_error = d.error().message;
        }
        if (!device) return make_error(ErrorCode::DeviceError, reason + "; recreating the device failed: " + create_error);
        dev = device.get();
        // GLES needs an OpenGL window; switching between GLES and Vulkan/Metal would need a new window.
        if ((dev->capabilities().backend == rhi::Backend::GLES) != wd.opengl)
            return make_error(ErrorCode::DeviceError, reason + "; the new device uses a different graphics API");
        if (auto st = dev->attach_window(**window, present_mode); !st)
            return make_error(ErrorCode::DeviceError, reason + "; attaching the window to the new device failed: " + st.error().message);
        auto g = render::GpuContext::create(*dev);
        if (!g) return make_error(ErrorCode::DeviceError, reason + "; " + g.error().message);
        gpu = std::move(g).value();
        cache.rebind(*gpu);
        auto v = runtime::SceneView::create(*gpu, rhi::Format::RGBA8_UNORM, renderer_options);
        if (!v) return make_error(ErrorCode::DeviceError, reason + "; " + v.error().message);
        view = std::move(v).value();
        view->renderer().set_settings(settings);
        view->renderer().set_job_system(&workers);
        ++report.device_recoveries;
        VICTUS_LOG_INFO("player", "GPU device recreated: {}", dev->capabilities().driver_name);
        return {};
    };
    bool master_was_muted = false;
    for (;;) {
        for (const auto& ev : (*plat)->poll_events()) {
            if (ev.type == platform::EventType::AppLowMemory) VICTUS_LOG_WARN("player", "the OS reports low memory");
            if (!lifecycle.on_event(ev.type)) continue;
            if (audio_engine) {
                if (lifecycle.suspended()) {
                    master_was_muted = (*audio_engine)->bus_muted(audio::Bus::Master);
                    (*audio_engine)->set_bus_muted(audio::Bus::Master, true);
                } else {
                    (*audio_engine)->set_bus_muted(audio::Bus::Master, master_was_muted);
                }
            }
            VICTUS_LOG_INFO("player", "{}", lifecycle.suspended() ? "suspended: app in the background" : "resumed");
            if (!lifecycle.suspended()) last = platform::Platform::seconds(); // no time jump on resume
        }
        if (frame_profiler) frame_profiler->frame();
        if ((*plat)->quit_requested()) break;
        if (lifecycle.suspended()) {
            // The surface may be gone: render nothing. Networked sessions keep simulating so the
            // connection and the shared world stay alive; offline games simply pause.
            const double now = platform::Platform::seconds();
            if (server || client) sim.tick(scene, std::clamp(static_cast<float>(now - last), 1e-4f, 0.25f));
            last = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        // Escape quits windowed runs, unless it is ending a text field edit.
        if ((*plat)->input().key_pressed(platform::Key::Escape) && !o.fullscreen && o.max_frames == 0 &&
            gameui::editing(scene).is_null())
            break;
        if (frame_cap) {
            const double t = platform::Platform::seconds();
            busy_ms = static_cast<float>((t - frame_begin) * 1000.0);
            const double wait = frame_begin + frame_period - t;
            if (wait > 0.001) std::this_thread::sleep_for(std::chrono::duration<double>(wait - 0.0005));
            const double after = platform::Platform::seconds();
            // Keep a fixed cadence; resynchronise after a long frame instead of bursting to catch up.
            frame_begin = after - frame_begin > 2.0 * frame_period ? after : frame_begin + frame_period;
            if (frame_begin > after + frame_period) frame_begin = after;
        }
        const double now = platform::Platform::seconds();
        if (reloader) {
            const auto changes = reloader->poll(now);
            if (!changes.empty()) {
                runtime::AssetHotReloader::apply(changes, &cache, &animations, &tilemaps, &terrains, physics_world->get());
                if (audio_engine)
                    for (const auto& clip : changes.audio) (void)(*audio_engine)->reload_clip(clip);
                if (!changes.prefabs.empty()) (*scripts)->forget_prefabs(); // new instances use the new version
                for (const auto& t : changes.behavior_trees) ai_system.reload(t);
                for (const auto& q : changes.sequences) sequencer_system.reload(q);
                if (!changes.string_tables.empty()) (void)(*scripts)->reload_localization();
                report.assets_reloaded += changes.reimported.size() + changes.audio.size() + changes.prefabs.size() + changes.materials.size() + changes.shaders.size() + changes.sprite_sheets.size() + changes.tilesets.size() + changes.terrains.size() + changes.behavior_trees.size() + changes.sequences.size() + changes.string_tables.size() + changes.anim_controllers.size();
            }
        }
        if (dynamic_resolution) {
            const float before = dynamic_resolution->scale();
            const float scale = dynamic_resolution->update(frame_cap ? busy_ms : static_cast<float>(now - last) * 1000.0f);
            if (scale != before) {
                render::RenderSettings rs = view->renderer().settings();
                rs.render_scale = scale;
                view->renderer().set_settings(rs);
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
        // Platform text input (IME, on-screen keyboard on phones) only while a UI text field edits.
        if (const bool want_text = !gameui::editing(scene).is_null(); want_text != text_input_on) {
            (*window)->set_text_input(want_text);
            text_input_on = want_text;
        }
        if (replay) {
            if (!replay->end_tick(scene)) VICTUS_LOG_WARN("player", "replay diverged at {}", replay->mismatch());
        } else if (!o.record.empty()) {
            captured_input.end_frame();
            recording.after_tick(scene);
        }
        stream_world();
        view->renderer().set_frame_time(dt);
        // Surface-shader time follows game time, not the wall clock: animated materials pause
        // with the game and fixed-frame runs render the same pixels every time (ADR-0102).
        view->renderer().set_time(sim.time().time);

        if (device_lost) {
            device_lost = false;
            if (recoveries_left == 0) {
                result = make_error(ErrorCode::DeviceError, dev->lost_reason());
                break;
            }
            if (auto st = recover_device(); !st) {
                result = std::move(st).error();
                break;
            }
        }
        auto cmd = dev->begin_commands();
        std::uint32_t sw = 0, sh = 0;
        const rhi::TextureHandle swap = cmd.acquire_swapchain(**window, sw, sh);
        if (!swap) {
            cmd.submit();
            device_lost = dev->lost();
            continue;
        }
        if (!frame || fw != sw || fh != sh) {
            if (frame) dev->destroy(frame);
            rhi::TextureDesc d;
            d.name = "player_frame";
            d.width = fw = sw;
            d.height = fh = sh;
            d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
            auto t = dev->create_texture(d);
            if (!t) {
                result = std::move(t).error();
                cmd.submit();
                break;
            }
            frame = *t;
        }
        VICTUS_PROFILE_SCOPE("frame.render");
        const std::uint64_t draws_before = dev->stats().draw_calls;
        auto rendered = view->render(cmd, scene, cache, frame, fw, fh);
        if (!rendered) {
            result = std::move(rendered).error();
            cmd.submit();
            break;
        }
        if (dev->lost()) { // submissions kept failing while the swapchain still worked
            cmd.submit();
            device_lost = true;
            continue;
        }
        last_view = *rendered;
        last_view_draws = dev->stats().draw_calls - draws_before;
        last_view_passes = rendered->mode == runtime::ViewMode::ThreeD
                               ? static_cast<std::uint32_t>(view->renderer().graph_info().executed_passes.size())
                               : 0;
        cmd.blit(frame, swap);
        ++report.frames;
        const bool last_frame = o.max_frames && report.frames >= o.max_frames;
        // Frame sequence for perceptual comparison (ADR-0102): every capture_every-th frame of a
        // fixed-step run, so two runs of the same project write comparable sequences.
        const bool capture = !o.capture_dir.empty() && o.max_frames && report.frames % std::max<std::uint32_t>(o.capture_every, 1) == 0;
        if (!last_frame && !capture) {
            cmd.submit();
            continue;
        }
        cmd.submit_and_wait();
        const auto save_frame = [&](const fs::Path& png) -> Status {
            auto px = dev->read_texture(frame);
            if (!px) return std::move(px).error();
            render::Image img{fw, fh, std::move(px).value()};
            for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
            return render::save_png(png, img);
        };
        if (capture) {
            std::error_code dir_ec;
            std::filesystem::create_directories(o.capture_dir, dir_ec);
            char name[32];
            std::snprintf(name, sizeof name, "frame_%05llu.png", static_cast<unsigned long long>(report.frames));
            if (auto st = save_frame(o.capture_dir / name); !st) {
                result = std::move(st);
                break;
            }
            ++report.frames_captured;
        }
        if (!last_frame) continue;
        if (!o.screenshot.empty()) result = save_frame(o.screenshot);
        break;
    }
    report.assets_streamed = cache.async_stats().uploaded;
    // view and dev are null when a device-loss recovery failed part way (ADR-0157).
    if (view && last_view.mode == runtime::ViewMode::ThreeD) report.render_path = view->renderer().path_report();
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
    if (frame_profiler) {
        frame_profiler->frame();
        report.profile = frame_profiler->report();
        const rhi::DeviceStats ds = dev ? dev->stats() : rhi::DeviceStats{};
        auto& c = report.profile.counters;
        c.emplace_back("gpu.texture_bytes", static_cast<double>(ds.texture_bytes));
        c.emplace_back("gpu.buffer_bytes", static_cast<double>(ds.buffer_bytes));
        c.emplace_back("gpu.live_textures", static_cast<double>(ds.live_textures));
        c.emplace_back("gpu.live_buffers", static_cast<double>(ds.live_buffers));
        if (ds.frames_submitted > 0)
            c.emplace_back("gpu.draw_calls_per_frame", static_cast<double>(ds.draw_calls) / static_cast<double>(ds.frames_submitted));
        // Deterministic counters of the last frame (same scene, frame count and size give the
        // same values on every machine); tests/perf/budgets.json gates them.
        const auto& ss = last_view.scene;
        c.emplace_back("frame.draw_calls", static_cast<double>(last_view_draws));
        c.emplace_back("frame.render_passes", static_cast<double>(last_view_passes));
        c.emplace_back("frame.triangles", static_cast<double>(ss.triangles));
        c.emplace_back("frame.shadow_triangles", static_cast<double>(ss.shadow_triangles));
        c.emplace_back("frame.submitted", static_cast<double>(ss.submitted));
        c.emplace_back("frame.visible", static_cast<double>(ss.visible));
        c.emplace_back("frame.culled", static_cast<double>(ss.culled));
        c.emplace_back("frame.shadow_casters", static_cast<double>(ss.shadow_casters));
        c.emplace_back("frame.reflection_probes", static_cast<double>(ss.reflection_probes));
        c.emplace_back("frame.main_batches", static_cast<double>(ss.main_batches));
        c.emplace_back("frame.shadow_batches", static_cast<double>(ss.shadow_batches + ss.local_shadow_draws));
        c.emplace_back("frame.transparent_draws", static_cast<double>(ss.transparent_draws));
        c.emplace_back("frame.transparent_triangles", static_cast<double>(ss.transparent_triangles));
        c.emplace_back("frame.alpha_tested_draws", static_cast<double>(ss.alpha_tested_draws));
        c.emplace_back("frame.surface_shader_draws", static_cast<double>(ss.surface_shader_draws));
        c.emplace_back("frame.sprites", static_cast<double>(last_view.sprites));
        c.emplace_back("frame.ui_items", static_cast<double>(last_view.ui.items));
        c.emplace_back("scene.entities", static_cast<double>(scene.size()));
        c.emplace_back("sim.systems", static_cast<double>(sim.systems().size()));
        if (const auto* lod = gameplay::simulation_lod_stats(scene)) {
            c.emplace_back("sim.lod_entities", static_cast<double>(lod->entities));
            c.emplace_back("sim.lod_due", static_cast<double>(lod->due));
            for (int ti = 0; ti < gameplay::kSimulationLodTiers; ++ti)
                c.emplace_back(std::string("sim.lod_tier") + static_cast<char>('0' + ti),
                               static_cast<double>(lod->tiers[static_cast<std::size_t>(ti)]));
        }
        if (memory::allocation_tracking_enabled()) {
            const auto ms = memory::allocation_stats();
            c.emplace_back("cpu.live_bytes", static_cast<double>(ms.live_bytes));
            c.emplace_back("cpu.peak_live_bytes", static_cast<double>(ms.peak_live_bytes));
            c.emplace_back("cpu.allocations", static_cast<double>(ms.allocations));
        }
        if (!o.profile.empty())
            if (auto st = fs::write_text_atomic(o.profile, profiler::report_json(report.profile)); !st && result.ok()) result = std::move(st).error();
        if (!o.trace.empty()) {
            frame_profiler->stop_recording(); // a shorter session keeps what was recorded
            auto zones = frame_profiler->take_recording();
            if (auto st = profiler::write_chrome_trace(o.trace, zones); !st && result.ok()) result = std::move(st).error();
        }
    }
    if (dev) {
        dev->wait_idle();
        if (frame) dev->destroy(frame);
    }
    report.seconds = platform::Platform::seconds() - start;
    report.script_errors = (*scripts)->stats().errors;
    report.scene_loads = (*scripts)->stats().scene_loads;
    report.final_scene = (*scripts)->current_scene();
    if (streamer) {
        report.world_cells_peak = peak_cells;
        report.world_cell_loads = streamer->stats().loads;
    }
    if (!result.ok()) return std::move(result).error();
    return report;
}

} // namespace victus::player
