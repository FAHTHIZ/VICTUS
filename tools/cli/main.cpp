// SPDX-License-Identifier: Apache-2.0
// `victus` command-line tool: project creation, asset import, running games and inspection
// from a terminal or CI. Shares the project, asset and player modules with the editor.
#include "victus/gameui/gameui.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/player/export.hpp"
#include "victus/player/player.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/platform/platform.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <thread>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace victus;

namespace {

constexpr const char* kUsage =
    "usage: victus <command> [options]\n"
    "\n"
    "commands:\n"
    "  new <folder> [--template 3d|2d|simulation|multiplayer] [--name NAME]\n"
    "  templates                                                  list project templates\n"
    "  info <project>                                             show project settings, scenes, assets\n"
    "  import <project>                                           import changed assets (exit 1 on failure)\n"
    "  run <project> [--scene S] [--frames N] [--screenshot PNG] [--quality TIER]  play a scene\n"
    "      [--connect HOST[:PORT]] [--name NAME]                  ...as a client of a `victus serve` server\n"
    "      [--host] [--port P] [--max-clients N]                  ...host a session and play in it (listen server)\n"
    "      [--server-key HEX] [--password P] [--insecure]         ...pin the server key / join a protected server\n"
    "      [--record F.vcapture] [--replay F.vcapture]            ...record a play session, or watch one replay\n"
    "  simulate <project> [--frames N] [--hold KEYS] [--save F]   run gameplay and scripts headless (no GPU)\n"
    "           [--record F.vcapture] [--replay F.vcapture]         ...record a session, or replay one and verify it\n"
    "           [--connect HOST[:PORT]]                           ...as a headless multiplayer client\n"
    "  serve <project> [--port P] [--max-clients N] [--frames N]  host a multiplayer session (dedicated server)\n"
    "        [--password P] [--key FILE] [--insecure]              ...encrypted by default; prints its public key\n"
    "  check <project>                                            validate scenes and script syntax\n"
    "  export <project> --out DIR [--name EXE] [--player PATH]    package a runnable game folder\n"
    "  version                                                    print the engine version\n"
    "\n"
    "Run `victus <command> --help` for command options. The editor is `victus_editor`.\n";

int error(const std::string& message) {
    std::fprintf(stderr, "victus: %s\n", message.c_str());
    return 1;
}

std::vector<std::string> rest(int argc, char** argv) {
    std::vector<std::string> v;
    for (int i = 2; i < argc; ++i) v.emplace_back(argv[i]);
    return v;
}

bool wants_help(const std::vector<std::string>& a) {
    for (const auto& s : a)
        if (s == "--help" || s == "-h") return true;
    return false;
}

int cmd_new(const std::vector<std::string>& a) {
    cli::Parser p("victus new", "Create a project from a template");
    p.positional("folder", "new project folder", true)
        .option("template", "3d, 2d, simulation or multiplayer", "3d")
        .option("name", "project name (default: folder name)");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message + "\n" + p.help());
    const fs::Path dir = p.get("folder");
    const std::string name = p.has("name") ? p.get("name") : dir.filename().string();
    auto proj = project::Project::create(dir, name, p.get("template"));
    if (!proj) return error(proj.error().message);
    std::printf("Created %s project '%s' in %s\n", p.get("template").c_str(), name.c_str(), proj->root().string().c_str());
    std::printf("Open it with: victus_editor \"%s\"   or run it with: victus run \"%s\"\n", proj->root().string().c_str(),
                proj->root().string().c_str());
    return 0;
}

int cmd_templates() {
    for (const auto& t : project::templates()) std::printf("%-12s %-12s %s\n", t.id.c_str(), t.display_name.c_str(), t.description.c_str());
    return 0;
}

int cmd_info(const std::vector<std::string>& a) {
    cli::Parser p("victus info", "Show a project");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    const auto& s = proj->settings();
    std::printf("name:          %s\nroot:          %s\ntemplate:      %s\nengine:        %s\nstartup scene: %s\nwindow:        %dx%d vsync=%s\n",
                s.name.c_str(), proj->root().string().c_str(), s.template_id.c_str(), s.engine_version.c_str(),
                s.startup_scene.c_str(), s.window_width, s.window_height, s.vsync ? "on" : "off");
    std::printf("scenes:\n");
    for (const auto& sc : proj->scenes()) std::printf("  %s\n", sc.c_str());
    auto db = assets::AssetDatabase::open(proj->root());
    if (!db) return error(db.error().message);
    const auto recs = (*db)->records();
    std::printf("assets: %zu\n", recs.size());
    for (const auto& r : recs) {
        auto up = (*db)->is_up_to_date(r.uuid);
        std::printf("  %-8s %-40s %s\n", assets::to_string(r.kind), r.path.c_str(),
                    r.importer.empty() ? "" : (up && *up) ? "imported" : "needs import");
    }
    return 0;
}

int cmd_import(const std::vector<std::string>& a) {
    cli::Parser p("victus import", "Import changed assets");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    assets::RefreshReport refresh;
    auto db = assets::AssetDatabase::open(proj->root(), &refresh);
    if (!db) return error(db.error().message);
    for (const auto& s : refresh.added) std::printf("new asset: %s\n", s.c_str());
    for (const auto& s : refresh.errors) std::printf("metadata error: %s\n", s.c_str());
    jobs::JobSystem jobs;
    const auto r = (*db)->import_all(&jobs);
    for (const auto& m : r.messages) std::printf("%s\n", m.c_str());
    std::printf("%zu imported, %zu up to date, %zu failed\n", r.imported, r.up_to_date, r.failed);
    return r.failed || !refresh.errors.empty() ? 1 : 0;
}

int cmd_run(const std::vector<std::string>& a) {
    cli::Parser p("victus run", "Play a project");
    p.positional("project", "project folder", true)
        .option("scene", "project-relative scene (default: startup scene)")
        .option("frames", "exit after N frames (deterministic 1/60 s steps)", "0")
        .option("screenshot", "write the last frame to this PNG (with --frames)")
        .option("width", "window width (default: project setting)", "0")
        .option("height", "window height (default: project setting)", "0")
        .option("quality", "rendering quality: low, medium, high or ultra (default: project setting)")
        .flag("fullscreen", "run fullscreen")
        .flag("no-hot-reload", "do not reload scripts, textures, models and audio when their files change")
        .flag("strict", "exit with code 3 if any script raised an error")
        .option("connect", "join a `victus serve` server at host[:port]")
        .flag("host", "host a multiplayer session and play in it (listen server)")
        .option("port", "with --host: UDP port to listen on", std::to_string(net::kDefaultPort))
        .option("max-clients", "with --host: players who may join", "8")
        .option("key", "with --host: secret key file, created if missing (default: <project>/.victus/server.key)")
        .option("name", "player name shown to others when connected", "player")
        .option("server-key", "with --connect: refuse servers whose public key differs (printed by `victus serve`)")
        .option("password", "with --connect: password of a protected server; with --host: require it")
        .flag("insecure", "no encryption: join a server started with --insecure, or host one")
        .option("conditions", "emulate a bad link, e.g. latency=80,jitter=10,loss=0.05")
        .option("saves", "save games folder (default: <project>/.victus/saves)")
        .option("record", "record input, frame times and state digests to this .vcapture file")
        .option("replay", "replay a .vcapture file (exit 5 if the game state diverges)");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    player::PlayerOptions o;
    o.record = p.get("record");
    o.replay = p.get("replay");
    o.connect = p.get("connect");
    o.player_name = p.get("name");
    o.net_conditions = p.get("conditions");
    o.server_key = p.get("server-key");
    o.net_password = p.get("password");
    o.insecure = p.has("insecure");
    o.host = p.has("host");
    o.host_port = static_cast<std::uint16_t>(p.get_number("port", net::kDefaultPort));
    o.max_clients = static_cast<std::uint16_t>(p.get_number("max-clients", 8));
    o.host_key = p.get("key");
    o.project = p.get("project");
    o.scene = p.get("scene");
    o.max_frames = static_cast<std::uint64_t>(p.get_number("frames", 0));
    o.screenshot = p.get("screenshot");
    o.width = static_cast<int>(p.get_number("width", 0));
    o.height = static_cast<int>(p.get_number("height", 0));
    o.fullscreen = p.has("fullscreen");
    o.hot_reload = !p.has("no-hot-reload");
    o.quality = p.get("quality");
    o.saves = p.get("saves");
    auto r = player::run(o);
    if (!r) return error(r.error().message);
    std::printf("ran %llu frames in %.2f s on %s at %s quality (%llu script errors)\n",
                static_cast<unsigned long long>(r->frames), r->seconds, r->backend.c_str(), r->quality.c_str(),
                static_cast<unsigned long long>(r->script_errors));
    if (!r->net_status.empty()) std::printf("net: %s\n", r->net_status.c_str());
    if (r->assets_reloaded) std::printf("hot reload: %zu assets reloaded\n", r->assets_reloaded);
    if (r->scene_loads)
        std::printf("scenes: %llu transitions, ended in %s\n", static_cast<unsigned long long>(r->scene_loads), r->final_scene.c_str());
    if (!o.record.empty()) std::printf("recorded capture: %s\n", o.record.string().c_str());
    if (!o.replay.empty()) {
        if (!r->replay_mismatch.empty()) return (void)error("replay diverged at " + r->replay_mismatch), 5;
        std::printf("replay: %llu state digests match\n", static_cast<unsigned long long>(r->digests_checked));
    }
    return p.has("strict") && r->script_errors ? 3 : 0;
}

std::string entity_name(const scene::Scene& s, scene::Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? n->value : std::string("<unnamed>");
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t c = s.find(',', start);
        const std::string item = s.substr(start, c == std::string::npos ? std::string::npos : c - start);
        if (!item.empty()) out.push_back(item);
        if (c == std::string::npos) break;
        start = c + 1;
    }
    return out;
}

// The game simulation of `victus simulate` and `victus serve`: gameplay, UI, scripts, physics,
// animation, particles, navigation and (device-less) audio on a scene, without a window or GPU.
struct Headless {
    std::optional<project::Project> proj;
    std::string scene_rel;
    scene::Scene scene;
    std::unique_ptr<script::ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> physics_world;
    std::unique_ptr<assets::AssetDatabase> db;
    std::unique_ptr<animation::AnimationSystem> animations;
    std::unique_ptr<tilemap::TilemapSystem> tilemaps;
    particles::ParticleSystem particle_system; // single-threaded like physics; identical either way
    navigation::NavigationSystem nav;
    std::unique_ptr<audio::AudioEngine> audio_engine;
    gameplay::ManualInput input;
    gameplay::Simulation sim;

    Status open(const std::string& project_dir, const std::string& scene_arg, const std::string& saves = {},
                std::uint64_t random_seed = 0) {
        project::register_components();
        auto pr = project::Project::open(project_dir);
        if (!pr) return pr.error();
        proj.emplace(std::move(*pr));
        scene_rel = scene_arg.empty() ? proj->settings().startup_scene : scene_arg;
        auto path = scene_arg.empty() ? proj->startup_scene_path() : proj->resolve(scene_arg);
        if (!path) return path.error();
        if (auto st = scene.load_file(*path); !st) return st;
        script::HostOptions so;
        so.assets_root = proj->root() / "assets";
        so.input_map = proj->settings().input;
        so.save_version = proj->settings().save_version;
        so.save_directory = saves.empty() ? proj->dev_save_directory() : fs::Path(saves);
        so.random_seed = random_seed;
        so.project_root = proj->root();
        so.bridge_launcher = [project = &*proj, save_dir = so.save_directory](std::string_view name) {
            return project::bridge_launch(*project, name, save_dir);
        };
        so.on_scene_loaded = [this](scene::Scene&, std::string_view loaded) {
            scene_rel = std::string(loaded);
            if (physics_world) physics_world->clear();
            if (audio_engine) audio_engine->stop_all();
        };
        auto h = script::ScriptHost::create(so);
        if (!h) return h.error();
        host = std::move(*h);
        host->set_current_scene(scene_rel);
        auto pw = physics::PhysicsWorld::create(); // single-threaded: reproducible output
        if (!pw) return pw.error();
        physics_world = std::move(*pw);
        gameplay::add_builtin_systems(sim);
        gameui::add_ui_systems(sim);
        script::add_script_systems(sim, *host);
        physics::add_physics_systems(sim, *physics_world);
        // Animation needs cooked models: open the asset database when the project has one.
        if (auto d = assets::AssetDatabase::open(proj->root())) db = std::move(*d);
        animations = std::make_unique<animation::AnimationSystem>(
            animation::AnimationSettings{.provider = db ? assets::animation_provider(*db) : animation::SetProvider{},
                                         .sheets = db ? assets::sprite_sheet_provider(*db) : animation::SheetProvider{}});
        animation::add_animation_systems(sim, *animations);
        tilemaps = std::make_unique<tilemap::TilemapSystem>(db ? assets::tileset_provider(*db) : tilemap::TilesetProvider{});
        tilemap::add_tilemap_systems(sim, *tilemaps);
        particles::add_particle_systems(sim, particle_system);
        navigation::add_navigation_systems(sim, nav);
        audio::EngineOptions ao; // mixed without a device so audio calls behave as in the game
        ao.assets_root = proj->root() / "assets";
        ao.null_device = true;
        auto ae = audio::AudioEngine::create(ao);
        if (!ae) return ae.error();
        audio_engine = std::move(*ae);
        audio::add_audio_systems(sim, *audio_engine);
        sim.input = &input;
        return {};
    }
    [[nodiscard]] std::string game_id() const { return net::make_game_id(proj->settings().name, scene_rel); }
};

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

// Paces a loop at `hz` against the wall clock; returns seconds since start.
struct Pacer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    Clock::time_point next = start;
    double step;
    explicit Pacer(double hz) : step(1.0 / hz) {}
    void wait() {
        next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(step));
        const auto now = Clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > std::chrono::milliseconds(250)) next = now; // fell far behind: do not spiral
    }
};

Result<std::unique_ptr<net::Transport>> condition(std::unique_ptr<net::Transport> t, const std::string& spec) {
    if (spec.empty()) return t;
    auto c = net::parse_conditions(spec);
    if (!c) return c.error();
    return net::with_conditions(std::move(t), *c);
}

// Clients encrypt unless --insecure; --server-key pins the server's public key.
Result<std::unique_ptr<net::Transport>> secure_client(std::unique_ptr<net::Transport> t, const cli::Parser& p) {
    if (p.has("insecure")) {
        if (p.has("password") || p.has("server-key")) return make_error(ErrorCode::InvalidArgument, "--password/--server-key need encryption; remove --insecure");
        return t;
    }
    net::SecureOptions so;
    if (p.has("server-key")) {
        auto k = net::key_from_hex(p.get("server-key"));
        if (!k) return make_error(ErrorCode::InvalidArgument, "--server-key: " + k.error().message);
        so.expected_server_key = *k;
    }
    so.password = p.get("password");
    return net::with_encryption(std::move(t), std::move(so));
}

void print_net(const net::NetContext& ctx) {
    const auto d = ctx.diagnostics();
    std::printf("net: %s (%s), %u clients, %u replicated entities, rtt %.1f ms, sent %.1f KB, received %.1f KB, "
                "snapshots %llu sent / %llu received (%llu delta), last snapshot %u bytes, %llu corrections, %llu rejected messages\n",
                d.status.c_str(), d.encrypted ? "encrypted" : "unencrypted", d.clients, d.entities, d.rtt_ms, static_cast<double>(d.bytes_sent) / 1024.0,
                static_cast<double>(d.bytes_received) / 1024.0, static_cast<unsigned long long>(d.snapshots_sent),
                static_cast<unsigned long long>(d.snapshots_received), static_cast<unsigned long long>(d.delta_snapshots), d.snapshot_bytes,
                static_cast<unsigned long long>(d.corrections), static_cast<unsigned long long>(d.rejected_messages));
}

// Runs a scene's gameplay and scripts at a fixed 1/60 s step without a window or GPU. Used for
// CI checks of game logic, simulation batch jobs and (with --connect) headless test clients.
int cmd_simulate(const std::vector<std::string>& a) {
    cli::Parser p("victus simulate", "Run a scene's gameplay and scripts headless");
    p.positional("project", "project folder", true)
        .option("scene", "project-relative scene (default: startup scene)")
        .option("frames", "number of 1/60 s frames to run", "600")
        .option("hold", "comma-separated controls held for the whole run, e.g. D,Space or Pad.LeftX+,Pad.South")
        .option("print", "comma-separated entity names whose final position is printed")
        .option("save", "write the final scene state to this .vscene file")
        .option("saves", "save games folder for the save API (default: <project>/.victus/saves)")
        .option("connect", "join a `victus serve` server at host[:port] (runs in real time)")
        .option("name", "player name sent to the server", "player")
        .option("server-key", "with --connect: refuse servers whose public key differs (printed by `victus serve`)")
        .option("password", "with --connect: password of a server started with --password")
        .flag("insecure", "with --connect: join a server started with --insecure (no encryption)")
        .option("conditions", "emulate a bad link, e.g. latency=80,jitter=10,loss=0.05,bandwidth=256")
        .option("expect-players", "with --connect: exit 4 unless at least N players were seen", "0")
        .option("record", "record input, frame times and state digests to this .vcapture file")
        .option("replay", "replay a .vcapture file headless; exit 5 at the first diverging state digest")
        .option("seed", "script random seed (math.random) for this run; recordings store it", "0");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    const bool recording = p.has("record"), replaying = p.has("replay");
    if ((recording || replaying) && p.has("connect")) return error("--record/--replay work offline only");
    if (recording && replaying) return error("choose either --record or --replay");
    std::optional<gameplay::Capture> capture;
    std::optional<gameplay::CaptureReplay> replay;
    gameplay::ReplayInput captured_input;
    std::uint64_t seed = static_cast<std::uint64_t>(p.get_number("seed", 0));
    if (replaying) {
        auto c = gameplay::Capture::load(p.get("replay"));
        if (!c) return error(c.error().message);
        capture = std::move(*c);
        seed = capture->seed;
    } else if (recording) {
        capture.emplace();
        if (!seed) seed = Random(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())).next_u64() | 1;
        capture->seed = seed;
    }
    auto hs = std::make_unique<Headless>();
    if (auto st = hs->open(p.get("project"), p.get("scene"), p.get("saves"), seed); !st) return error(st.error().message);
    if (replaying) {
        replay.emplace(*capture);
        hs->sim.input = &replay->input();
    } else if (recording) {
        capture->project = hs->proj->settings().name;
        capture->scene = hs->scene_rel;
        hs->sim.input = &captured_input;
    }
    for (const auto& k : split_list(p.get("hold"))) {
        auto b = gameplay::parse_binding(k);
        if (!b) return error("--hold: " + b.error().message);
        using Kind = gameplay::InputBinding::Kind;
        if (b->kind == Kind::Key) hs->input.set_key(k, true);
        else if (b->kind == Kind::Mouse) hs->input.set_mouse(b->index, true);
        else if (b->kind == Kind::GamepadButton) hs->input.set_gamepad_button(0, b->index, true);
        else hs->input.set_gamepad_axis(0, b->index, b->direction < 0 ? -1.0f : 1.0f);
    }

    std::unique_ptr<net::NetClient> client;
    if (p.has("connect")) {
        log::logger().add_sink(std::make_shared<log::ConsoleSink>(false));
        auto addr = net::parse_address(p.get("connect"));
        if (!addr) return error(addr.error().message);
        auto t = net::create_udp_transport({});
        if (!t) return error(t.error().message);
        auto tc = condition(std::move(*t), p.get("conditions"));
        if (!tc) return error(tc.error().message);
        auto ts = secure_client(std::move(*tc), p);
        if (!ts) return error(ts.error().message);
        net::ClientOptions co;
        co.host = addr->first;
        co.port = addr->second;
        co.name = p.get("name");
        co.game_id = hs->game_id();
        client = std::make_unique<net::NetClient>(std::move(*ts), co);
        client->install(hs->sim);
        std::signal(SIGINT, on_signal);
    }

    const auto frames = replaying ? static_cast<std::uint64_t>(capture->frames.size())
                                  : static_cast<std::uint64_t>(p.get_number("frames", 600));
    std::size_t most_players = 0;
    Pacer pacer(60.0);
    std::uint64_t f = 0;
    for (; f < frames && !g_stop.load(); ++f) {
        float dt = 1.0f / 60.0f;
        if (replaying) dt = replay->begin_tick();
        else if (recording) {
            const auto state = gameplay::sample_input(hs->input);
            capture->add_frame(state, dt);
            captured_input.set(state);
        }
        hs->sim.tick(hs->scene, dt);
        hs->input.end_frame();
        if (replaying && !replay->end_tick(hs->scene)) {
            ++f;
            break;
        }
        if (recording) {
            captured_input.end_frame();
            capture->after_tick(hs->scene);
        }
        if (client) {
            most_players = std::max(most_players, client->clients().size());
            if (client->failed()) break;
            pacer.wait();
        }
    }
    const auto& st = hs->host->stats();
    std::printf("simulated %llu frames (%.2f s game time): %zu entities, %zu script instances, %llu script errors\n",
                static_cast<unsigned long long>(f), hs->sim.time().time, hs->scene.world().alive_count(), st.instances,
                static_cast<unsigned long long>(st.errors));
    for (const auto& e : hs->host->recent_errors()) std::fprintf(stderr, "script error: %s\n", e.c_str());
    if (client) {
        print_net(*client);
        std::printf("net: client id %u, most players seen %zu\n", client->local_id(), most_players);
        for (const auto& c : client->clients()) {
            const auto score = c.player.is_null() ? json::Value{} : net::get_var(hs->scene, c.player, "score");
            std::printf("net: player %u '%s' score %s\n", c.id, c.name.c_str(), json::stringify(score, false).c_str());
        }
    }
    for (const auto& name : split_list(p.get("print"))) {
        const auto e = hs->scene.find_by_name(name);
        if (const auto* t = e != scene::kNullEntity ? hs->scene.world().get<scene::Transform>(e) : nullptr)
            std::printf("%s: %.3f %.3f %.3f\n", name.c_str(), t->position.x, t->position.y, t->position.z);
        else
            std::printf("%s: (not found)\n", name.c_str());
    }
    if (p.has("save"))
        if (auto s2 = hs->scene.save_file(p.get("save")); !s2) return error(s2.error().message);
    if (recording) {
        capture->finish(hs->scene);
        if (auto s2 = capture->save(p.get("record")); !s2) return error(s2.error().message);
        std::printf("recorded %zu frames, seed %016llx, %zu state digests -> %s\n", capture->frames.size(),
                    static_cast<unsigned long long>(seed), capture->digests.size(), p.get("record").c_str());
    }
    if (replaying) {
        if (replay->diverged()) return (void)error("replay diverged at " + replay->mismatch()), 5;
        std::printf("replay: %llu frames, %llu state digests match\n", static_cast<unsigned long long>(replay->frame()),
                    static_cast<unsigned long long>(replay->digests_checked()));
    }
    if (client) {
        const bool failed = client->failed();
        client->disconnect(hs->sim.time().time);
        if (failed) return (void)error("connection failed: " + client->diagnostics().status), 4;
        if (most_players < static_cast<std::size_t>(p.get_number("expect-players", 0)))
            return (void)error("expected at least " + p.get("expect-players") + " players, saw " + std::to_string(most_players)), 4;
    }
    return st.errors ? 3 : 0;
}

// Dedicated server: runs the scene headless in real time at 60 Hz and replicates it to clients.
int cmd_serve(const std::vector<std::string>& a) {
    cli::Parser p("victus serve", "Host a multiplayer session (headless dedicated server)");
    p.positional("project", "project folder", true)
        .option("scene", "project-relative scene (default: startup scene)")
        .option("port", "UDP port to listen on", std::to_string(net::kDefaultPort))
        .option("bind", "local address to bind (default: all interfaces)")
        .option("max-clients", "maximum connected players", "8")
        .option("frames", "stop after N frames (0 = until Ctrl+C)", "0")
        .option("conditions", "emulate a bad link on every client, e.g. latency=80,loss=0.05")
        .option("key", "server secret key file, created if missing (default: <project>/.victus/server.key)")
        .option("password", "players must send this password (it travels encrypted)")
        .flag("insecure", "accept unencrypted clients only (no keys, no passwords); for trusted networks")
        .option("expect-clients", "exit 4 unless at least N clients joined", "0");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    log::logger().add_sink(std::make_shared<log::ConsoleSink>(false)); // joins, leaves, rejects
    auto hs = std::make_unique<Headless>();
    if (auto st = hs->open(p.get("project"), p.get("scene")); !st) return error(st.error().message);
    net::UdpOptions uo;
    uo.port = static_cast<std::uint16_t>(p.get_number("port", net::kDefaultPort));
    uo.bind_address = p.get("bind");
    const auto max_clients = static_cast<std::uint16_t>(p.get_number("max-clients", 8));
    uo.max_peers = max_clients + 4u; // room to tell extra peers the server is full
    auto t = net::create_udp_transport(uo);
    if (!t) return error(t.error().message);
    auto tc = condition(std::move(*t), p.get("conditions"));
    if (!tc) return error(tc.error().message);
    std::unique_ptr<net::Transport> transport = std::move(*tc);
    std::string key_line = "unencrypted (--insecure): only use on networks you trust";
    if (p.has("insecure")) {
        if (p.has("password")) return error("--password needs encryption; remove --insecure");
    } else {
        const fs::Path key_file = p.has("key") ? fs::Path(p.get("key")) : hs->proj->root() / ".victus" / "server.key";
        auto kp = net::load_or_create_key(key_file);
        if (!kp) return error(kp.error().message);
        net::SecureOptions sec;
        sec.server_secret = kp->secret;
        sec.password = p.get("password");
        transport = net::with_encryption(std::move(transport), std::move(sec));
        key_line = "encrypted; server key " + net::key_to_hex(kp->public_key) + (p.has("password") ? " (password required)" : "");
    }
    net::ServerOptions so;
    so.game_id = hs->game_id();
    so.max_clients = max_clients;
    net::NetServer server(std::move(transport), so);
    server.install(hs->sim);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("serving '%s' (%s) on UDP port %u, up to %u players. Ctrl+C stops.\n",
                hs->proj->settings().name.c_str(), hs->scene_rel.c_str(), uo.port, max_clients);
    std::printf("%s\n", key_line.c_str());
    std::fflush(stdout);
    const auto frames = static_cast<std::uint64_t>(p.get_number("frames", 0));
    std::size_t most = 0;
    Pacer pacer(60.0);
    std::uint64_t f = 0;
    for (; (frames == 0 || f < frames) && !g_stop.load(); ++f) {
        hs->sim.tick(hs->scene, 1.0f / 60.0f);
        most = std::max(most, server.clients().size());
        if (f % (60 * 30) == 0 && f) print_net(server), std::fflush(stdout);
        pacer.wait();
    }
    server.shutdown(hs->sim.time().time);
    const auto& st = hs->host->stats();
    std::printf("served %llu frames (%.2f s): most players %zu, %llu script errors\n", static_cast<unsigned long long>(f),
                hs->sim.time().time, most, static_cast<unsigned long long>(st.errors));
    print_net(server);
    for (const auto& e : hs->host->recent_errors()) std::fprintf(stderr, "script error: %s\n", e.c_str());
    if (most < static_cast<std::size_t>(p.get_number("expect-clients", 0)))
        return (void)error("expected at least " + p.get("expect-clients") + " clients, saw " + std::to_string(most)), 4;
    return st.errors ? 3 : 0;
}

// Validates every scene (loads, no unknown components) and every script (syntax, module loads,
// referenced files exist). Exit 1 if anything is wrong.
int cmd_check(const std::vector<std::string>& a) {
    cli::Parser p("victus check", "Validate scenes, prefabs, materials, shaders, sprite sheets, tile sets and scripts");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    project::register_components();
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    const fs::Path assets = proj->root() / "assets";
    std::size_t problems = 0, scripts = 0, scenes = 0, prefabs = 0;
    const std::vector<std::string> project_scenes = proj->scenes(); // targets of scene.load
    for (const auto& msg : proj->settings().input.validate(
             [](std::string_view k) { return platform::key_from_name(k) != platform::Key::Unknown; })) {
        std::printf("error: victus.project: %s\n", msg.c_str());
        ++problems;
    }
    std::vector<std::string> prefab_files;
    std::set<Uuid> prefab_uuids;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(assets, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string rel = std::filesystem::relative(it->path(), assets).generic_string();
        if (it->path().extension() == scene::kPrefabExtension) {
            prefab_files.push_back(rel);
            if (auto src = script::read_prefab(assets, rel); src && !src->asset.is_nil()) prefab_uuids.insert(src->asset);
            continue;
        }
        if (it->path().extension() != ".luau" && it->path().extension() != ".vgraph") continue;
        ++scripts;
        auto raw = fs::read_text(it->path());
        auto text = raw ? script::luau_source(rel, std::move(*raw)) : Result<std::string>(std::move(raw).error());
        if (!text) {
            std::printf("error: %s: %s\n", rel.c_str(), text.error().message.c_str());
            ++problems;
        } else if (auto st = script::check_syntax(*text, rel); !st) {
            std::printf("error: %s\n", st.error().message.c_str());
            ++problems;
        } else {
            for (const std::string& target : script::scene_references(*text))
                if (std::find(project_scenes.begin(), project_scenes.end(), target) == project_scenes.end()) {
                    std::printf("error: %s: scene.load(\"%s\") names a scene that does not exist\n", rel.c_str(), target.c_str());
                    ++problems;
                }
        }
    }
    // Materials: each must parse and reference existing textures (resolved through the database).
    std::size_t materials = 0;
    auto db = assets::AssetDatabase::open(proj->root());
    if (!db) return error(db.error().message);
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Material) continue;
        ++materials;
        auto m = (*db)->resolve_material(r.uuid); // also checks the parent chain
        if (!m) {
            std::printf("error: %s\n", m.error().message.c_str());
            ++problems;
            continue;
        }
        const auto refs = m->textures();
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (refs[i]->is_nil()) continue;
            const assets::AssetRecord* t = (*db)->find(*refs[i]);
            if (!t || t->kind != assets::AssetKind::Texture) {
                std::printf("error: %s: %s texture %s does not exist\n", r.path.c_str(), assets::kMaterialTextureSlotNames[i],
                            refs[i]->to_string().c_str());
                ++problems;
            }
        }
        if (!m->shader.is_nil()) {
            const assets::AssetRecord* sh = (*db)->find(m->shader);
            if (!sh || sh->kind != assets::AssetKind::Shader) {
                std::printf("error: %s: surface shader %s does not exist\n", r.path.c_str(), m->shader.to_string().c_str());
                ++problems;
            }
        }
    }
    // Surface shaders: each must compile; errors name the file and line.
    std::size_t shaders = 0;
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Shader) continue;
        ++shaders;
        const std::string path = r.path;
        auto code = (*db)->load_shader_source(r.uuid);
        if (!code) {
            std::printf("error: %s: %s\n", path.c_str(), code.error().message.c_str());
            ++problems;
        } else if (auto c = runtime::compile_surface_shader(*code, path, false); !c) {
            std::printf("error: %s\n", c.error().message.c_str());
            ++problems;
        }
    }
    // Sprite sheets: each must parse and point at an existing texture.
    std::size_t sheets = 0;
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::SpriteSheet) continue;
        ++sheets;
        if (auto sheet = (*db)->load_sprite_sheet(r.uuid); !sheet) {
            std::printf("error: %s\n", sheet.error().message.c_str());
            ++problems;
        }
    }
    // Tile sets: each must parse and point at an existing texture.
    std::size_t tilesets = 0;
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Tileset) continue;
        ++tilesets;
        if (auto ts = (*db)->load_tileset(r.uuid); !ts) {
            std::printf("error: %s\n", ts.error().message.c_str());
            ++problems;
        }
    }
    script::HostOptions so;
    so.assets_root = assets;
    so.input_map = proj->settings().input;
    auto host = script::ScriptHost::create(so);
    if (!host) return error(host.error().message);
    // Unknown components (warning), scripts that do not load and links to missing prefabs.
    const auto check_entities = [&](const std::string& rel, scene::Scene& scene) {
        scene.world().each<scene::UnknownComponents>([&](scene::Entity e, scene::UnknownComponents& u) {
            for (const auto& [name, value] : u.components) {
                std::printf("warning: %s: entity '%s' has unknown component '%s'\n", rel.c_str(),
                            entity_name(scene, e).c_str(), name.c_str());
                (void)value;
            }
        });
        scene.world().each<script::ScriptComponent>([&](scene::Entity e, script::ScriptComponent& c) {
            if (c.script.empty()) return;
            if (auto d = (*host)->describe(c.script); !d) {
                std::printf("error: %s: entity '%s': %s\n", rel.c_str(), entity_name(scene, e).c_str(),
                            d.error().message.c_str());
                ++problems;
            }
        });
        scene.world().each<scene::MeshRendererComponent>([&](scene::Entity e, scene::MeshRendererComponent& c) {
            if (c.material_asset.is_nil()) return;
            const assets::AssetRecord* m = (*db)->find(c.material_asset);
            if (!m || m->kind != assets::AssetKind::Material) {
                std::printf("error: %s: entity '%s' uses missing material %s\n", rel.c_str(), entity_name(scene, e).c_str(),
                            c.material_asset.to_string().c_str());
                ++problems;
            }
        });
        scene.world().each<animation::SpriteAnimatorComponent>([&](scene::Entity e, animation::SpriteAnimatorComponent& c) {
            if (c.sheet.is_nil()) return;
            const assets::AssetRecord* s = (*db)->find(c.sheet);
            if (!s || s->kind != assets::AssetKind::SpriteSheet) {
                std::printf("error: %s: entity '%s' uses missing sprite sheet %s\n", rel.c_str(), entity_name(scene, e).c_str(),
                            c.sheet.to_string().c_str());
                ++problems;
            }
        });
        scene.world().each<tilemap::TilemapComponent>([&](scene::Entity e, tilemap::TilemapComponent& c) {
            if (c.tileset.is_nil()) return;
            const assets::AssetRecord* t = (*db)->find(c.tileset);
            if (!t || t->kind != assets::AssetKind::Tileset) {
                std::printf("error: %s: entity '%s' uses missing tile set %s\n", rel.c_str(), entity_name(scene, e).c_str(),
                            c.tileset.to_string().c_str());
                ++problems;
            }
        });
        scene.world().each<scene::PrefabInstance>([&](scene::Entity e, scene::PrefabInstance& pi) {
            if (!prefab_uuids.contains(pi.prefab))
                std::printf("warning: %s: entity '%s' is an instance of a prefab that no longer exists (%s); it keeps "
                            "its saved content\n",
                            rel.c_str(), entity_name(scene, e).c_str(), pi.prefab.to_string().c_str());
        });
    };
    for (const auto& rel : prefab_files) {
        ++prefabs;
        auto src = script::read_prefab(assets, rel);
        if (!src) {
            std::printf("error: %s\n", src.error().message.c_str());
            ++problems;
            continue;
        }
        scene::Scene scene;
        if (auto e = scene::instantiate(scene, src->prefab, src->asset); !e) {
            std::printf("error: %s: %s\n", rel.c_str(), e.error().message.c_str());
            ++problems;
            continue;
        }
        check_entities(rel, scene);
    }
    for (const auto& rel : proj->scenes()) {
        ++scenes;
        auto path = proj->resolve(rel);
        scene::Scene scene;
        if (!path) {
            std::printf("error: %s: %s\n", rel.c_str(), path.error().message.c_str());
            ++problems;
            continue;
        }
        if (auto st = scene.load_file(*path); !st) {
            std::printf("error: %s: %s\n", rel.c_str(), st.error().message.c_str());
            ++problems;
            continue;
        }
        check_entities(rel, scene);
    }
    std::printf("checked %zu scenes, %zu prefabs, %zu materials, %zu shaders, %zu sprite sheets, %zu tile sets and %zu scripts: "
                "%zu problems\n",
                scenes, prefabs, materials, shaders, sheets, tilesets, scripts, problems);
    return problems ? 1 : 0;
}

int cmd_export(const std::vector<std::string>& a) {
    cli::Parser p("victus export", "Package a project into a runnable game folder (player + game/game.vpak)");
    p.positional("project", "project folder", true)
        .option("out", "output folder (created; must be empty or a previous export)", "")
        .option("name", "executable name (default: project name)", "")
        .option("player", "player executable to ship (default: victus_player next to victus)", "")
        .flag("pack-only", "write game/game.vpak and the manifest without copying the player");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    if (p.get("out").empty()) return error("export needs --out DIR");
    player::ExportOptions o;
    o.project = p.get("project");
    o.output = p.get("out");
    o.executable_name = p.get("name");
    o.player_executable = p.get("player");
    o.include_player = !p.has("pack-only");
    auto r = player::export_game(o);
    if (!r) return error(r.error().message);
    for (const auto& w : r->warnings) std::printf("warning: %s\n", w.c_str());
    std::printf("exported %zu files (%zu cooked assets), pack %llu bytes: %s\n", r->files, r->cooked_assets,
                static_cast<unsigned long long>(r->pack_bytes), r->pack.string().c_str());
    if (!r->executable.empty()) std::printf("run: %s\n", r->executable.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    // A closed stdout (a pipeline reader that already exited) must not kill a running server
    // or client; writes then fail quietly instead.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (argc < 2) return std::fprintf(stderr, "%s", kUsage), 2;
    const std::string cmd = argv[1];
    const auto args = rest(argc, argv);
    if (cmd == "new") return cmd_new(args);
    if (cmd == "templates") return cmd_templates();
    if (cmd == "info") return cmd_info(args);
    if (cmd == "import") return cmd_import(args);
    if (cmd == "run") return cmd_run(args);
    if (cmd == "simulate") return cmd_simulate(args);
    if (cmd == "serve") return cmd_serve(args);
    if (cmd == "check") return cmd_check(args);
    if (cmd == "export") return cmd_export(args);
    if (cmd == "version" || cmd == "--version") return std::printf("VICTUS %s\n", build::version()), 0;
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return std::printf("%s", kUsage), 0;
    std::fprintf(stderr, "victus: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
