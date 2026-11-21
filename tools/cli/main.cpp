// SPDX-License-Identifier: Apache-2.0
// `victus` command-line tool: project creation, asset import, running games and inspection
// from a terminal or CI. Shares the project, asset and player modules with the editor.
#include "victus/gameui/gameui.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/crash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/json.hpp"
#include "victus/core/pack.hpp"
#include "victus/core/fs.hpp"
#include "victus/render/image.hpp"
#include "victus/render/image_compare.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/player/bake_lighting.hpp"
#include "victus/player/diagnostics.hpp"
#include "victus/player/export.hpp"
#include "victus/player/migrate.hpp"
#include "victus/player/player.hpp"
#include "victus/player/validate.hpp"
#include "victus/plugin/plugin.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/scene_lightmap.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/platform/platform.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/dap.hpp"
#include "victus/script/script.hpp"
#include "victus/world/world.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
#include <cmath>
#include <format>
#include <csignal>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace victus;

namespace victus_cli {
extern const std::string_view kPluginSdkHeader; // generated from victus_plugin.h (CMakeLists.txt)
}

namespace {

constexpr const char* kUsage =
    "usage: victus <command> [options]\n"
    "\n"
    "commands:\n"
    "  new <folder> [--template 3d|2d|simulation|multiplayer] [--name NAME] [--character orion|nova|none]\n"
    "  templates                                                  list project templates\n"
    "  info <project>                                             show project settings, scenes, assets\n"
    "  import <project>                                           import changed assets (exit 1 on failure)\n"
    "  run <project> [--scene S] [--frames N] [--screenshot PNG] [--quality TIER] [--profile F] [--trace F]  play a scene\n"
    "      [--capture-dir DIR] [--capture-every N] [--render-report F.json]  ...capture frames, report the rendering path\n"
    "      [--connect HOST[:PORT]] [--name NAME]                  ...as a client of a `victus serve` server\n"
    "      [--host] [--port P] [--max-clients N]                  ...host a session and play in it (listen server)\n"
    "      [--server-key HEX] [--password P] [--insecure]         ...pin the server key / join a protected server\n"
    "      [--record F.vcapture] [--replay F.vcapture]            ...record a play session, or watch one replay\n"
    "  simulate <project> [--frames N] [--hold KEYS] [--save F]   run gameplay and scripts headless (no GPU)\n"
    "           [--record F.vcapture] [--replay F.vcapture]         ...record a session, or replay one and verify it\n"
    "           [--connect HOST[:PORT]]                           ...as a headless multiplayer client\n"
    "  serve <project> [--port P] [--max-clients N] [--frames N]  host a multiplayer session (dedicated server)\n"
    "        [--password P] [--key FILE] [--insecure]              ...encrypted by default; prints its public key\n"
    "  debug-adapter                                              Debug Adapter Protocol server on stdio (script debugging)\n"
    "  migrate <project> [--write]                                upgrade files written by an older VICTUS (keeps originals)\n"
    "  compare <reference> <test> [--heatmap PNG] [--json F] [--max-flip X] ...  perceptual image/sequence diff (FLIP, SSIM)\n"
    "  check <project>                                            validate scenes and script syntax\n"
    "  plugins <project> [--load] [--new NAME]                    list, load or create native plugins\n"
    "  export <project> --out DIR [--name EXE] [--player PATH]    package a runnable game folder\n"
    "         [--sign-key FILE] [--no-compress] [--compression-level N]  ...sign / compress game.vpak\n"
    "  pack keygen --out FILE | verify PAK [--key HEX] | list PAK  pack signing keys, verification, contents\n"
    "  world <project> --scene S --out DIR [--cell-size N]       split a scene into streamed world cells (.vworld)\n"
    "  bake-lighting <project> [--scene S] [--samples N] [--density T] [--mark-static]  bake lightmaps for Static Lighting meshes\n"
    "  diagnose [project] --out FILE.zip [--no-logs] [--keep-paths] ...  bundle crash reports, logs and checks\n"
    "  script-api [--out FILE] [--write DIR] [--check DIR]        print, freeze or check the versioned Luau API\n"
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
        .option("template", "3d, 2d, nocode, nocode3d, simulation or multiplayer", "3d")
        .option("name", "project name (default: folder name)")
        .option("character", "starter character for 3d/multiplayer: orion, nova or none", "orion")
        .flag("no-bake", "do not bake the template's static lighting (victus bake-lighting can do it later)");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message + "\n" + p.help());
    const fs::Path dir = p.get("folder");
    const std::string name = p.has("name") ? p.get("name") : dir.filename().string();
    project::TemplateOptions options;
    options.character = p.get("character");
    auto proj = project::Project::create(dir, name, p.get("template"), options);
    if (!proj) return error(proj.error().message);
    std::printf("Created %s project '%s' in %s\n", p.get("template").c_str(), name.c_str(), proj->root().string().c_str());
    if (!p.has("no-bake")) {
        // 3D templates ship with baked static lighting and light probes (ADR-0134).
        jobs::JobSystem workers(0);
        auto baked = player::bake_template_lighting(*proj, &workers);
        if (!baked) std::fprintf(stderr, "victus: warning: baking the template's lighting failed: %s\n", baked.error().message.c_str());
        else if (*baked) std::printf("Baked static lighting (victus bake-lighting re-bakes it after you change the scene)\n");
    }
    std::printf("Open it with: victus_editor \"%s\"   or run it with: victus run \"%s\"\n", proj->root().string().c_str(),
                proj->root().string().c_str());
    return 0;
}

// The versioned script API (ADR-0155): print it, write the snapshot of this version into a
// folder, or check a folder of snapshots against the live bindings and each other.
int cmd_script_api(const std::vector<std::string>& a) {
    cli::Parser p("victus script-api", "Print, freeze or check the versioned Luau scripting API");
    p.option("out", "write the API description (JSON) to FILE instead of stdout")
        .option("write", "write victus_script_api_<version>.json into DIR (freezes this version)")
        .option("check", "verify DIR's snapshots: newest equals this build, each step follows the rules");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto api = script::describe_api();
    if (!api) return error(api.error().message);
    const std::string text = json::stringify(*api, true) + "\n";
    if (const std::string dir = p.get("write"); !dir.empty()) {
        const fs::Path file = fs::Path(dir) / script::snapshot_file_name(script::kScriptApiVersion);
        if (auto st = fs::write_text_atomic(file, text); !st) return error(st.error().message);
        std::printf("wrote %s\n", file.string().c_str());
    }
    if (const std::string dir = p.get("check"); !dir.empty()) {
        auto st = script::check_snapshot_folder(dir, *api);
        if (!st) return error(st.error().message);
        std::printf("script API %s matches %s\n", script::to_string(script::kScriptApiVersion).c_str(), dir.c_str());
    }
    if (const std::string out = p.get("out"); !out.empty()) {
        if (auto st = fs::write_text_atomic(out, text); !st) return error(st.error().message);
    } else if (p.get("write").empty() && p.get("check").empty()) {
        std::printf("%s", text.c_str());
    }
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
    std::printf("script API:    %s (engine provides %s)\n", script::to_string(s.script_api).c_str(),
                script::to_string(script::kScriptApiVersion).c_str());
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
        .option("capture-dir", "write frames to DIR/frame_NNNNN.png for `victus compare` (with --frames)")
        .option("capture-every", "with --capture-dir: write every Nth frame", "1")
        .option("render-report", "write the rendering path of the last frame (features, reason codes) as JSON; prints it too")
        .option("width", "window width (default: project setting)", "0")
        .option("height", "window height (default: project setting)", "0")
        .option("quality", "rendering quality: low, medium, high, ultra or auto (from the GPU and RAM; default: project setting)")
        .option("hdr", "HDR scene colour: compact (R11G11B10F), full (RGBA16F) or auto (compact on phones and Low/Medium)", "auto")
        .option("fp16", "half-precision shading: on, off or auto (on for phones and Low/Medium where the GPU supports it)", "auto")
        .option("render-scale", "3D resolution scale 0.25..2, overriding the tier and project (e.g. 0.5 to test upscaling)")
        .option("taa", "temporal anti-aliasing and upscaling: on, off or auto (the tier's choice)", "auto")
        .option("reactive", "TAA reactive mask for particles, water and rain: on or off (ADR-0143)", "on")
        .option("msaa", "MSAA of the opaque pass without TAA: off, 2, 4, 8 or auto (the tier's choice; ADR-0152)", "auto")
        .flag("fullscreen", "run fullscreen")
        .flag("sync-assets", "load textures and models on first use instead of in the background")
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
        .option("pack-key", "exported game: required game.vpak signer (hex; default: export.json signing_public_key)")
        .flag("insecure", "no encryption: join a server started with --insecure, or host one")
        .option("conditions", "emulate a bad link, e.g. latency=80,jitter=10,loss=0.05")
        .option("saves", "save games folder (default: <project>/.victus/saves)")
        .option("record", "record input, frame times and state digests to this .vcapture file")
        .option("replay", "replay a .vcapture file (exit 5 if the game state diverges)")
        .option("profile", "write frame-time percentiles and per-scope timings (JSON) to this file at exit")
        .option("trace", "write a Chrome/Perfetto trace of the first 600 frames to this file")
        .option("seed", "script random seed (math.random); default: fixed with --frames, time-based otherwise", "0");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    player::PlayerOptions o;
    o.random_seed = static_cast<std::uint64_t>(p.get_number("seed", 0));
    o.profile = p.get("profile");
    o.trace = p.get("trace");
    o.record = p.get("record");
    o.replay = p.get("replay");
    o.connect = p.get("connect");
    o.player_name = p.get("name");
    o.net_conditions = p.get("conditions");
    o.server_key = p.get("server-key");
    o.pack_key = p.get("pack-key");
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
    o.capture_dir = p.get("capture-dir");
    o.capture_every = static_cast<std::uint32_t>(std::max(1.0, p.get_number("capture-every", 1)));
    if (!o.capture_dir.empty() && !o.max_frames) return error("--capture-dir needs --frames");
    o.width = static_cast<int>(p.get_number("width", 0));
    o.height = static_cast<int>(p.get_number("height", 0));
    o.fullscreen = p.has("fullscreen");
    o.hot_reload = !p.has("no-hot-reload");
    o.async_assets = !p.has("sync-assets");
    o.quality = p.get("quality");
    if (const std::string hdr = p.get("hdr"); hdr == "compact") o.compact_hdr = true;
    else if (hdr == "full") o.compact_hdr = false;
    else if (hdr != "auto") return error("--hdr must be compact, full or auto");
    if (const std::string fp16 = p.get("fp16"); fp16 == "on") o.half_precision = true;
    else if (fp16 == "off") o.half_precision = false;
    else if (fp16 != "auto") return error("--fp16 must be on, off or auto");
    if (p.has("render-scale")) {
        const double scale = p.get_number("render-scale", 0);
        if (!(scale >= 0.25 && scale <= 2.0)) return error("--render-scale must be between 0.25 and 2");
        o.render_scale = static_cast<float>(scale);
    }
    if (const std::string taa = p.get("taa"); taa == "on") o.taa = true;
    else if (taa == "off") o.taa = false;
    else if (taa != "auto") return error("--taa must be on, off or auto");
    if (const std::string reactive = p.get("reactive"); reactive == "off") o.reactive_mask = false;
    else if (reactive != "on") return error("--reactive must be on or off");
    if (const std::string msaa = p.get("msaa"); msaa == "off" || msaa == "1") o.msaa_samples = 1u;
    else if (msaa == "2" || msaa == "4" || msaa == "8") o.msaa_samples = static_cast<std::uint32_t>(std::stoul(msaa));
    else if (msaa != "auto") return error("--msaa must be off, 2, 4, 8 or auto");
    o.saves = p.get("saves");
    auto r = player::run(o);
    if (!r) return error(r.error().message);
    std::printf("ran %llu frames in %.2f s on %s at %s quality (%llu script errors)\n",
                static_cast<unsigned long long>(r->frames), r->seconds, r->backend.c_str(), r->quality.c_str(),
                static_cast<unsigned long long>(r->script_errors));
    std::printf("hdr: %s\n", r->hdr_format.c_str());
    if (!r->device_class.empty()) std::printf("device class: %s\n", r->device_class.c_str());
    if (!r->net_status.empty()) std::printf("net: %s\n", r->net_status.c_str());
    if (p.has("render-report")) {
        if (r->render_path.features.empty()) {
            std::printf("render path: the last frame was not a 3D view\n");
        } else {
            std::printf("%s", r->render_path.to_text().c_str());
            if (auto st = fs::write_text_atomic(p.get("render-report"), json::stringify(r->render_path.to_json(), true)); !st)
                return error(st.error().message);
        }
    }
    if (r->frames_captured)
        std::printf("captured %llu frames to %s\n", static_cast<unsigned long long>(r->frames_captured), o.capture_dir.string().c_str());
    if (r->assets_streamed) std::printf("assets: %llu streamed in the background\n", static_cast<unsigned long long>(r->assets_streamed));
    if (r->assets_reloaded) std::printf("hot reload: %zu assets reloaded\n", r->assets_reloaded);
    if (r->world_cell_loads)
        std::printf("world: %llu cells streamed in, up to %u active\n", static_cast<unsigned long long>(r->world_cell_loads),
                    r->world_cells_peak);
    if (r->scene_loads)
        std::printf("scenes: %llu transitions, ended in %s\n", static_cast<unsigned long long>(r->scene_loads), r->final_scene.c_str());
    if (!o.record.empty()) std::printf("recorded capture: %s\n", o.record.string().c_str());
    if (!o.profile.empty() || !o.trace.empty()) {
        const auto& pr = r->profile;
        std::printf("frame time: mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f over %zu frames\n", pr.mean_ms, pr.p50_ms,
                    pr.p95_ms, pr.p99_ms, pr.max_ms, pr.frames);
        for (std::size_t i = 0; i < pr.scopes.size() && i < 8; ++i)
            std::printf("  %-32s %7.3f ms (p95 %.3f)\n", pr.scopes[i].name.c_str(), pr.scopes[i].mean_ms, pr.scopes[i].p95_ms);
        if (!o.profile.empty()) std::printf("profile: %s\n", o.profile.string().c_str());
        if (!o.trace.empty()) std::printf("trace: %s\n", o.trace.string().c_str());
    }
    if (!o.replay.empty()) {
        if (!r->replay_mismatch.empty()) return (void)error("replay diverged at " + r->replay_mismatch), 5;
        std::printf("replay: %llu state digests match\n", static_cast<unsigned long long>(r->digests_checked));
    }
    return p.has("strict") && r->script_errors ? 3 : 0;
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
    // Declared before the script host so plugins outlive the scripts that call them.
    std::unique_ptr<plugin::PluginManager> plugins;
    std::unique_ptr<script::ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> physics_world;
    std::unique_ptr<assets::AssetDatabase> db;
    std::unique_ptr<animation::AnimationSystem> animations;
    std::unique_ptr<tilemap::TilemapSystem> tilemaps;
    std::unique_ptr<terrain::TerrainSystem> terrains;
    particles::ParticleSystem particle_system; // single-threaded like physics; identical either way
    navigation::NavigationSystem nav;
    std::unique_ptr<ai::AiSystem> ai_system;
    std::unique_ptr<sequencer::SequencerSystem> sequencer_system;
    std::unique_ptr<audio::AudioEngine> audio_engine;
    gameplay::ManualInput input;
    gameplay::Simulation sim;
    // World streaming (ADR-0090) as in the player's fixed-frame runs: cells load synchronously
    // around the streaming sources (the camera by default), so headless checks of an open world
    // see the same trees, rocks and colliders the game does.
    std::unique_ptr<world::WorldStreamer> streamer;
    bool world_changed = false;
    std::uint32_t peak_cells = 0;

    Status start_world() {
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
        streamer = std::make_unique<world::WorldStreamer>(std::move(*desc), proj->root());
        return {};
    }
    void stream_world() {
        if (world_changed) {
            world_changed = false;
            if (auto st = start_world(); !st) std::fprintf(stderr, "world streaming: %s\n", st.error().message.c_str());
        }
        if (!streamer) return;
        scene.update_transforms();
        std::optional<math::Vec3> camera;
        scene.world().each<scene::CameraComponent, scene::WorldTransform>(
            [&](const scene::CameraComponent&, const scene::WorldTransform& t) {
                if (!camera) camera = t.matrix.transform_point({});
            });
        const auto sources = world::gather_sources(scene, camera ? &*camera : nullptr);
        streamer->update(scene, sources);
        peak_cells = std::max(peak_cells, streamer->stats().active);
    }

    Status open(const std::string& project_dir, const std::string& scene_arg, const std::string& saves = {},
                std::uint64_t random_seed = 0, bool debugger = false) {
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
        so.default_locale = proj->settings().default_locale;
        so.script_api = proj->settings().script_api;
        so.save_directory = saves.empty() ? proj->dev_save_directory() : fs::Path(saves);
        so.random_seed = random_seed;
        so.debugger = debugger;
        so.project_root = proj->root();
        so.bridge_launcher = [project = &*proj, save_dir = so.save_directory](std::string_view name) {
            return project::bridge_launch(*project, name, save_dir);
        };
        so.on_scene_loaded = [this](scene::Scene&, std::string_view loaded) {
            scene_rel = std::string(loaded);
            world_changed = true;
            if (physics_world) physics_world->clear();
            if (audio_engine) audio_engine->stop_all();
        };
        plugins = std::make_unique<plugin::PluginManager>(plugin::HostOptions{.target = "player"});
        plugins->load_directory(proj->root() / "plugins");
        if (!plugins->function_names().empty()) {
            so.plugin_call = [pm = plugins.get()](std::string_view name, std::string_view args) { return pm->call(name, args); };
            so.plugin_has = [pm = plugins.get()](std::string_view name) { return pm->has_function(name); };
        }
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
        if (db) physics_world->set_mesh_provider(assets::collision_mesh_provider(*db));
        animations = std::make_unique<animation::AnimationSystem>(
            animation::AnimationSettings{.provider = db ? assets::animation_provider(*db) : animation::SetProvider{},
                                         .controllers = db ? animation::directory_loader(db->assets_dir()) : animation::ControllerLoader{},
                                         .sheets = db ? assets::sprite_sheet_provider(*db) : animation::SheetProvider{}});
        animation::add_animation_systems(sim, *animations);
        tilemaps = std::make_unique<tilemap::TilemapSystem>(db ? assets::tileset_provider(*db) : tilemap::TilesetProvider{});
        tilemap::add_tilemap_systems(sim, *tilemaps);
        terrains = std::make_unique<terrain::TerrainSystem>(db ? assets::terrain_provider(*db) : terrain::TerrainProvider{});
        terrain::add_terrain_systems(sim, *terrains);
        particles::add_particle_systems(sim, particle_system);
        navigation::add_navigation_systems(sim, nav);
        ai_system = std::make_unique<ai::AiSystem>(db ? ai::directory_loader(db->assets_dir()) : ai::TreeLoader{});
        ai::add_ai_systems(sim, *ai_system);
        sequencer_system = std::make_unique<sequencer::SequencerSystem>(db ? sequencer::directory_loader(db->assets_dir()) : sequencer::SequenceLoader{});
        sequencer::add_sequencer_systems(sim, *sequencer_system);
        plugins->attach(sim);
        audio::EngineOptions ao; // mixed without a device so audio calls behave as in the game
        ao.assets_root = proj->root() / "assets";
        ao.null_device = true;
        auto ae = audio::AudioEngine::create(ao);
        if (!ae) return ae.error();
        audio_engine = std::move(*ae);
        audio::add_audio_systems(sim, *audio_engine);
        sim.input = &input;
        return start_world();
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
        .option("seed", "script random seed (math.random) for this run (default: fixed, so runs repeat; time-based with --connect); recordings store it", "0");
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
    } else if (!seed && !p.has("connect")) {
        seed = player::kFixedRunSeed; // fixed-step headless runs repeat exactly
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
        hs->stream_world();
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
    if (hs->streamer)
        std::printf("world: %u cells active, peak %u of %zu\n", hs->streamer->stats().active, hs->peak_cells,
                    hs->streamer->world().cells.size());
    for (const auto& e : hs->host->recent_errors()) std::fprintf(stderr, "script error: %s\n", e.c_str());
    if (!hs->plugins->statuses().empty()) {
        std::printf("plugins: %zu loaded\n", hs->plugins->loaded_count());
        for (const auto& ps : hs->plugins->statuses())
            if (ps.state != plugin::State::Loaded)
                std::fprintf(stderr, "plugin %s %s: %s\n", ps.name.c_str(), std::string(plugin::to_string(ps.state)).c_str(),
                             ps.error.c_str());
    }
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
// referenced files exist); see victus/player/validate.hpp. Exit 1 if anything is wrong.
int cmd_check(const std::vector<std::string>& a) {
    cli::Parser p("victus check", "Validate scenes, prefabs, materials, shaders, sprite sheets, tile sets, animation controllers and scripts");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    project::register_components();
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    auto report = player::validate_project(*proj);
    if (!report) return error(report.error().message);
    for (const player::Diagnostic& d : report->diagnostics) std::printf("%s\n", d.line().c_str());
    std::printf("%s\n", report->summary().c_str());
    return report->ok() ? 0 : 1;
}

int cmd_migrate(const std::vector<std::string>& a) {
    cli::Parser p("victus migrate",
                  "Report project files written by an older VICTUS (project, scenes, prefabs, worlds) and, with --write, "
                  "save them in the current format; each original is kept once as <file>.premigration. Exit 3 when "
                  "files need migrating (without --write), 1 when a file cannot be read or is from a newer VICTUS");
    p.positional("project", "project folder", true).flag("write", "save upgraded files (default: report only)");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    const bool write = p.has("write");
    auto scan = player::migrate_project(p.get("project"), write);
    if (!scan) return error(scan.error().message);
    for (const auto& e : scan->entries) {
        if (!e.error.empty()) {
            std::printf("error: %s (%s): %s\n", e.file.c_str(), e.format.c_str(), e.error.c_str());
            continue;
        }
        std::printf("%s: %s %s v%u -> v%u\n", e.written ? "migrated" : "needs migration", e.file.c_str(), e.format.c_str(), e.from, e.to);
        for (const auto& n : e.notes) std::printf("    %s\n", n.c_str());
    }
    std::printf("%zu files scanned, %zu to migrate, %zu migrated, %zu errors\n", scan->scanned, scan->pending(),
                static_cast<std::size_t>(std::count_if(scan->entries.begin(), scan->entries.end(), [](const auto& e) { return e.written; })),
                scan->failures());
    if (scan->failures()) return 1;
    return scan->pending() ? 3 : 0;
}

// Loads one PNG, or every *.png of a folder in name order (a captured frame sequence).
Result<std::vector<std::pair<std::string, render::Image>>> load_frames(const fs::Path& path) {
    std::vector<fs::Path> files;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(path, ec))
            if (e.is_regular_file() && e.path().extension() == ".png") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        if (files.empty()) return make_error(ErrorCode::NotFound, "no .png files in " + path.string());
    } else {
        files.push_back(path);
    }
    std::vector<std::pair<std::string, render::Image>> out;
    for (const auto& f : files) {
        auto img = render::load_image(f);
        if (!img) return make_error(img.error().code, f.string() + ": " + img.error().message);
        out.emplace_back(f.filename().string(), std::move(img).value());
    }
    return out;
}

int cmd_compare(const std::vector<std::string>& a) {
    cli::Parser p("victus compare",
                  "Compare a rendered image or frame sequence with a reference (FLIP, SSIM, worst tile, temporal)");
    p.positional("reference", "reference PNG, or folder of frame PNGs", true)
        .positional("test", "test PNG, or folder with the same frame names", true)
        .option("heatmap", "write the FLIP error map (magma) of the worst frame to this PNG")
        .option("json", "write all metrics as JSON to this file")
        .option("ppd", "viewing condition in pixels per degree (default: FLIP's 67, 0.7 m from a 4K 0.7 m monitor)", "0")
        .option("tile", "pooling tile size in pixels", "64")
        .option("max-flip", "fail (exit 4) if a frame's mean FLIP is above this")
        .option("max-tile-flip", "fail if any tile's mean FLIP is above this")
        .option("min-ssim", "fail if a frame's SSIM is below this")
        .option("min-tile-ssim", "fail if any tile's SSIM is below this")
        .option("max-temporal", "fail if the frame-to-frame change differs from the reference's by more than this");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message + "\n" + p.help());
    auto ref = load_frames(p.get("reference"));
    if (!ref) return error(ref.error().message);
    auto test = load_frames(p.get("test"));
    if (!test) return error(test.error().message);
    if (ref->size() != test->size()) return error(std::format("reference has {} frames, test has {}", ref->size(), test->size()));
    std::vector<render::Image> ri, ti;
    for (std::size_t i = 0; i < ref->size(); ++i) {
        if (ref->size() > 1 && (*ref)[i].first != (*test)[i].first)
            return error("frame names differ: " + (*ref)[i].first + " vs " + (*test)[i].first);
        ri.push_back(std::move((*ref)[i].second));
        ti.push_back(std::move((*test)[i].second));
    }
    render::CompareOptions co;
    co.pixels_per_degree = static_cast<float>(p.get_number("ppd", 0));
    co.tile = static_cast<std::uint32_t>(std::max(8.0, p.get_number("tile", 64)));
    co.keep_maps = p.has("heatmap");
    auto r = render::compare_sequences(ri, ti, co);
    if (!r) return error(r.error().message);
    for (std::size_t i = 0; i < r->frames.size(); ++i) {
        const auto& f = r->frames[i];
        std::printf("%s: FLIP mean %.4f p95 %.4f max %.4f worst tile %.4f at %u,%u %ux%u | SSIM %.4f worst tile %.4f | PSNR %.2f dB | visible %.2f%%\n",
                    (*ref)[i].first.c_str(), f.flip_mean, f.flip_p95, f.flip_max, f.flip_worst_tile, f.worst_tile_x, f.worst_tile_y,
                    f.worst_tile_w, f.worst_tile_h, f.ssim, f.ssim_worst_tile, f.psnr, 100.0 * f.visible_fraction);
    }
    if (r->frames.size() > 1)
        std::printf("sequence: %zu frames, FLIP mean %.4f, worst frame %s (%.4f), worst tile %.4f, temporal error %.5f (at %s)\n",
                    r->frames.size(), r->flip_mean, (*ref)[r->worst_frame].first.c_str(), r->flip_worst_frame, r->flip_worst_tile,
                    r->temporal_error, (*ref)[r->temporal_worst_frame].first.c_str());
    if (p.has("heatmap")) {
        if (auto st = render::save_png(p.get("heatmap"), render::flip_heatmap(r->frames[r->worst_frame])); !st)
            return error(st.error().message);
    }
    render::CompareLimits lim;
    const auto limit = [&](const char* name, double& out) {
        if (p.has(name)) out = p.get_number(name, -1);
    };
    limit("max-flip", lim.max_flip_mean);
    limit("max-tile-flip", lim.max_flip_tile);
    limit("min-ssim", lim.min_ssim);
    limit("min-tile-ssim", lim.min_ssim_tile);
    limit("max-temporal", lim.max_temporal);
    const auto failures = render::check_limits(*r, lim);
    if (p.has("json")) {
        auto root = json::Value::object();
        auto frames = json::Value::array();
        for (std::size_t i = 0; i < r->frames.size(); ++i) {
            const auto& f = r->frames[i];
            auto o = json::Value::object();
            o.set("name", (*ref)[i].first);
            o.set("flip_mean", f.flip_mean);
            o.set("flip_p95", f.flip_p95);
            o.set("flip_max", f.flip_max);
            o.set("flip_worst_tile", f.flip_worst_tile);
            auto rect = json::Value::array();
            for (const auto v : {f.worst_tile_x, f.worst_tile_y, f.worst_tile_w, f.worst_tile_h}) rect.push(static_cast<double>(v));
            o.set("worst_tile_rect", std::move(rect));
            o.set("ssim", f.ssim);
            o.set("ssim_worst_tile", f.ssim_worst_tile);
            o.set("psnr", std::isinf(f.psnr) ? 999.0 : f.psnr);
            o.set("visible_fraction", f.visible_fraction);
            frames.push(std::move(o));
        }
        root.set("frames", std::move(frames));
        root.set("flip_mean", r->flip_mean);
        root.set("flip_worst_frame", r->flip_worst_frame);
        root.set("flip_worst_tile", r->flip_worst_tile);
        root.set("ssim_worst_tile", r->ssim_worst_tile);
        root.set("temporal_error", r->temporal_error);
        root.set("pixels_per_degree", static_cast<double>(co.pixels_per_degree > 0 ? co.pixels_per_degree
                                                                                       : render::pixels_per_degree(0.7f, 3840.0f, 0.7f)));
        auto fails = json::Value::array();
        for (const auto& f : failures) fails.push(f);
        root.set("failures", std::move(fails));
        root.set("pass", failures.empty());
        if (auto st = fs::write_text_atomic(p.get("json"), json::stringify(root, true)); !st) return error(st.error().message);
    }
    for (const auto& f : failures) std::fprintf(stderr, "victus compare: outside tolerance: %s\n", f.c_str());
    return failures.empty() ? 0 : 4;
}

int cmd_export(const std::vector<std::string>& a) {
    cli::Parser p("victus export", "Package a project into a runnable game folder (player + game/game.vpak)");
    p.positional("project", "project folder", true)
        .option("out", "output folder (created; must be empty or a previous export)", "")
        .option("name", "executable name (default: project name)", "")
        .option("player", "player executable to ship (default: victus_player next to victus)", "")
        .option("target", "texture profile: all (cooked cache as is), desktop (no ASTC), android (ASTC, 1024 px cap)", "all")
        .flag("pack-only", "write game/game.vpak and the manifest without copying the player")
        .option("sign-key", "Ed25519 key file (`victus pack keygen`) to sign game.vpak with; the player then requires it", "")
        .flag("no-compress", "store every entry raw (faster export, larger pack)")
        .option("compression-level", "zstd level 1..19 (decoding speed does not depend on it)", "12");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    if (p.get("out").empty()) return error("export needs --out DIR");
    player::ExportOptions o;
    o.project = p.get("project");
    o.output = p.get("out");
    o.executable_name = p.get("name");
    o.player_executable = p.get("player");
    o.include_player = !p.has("pack-only");
    o.target = p.get("target");
    o.compress = !p.has("no-compress");
    o.compression_level = static_cast<int>(p.get_number("compression-level", 12));
    if (o.compression_level < 1 || o.compression_level > 19) return error("--compression-level must be 1..19");
    o.signing_key = p.get("sign-key");
    auto r = player::export_game(o);
    if (!r) return error(r.error().message);
    for (const auto& w : r->warnings) std::printf("warning: %s\n", w.c_str());
    std::printf("exported %zu files (%zu cooked assets), pack %llu bytes: %s\n", r->files, r->cooked_assets,
                static_cast<unsigned long long>(r->pack_bytes), r->pack.string().c_str());
    if (r->target != "all")
        std::printf("target %s: cooked assets %llu -> %llu bytes\n", r->target.c_str(), static_cast<unsigned long long>(r->cooked_bytes_in),
                    static_cast<unsigned long long>(r->cooked_bytes_out));
    std::printf("pack payload %llu bytes, %zu entries zstd-compressed\n", static_cast<unsigned long long>(r->payload_bytes),
                r->compressed_entries);
    if (!r->signer.empty()) std::printf("signed by %s\n", r->signer.c_str());
    if (r->plugins) std::printf("exported %zu native plugin(s) to game/plugins\n", r->plugins);
    if (!r->executable.empty()) std::printf("run: %s\n", r->executable.string().c_str());
    return 0;
}

// Pack tools (ADR-0154): signing keys, full verification and listing of .vpak archives.
int cmd_pack(const std::vector<std::string>& a) {
    cli::Parser p("victus pack", "keygen --out FILE: new Ed25519 signing key; verify PAK: check structure, signature and every "
                                 "entry digest; list PAK: entries with sizes and codecs");
    p.positional("action", "keygen, verify or list", true)
        .positional("pack", "pack file (verify, list)")
        .option("out", "keygen: key file to create (must not exist)", "")
        .option("key", "verify: required signer public key (hex)", "")
        .flag("require-signature", "verify: fail on unsigned packs");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    const std::string action = p.get("action");
    if (action == "keygen") {
        const fs::Path out = p.get("out");
        if (out.empty()) return error("pack keygen needs --out FILE");
        std::error_code ec;
        if (std::filesystem::exists(out, ec)) return error("refusing to overwrite " + out.string());
        auto key = pack::generate_signing_key();
        if (!key) return error(key.error().message);
        if (auto st = pack::save_signing_key(*key, out); !st) return error(st.error().message);
        std::printf("wrote %s (keep it secret)\npublic key: %s\n", out.string().c_str(), pack::to_hex(key->public_key()).c_str());
        return 0;
    }
    if (action != "verify" && action != "list") return error("unknown pack action '" + action + "' (keygen, verify, list)");
    if (p.get("pack").empty()) return error("pack " + action + " needs a pack file");
    pack::OpenOptions po;
    po.require_signature = p.has("require-signature");
    if (!p.get("key").empty()) {
        auto k = pack::public_key_from_hex(p.get("key"));
        if (!k) return error("--key: " + k.error().message);
        po.trusted_keys.push_back(*k);
    }
    auto pk = pack::Pack::open(p.get("pack"), po);
    if (!pk) return error(pk.error().message);
    const auto& pack = **pk;
    std::uint64_t raw = 0, stored = 0;
    std::size_t compressed = 0;
    for (const auto& e : pack.entries()) {
        raw += e.size;
        stored += e.stored_size;
        if (e.codec == pack::Codec::Zstd) ++compressed;
        if (action == "list")
            std::printf("%12llu %12llu %-6s %s\n", static_cast<unsigned long long>(e.size), static_cast<unsigned long long>(e.stored_size),
                        e.codec == pack::Codec::Zstd ? "zstd" : "stored", e.path.c_str());
    }
    if (action == "verify") {
        for (const auto& e : pack.entries())
            if (auto bytes = pack.read(e.path); !bytes) return error(e.path + ": " + bytes.error().message);
    }
    std::printf("%s: version %u, %zu entries (%zu zstd), %llu bytes -> %llu stored, %s\n", p.get("pack").c_str(), pack.version(),
                pack.entries().size(), compressed, static_cast<unsigned long long>(raw), static_cast<unsigned long long>(stored),
                pack.signer() ? ("signed by " + pack::to_hex(*pack.signer())).c_str() : "unsigned");
    if (action == "verify") std::printf("ok: every entry digest matches\n");
    return 0;
}

// Collects crash reports, recent session logs, system facts and (with a project) its settings
// and `victus check` results into one zip for a bug report. Nothing is uploaded; see
// docs/DIAGNOSTICS.md and victus/player/diagnostics.hpp.
int cmd_diagnose(const std::vector<std::string>& a) {
    cli::Parser p("victus diagnose", "Bundle crash reports, logs and project checks into a zip for a bug report (nothing is uploaded)");
    p.positional("project", "project folder (optional; adds victus.project and validation results)")
        .option("out", "zip file to write", "")
        .option("diagnostics", "diagnostics folder to read (default: VICTUS_DIAGNOSTICS_DIR or the user data folder)", "")
        .flag("list", "only list crash reports and logs that would be bundled")
        .flag("no-system", "leave out OS, CPU and memory facts")
        .flag("no-crashes", "leave out crash reports")
        .flag("no-logs", "leave out session logs")
        .flag("no-project", "leave out victus.project")
        .flag("no-validation", "leave out `victus check` results")
        .flag("keep-paths", "do not replace home folder, user name and project path");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    const fs::Path dir = p.get("diagnostics").empty() ? crash::default_directory() : fs::Path(p.get("diagnostics"));
    if (p.has("list")) {
        std::printf("diagnostics folder: %s\n", dir.string().c_str());
        const auto reports = crash::list_reports(dir);
        std::printf("crash reports: %zu\n", reports.size());
        for (const auto& r : reports)
            std::printf("  %s  %s  %s\n", r.path.filename().string().c_str(), r.app.c_str(), r.reason.c_str());
        return 0;
    }
    if (p.get("out").empty()) return error("diagnose needs --out FILE.zip (or --list)");
    player::BundleOptions o;
    o.output = p.get("out");
    o.diagnostics_dir = dir;
    o.project = p.get("project");
    o.requested_by = "victus diagnose";
    o.include_system = !p.has("no-system");
    o.include_crash_reports = !p.has("no-crashes");
    o.include_logs = !p.has("no-logs");
    o.include_project_settings = !p.has("no-project");
    o.include_validation = !p.has("no-validation");
    o.redact_paths = !p.has("keep-paths");
    auto r = player::create_diagnostics_bundle(o);
    if (!r) return error(r.error().message);
    std::printf("wrote %s: %zu files, %zu bytes (%zu crash reports, %zu logs, %zu redactions)\n", r->path.string().c_str(),
                r->files.size(), r->bytes, r->crash_reports, r->logs, r->redactions);
    std::printf("nothing was uploaded; review the bundle before attaching it to a bug report\n");
    return 0;
}

// Native plugins (ADR-0078, docs/PLUGINS.md): list and check `<project>/plugins`, load them for
// real with --load, or scaffold a new C plugin with --new NAME.
constexpr std::string_view kPluginTemplateC = R"C(/* NAME: a VICTUS native plugin (docs/PLUGINS.md). */
#include "victus/plugin/victus_plugin.h"

static const VictusHostApi* g_host;
static const VictusLogV1* g_log;
static const VictusSystemsV1* g_systems;
static const VictusSceneV1* g_scene;
static const VictusFunctionsV1* g_functions;

/* Runs every frame: spins the entity named "Spinner" around Y, if the scene has one. */
static void spin(void* user, VictusScene* scene, float dt) {
    static float angle;
    float q[4];
    VictusEntity e = g_scene->find_by_name(scene, "Spinner");
    (void)user;
    if (e == VICTUS_NULL_ENTITY) return;
    angle += dt;
    /* Quaternion for `angle` radians about +Y (small-angle-safe without libm). */
    q[0] = 0.0f;
    q[1] = angle / 2.0f - (angle * angle * angle) / 48.0f;
    q[2] = 0.0f;
    q[3] = 1.0f - (angle * angle) / 8.0f;
    if (angle > 1.0f) angle = 0.0f;
    g_scene->set_rotation(scene, e, q);
}

/* Luau: plugins.call("NAME.greet", "world") --> "hello from NAME" */
static int greet(void* user, const char* args_json, VictusCallResult* result) {
    (void)user;
    (void)args_json;
    g_functions->set_result(result, "\"hello from NAME\"");
    return VICTUS_PLUGIN_OK;
}

static int init(void* user) {
    (void)user;
    if (g_systems->add_system(g_host->host, "spin", VICTUS_PHASE_UPDATE, spin, 0) != VICTUS_PLUGIN_OK) return VICTUS_PLUGIN_ERROR;
    if (g_functions->register_function(g_host->host, "greet", greet, 0) != VICTUS_PLUGIN_OK) return VICTUS_PLUGIN_ERROR;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "NAME plugin initialized");
    return VICTUS_PLUGIN_OK;
}

static void shutdown(void* user) {
    (void)user;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "NAME plugin shut down");
}

VICTUS_PLUGIN_EXPORT int victus_plugin_entry(const VictusHostApi* host, VictusPluginDesc* out) {
    if (host->api_major != VICTUS_PLUGIN_API_MAJOR) return VICTUS_PLUGIN_ERROR;
    g_host = host;
    g_log = (const VictusLogV1*)host->get_interface(host->host, VICTUS_LOG_INTERFACE_1);
    g_systems = (const VictusSystemsV1*)host->get_interface(host->host, VICTUS_SYSTEMS_INTERFACE_1);
    g_scene = (const VictusSceneV1*)host->get_interface(host->host, VICTUS_SCENE_INTERFACE_1);
    g_functions = (const VictusFunctionsV1*)host->get_interface(host->host, VICTUS_FUNCTIONS_INTERFACE_1);
    if (!g_log || !g_systems || !g_scene || !g_functions) return VICTUS_PLUGIN_ERROR;
    out->struct_size = sizeof *out;
    out->api_major = VICTUS_PLUGIN_API_MAJOR;
    out->api_minor = VICTUS_PLUGIN_API_MINOR;
    out->user = 0;
    out->init = init;
    out->shutdown = shutdown;
    return VICTUS_PLUGIN_OK;
}
)C";

constexpr std::string_view kPluginTemplateCMake = R"CM(# Builds the NAME plugin library into bin/ next to NAME.vplugin:
#   cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(NAME C)
add_library(NAME MODULE src/NAME.c)
target_include_directories(NAME PRIVATE include)
# "$<1:...>" keeps multi-config generators from adding a Debug/Release subfolder.
set_target_properties(NAME PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>")
if(APPLE)
    set_target_properties(NAME PROPERTIES SUFFIX ".dylib")
endif()
)CM";

constexpr std::string_view kPluginTemplateManifest = R"J({
  "name": "NAME",
  "version": "0.1.0",
  "api": "API",
  "description": "A native VICTUS plugin.",
  "libraries": {
    "linux": "bin/NAME.so",
    "windows": "bin/NAME.dll",
    "macos": "bin/NAME.dylib"
  },
  "dependencies": {},
  "targets": ["player", "editor"],
  "enabled": true
}
)J";

std::string fill_template(std::string_view text, std::string_view name) {
    std::string out(text);
    auto replace_all = [&out](std::string_view key, const std::string& value) {
        for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size()))
            out.replace(at, key.size(), value);
    };
    replace_all("NAME", std::string(name));
    replace_all("\"API\"", std::format("\"{}.{}\"", VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR));
    return out;
}

int plugins_new(const fs::Path& root, const std::string& name) {
    const fs::Path dir = root / "plugins" / name;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) return error(dir.string() + " already exists");
    std::filesystem::create_directories(dir / "src", ec);
    std::filesystem::create_directories(dir / "include" / "victus" / "plugin", ec);
    if (ec) return error("cannot create " + dir.string() + ": " + ec.message());
    const std::pair<fs::Path, std::string> files[] = {
        {dir / (name + std::string(plugin::kManifestExtension)), fill_template(kPluginTemplateManifest, name)},
        {dir / "src" / (name + ".c"), fill_template(kPluginTemplateC, name)},
        {dir / "CMakeLists.txt", fill_template(kPluginTemplateCMake, name)},
        {dir / "include" / "victus" / "plugin" / "victus_plugin.h", std::string(victus_cli::kPluginSdkHeader)},
        {dir / ".gitignore", "build/\nbin/\n"},
    };
    for (const auto& [path, text] : files)
        if (auto st = fs::write_text_atomic(path, text); !st) return error(st.error().message);
    std::printf("created plugin '%s' in %s\n", name.c_str(), dir.string().c_str());
    std::printf("build it: cmake -S %s -B %s && cmake --build %s --config Release\n", dir.string().c_str(),
                (dir / "build").string().c_str(), (dir / "build").string().c_str());
    return 0;
}

int cmd_plugins(const std::vector<std::string>& a) {
    cli::Parser p("victus plugins", "List, check, load or create native plugins in <project>/plugins");
    p.positional("project", "project folder", true)
        .flag("load", "load and initialize every plugin (as the player does), then shut them down")
        .option("new", "create plugins/NAME with a C template, CMake file and the SDK header", "");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    if (!p.get("new").empty()) {
        if (auto m = plugin::parse_manifest(std::format(R"({{"name":"{}","version":"0.1.0","api":"1.0"}})", p.get("new")), {}); !m)
            return error("plugin names are 1-64 characters of [a-z0-9_]");
        return plugins_new(proj->root(), p.get("new"));
    }
    const fs::Path dir = proj->root() / "plugins";
    std::printf("plugin API %u.%u, platform %s\n", VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR,
                std::string(plugin::platform_key()).c_str());
    int problems = 0;
    if (p.has("load")) {
        plugin::PluginManager pm({.target = "player"});
        pm.load_directory(dir);
        for (const auto& st : pm.statuses()) {
            std::printf("%-24s %-10s %s%s%s\n", st.name.c_str(), plugin::to_string(st.version).c_str(),
                        std::string(plugin::to_string(st.state)).c_str(), st.error.empty() ? "" : ": ", st.error.c_str());
            for (const auto& s : st.systems) std::printf("    system   %s\n", s.c_str());
            for (const auto& f : st.functions) std::printf("    function %s\n", f.c_str());
            if (st.state == plugin::State::Failed || st.state == plugin::State::Rejected || st.state == plugin::State::Skipped)
                ++problems;
        }
        std::printf("%zu loaded, %d problems\n", pm.loaded_count(), problems);
        return problems ? 1 : 0;
    }
    std::vector<plugin::DiscoveryIssue> issues;
    const auto manifests = plugin::discover(dir, &issues);
    for (const auto& i : issues) {
        std::printf("error: %s: %s\n", i.path.string().c_str(), i.message.c_str());
        ++problems;
    }
    const auto resolved = plugin::resolve(manifests);
    for (const auto& m : manifests) {
        std::string state = m.enabled ? "ready" : "disabled";
        if (auto it = resolved.rejected.find(m.name); it != resolved.rejected.end()) {
            state = "error: " + it->second;
            ++problems;
        } else if (m.enabled) {
            const auto lib = m.libraries.find(std::string(plugin::platform_key()));
            if (lib == m.libraries.end()) state = "warning: no library for this platform";
            else if (std::error_code ec; !std::filesystem::is_regular_file(m.dir / lib->second, ec))
                state = "warning: " + lib->second + " is not built";
        }
        std::printf("%-24s %-10s %s\n", m.name.c_str(), plugin::to_string(m.version).c_str(), state.c_str());
    }
    std::printf("%zu plugins, %d problems\n", manifests.size(), problems);
    return problems ? 1 : 0;
}

// World partition (6h, ADR-0090): moves the scene's streamable root subtrees into grid cell
// scenes under DIR, writes DIR.vworld and rewrites the scene as the persistent part.
int cmd_bake_lighting(const std::vector<std::string>& a) {
    cli::Parser p("victus bake-lighting", "Bake static lighting (lightmaps, ADR-0125) for a scene's Lightmapped mesh renderers");
    p.positional("project", "project folder", true)
        .option("scene", "scene to bake (project-relative; default: the startup scene)", "")
        .option("out", "project-relative .vlightmap to write (default: the Environment's lightmap, else next to the scene)", "")
        .option("samples", "rays per texel (more = less noise, slower)", "128")
        .option("bounces", "indirect bounces", "3")
        .option("density", "lightmap texels per metre", "8")
        .option("max-size", "largest atlas side in texels (density is lowered to fit)", "2048")
        .option("threads", "worker threads (0 = all cores)", "0")
        .flag("direct", "also bake direct light from the scene's lights (otherwise only sky and bounce light)")
        .option("probe-spacing", "metres between baked light probes for dynamic objects", "2")
        .flag("no-probes", "do not bake light probes (dynamic objects keep sky ambient)")
        .flag("mark-static", "first mark every mesh that cannot move (no dynamic/kinematic body, character, animator or script) as Static Lighting")
        .flag("dry-run", "bake and report without writing files");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    project::register_components();
    player::BakeLightingOptions o;
    render::LightmapSettings& ls = o.settings;
    std::uint32_t threads = 0;
    try {
        ls.samples = static_cast<std::uint32_t>(std::clamp(std::stoi(p.get("samples")), 1, 65536));
        ls.bounces = static_cast<std::uint32_t>(std::clamp(std::stoi(p.get("bounces")), 0, 16));
        ls.texels_per_unit = std::stof(p.get("density"));
        ls.max_atlas_size = static_cast<std::uint32_t>(std::clamp(std::stoi(p.get("max-size")), 64, 8192));
        threads = static_cast<std::uint32_t>(std::clamp(std::stoi(p.get("threads")), 0, 256));
        ls.probe_spacing = std::stof(p.get("probe-spacing"));
    } catch (const std::exception&) {
        return error("--samples, --bounces, --density, --max-size, --threads and --probe-spacing must be numbers");
    }
    if (!(ls.probe_spacing >= 0.25f) || !std::isfinite(ls.probe_spacing)) return error("--probe-spacing must be at least 0.25");
    ls.probes = !p.has("no-probes");
    if (!(ls.texels_per_unit > 0.0f) || !std::isfinite(ls.texels_per_unit)) return error("--density must be positive");
    ls.include_direct = p.has("direct");
    o.scene = p.get("scene");
    o.out = p.get("out");
    o.mark_static = p.has("mark-static");
    o.dry_run = p.has("dry-run");
    jobs::JobSystem workers(threads);
    o.jobs = &workers;
    auto r = player::bake_project_lighting(*proj, o);
    if (!r) return error(r.error().message);
    if (o.mark_static) std::printf("marked %u mesh renderers as Static Lighting\n", r->marked);
    const auto& bake = r->bake;
    for (const auto& w : bake.warnings) std::fprintf(stderr, "victus: warning: %s\n", w.c_str());
    const auto& st = bake.stats;
    std::printf("baked %u lightmapped renderers (%u blockers, %u lights): %ux%u atlas at %.2f texels/m, %llu texels "
                "(%llu inside geometry), %llu rays in %.1f s\n",
                bake.receivers, bake.blockers, bake.lights, st.width, st.height, st.texels_per_unit,
                static_cast<unsigned long long>(st.texels), static_cast<unsigned long long>(st.invalid_texels),
                static_cast<unsigned long long>(st.rays), r->seconds);
    if (st.probes > 0) {
        const auto& pr = bake.file.probes;
        std::printf("baked %u light probes (%ux%ux%u, %u inside geometry, %u dilated) in %.1f s\n", st.probes, pr.counts[0],
                    pr.counts[1], pr.counts[2], st.invalid_probes, st.dilated_probes, st.probe_ms / 1000.0);
    }
    if (o.dry_run) return std::printf("dry run: would write %s\n", r->out.c_str()), 0;
    if (r->added_environment) std::printf("added an Environment entity (default sky) to reference the lightmap\n");
    std::printf("wrote %s%s\n", r->out.c_str(), r->scene_updated ? std::format(" and updated {}", r->scene).c_str() : "");
    return 0;
}

int cmd_world(const std::vector<std::string>& a) {
    cli::Parser p("victus world", "Split a scene into streamed world cells");
    p.positional("project", "project folder", true)
        .option("scene", "scene to partition (project-relative; default: the startup scene)", "")
        .option("out", "project-relative folder for the cell scenes; the descriptor is <out>.vworld", "")
        .option("cell-size", "grid cell size in world units (XZ)", "64")
        .option("load-radius", "distance at which cells load (0 = 1.5 x cell size)", "0")
        .option("name", "world name", "World")
        .flag("dry-run", "print the partition without writing files");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    if (p.get("out").empty()) return error("--out DIR is required");
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    const std::string rel = p.get("scene").empty() ? proj->settings().startup_scene : p.get("scene");
    auto path = proj->resolve(rel);
    if (!path) return error(path.error().message);
    scene::Scene s;
    if (auto st = s.load_file(*path); !st) return error(st.error().message);
    world::PartitionSettings ps;
    try {
        ps.cell_size = std::stof(p.get("cell-size"));
        ps.load_radius = std::stof(p.get("load-radius"));
    } catch (const std::exception&) {
        return error("--cell-size and --load-radius must be numbers");
    }
    ps.directory = p.get("out");
    ps.name = p.get("name");
    const std::string world_rel = ps.directory + std::string(world::kWorldExtension);
    auto part = world::partition_scene(s, ps, world_rel);
    if (!part) return error(part.error().message);
    for (std::size_t i = 0; i < part->world.cells.size(); ++i) // cells and their documents share one order
        std::printf("  %-16s %-48s %zu entities\n", part->world.cells[i].name.c_str(), part->world.cells[i].scene.c_str(),
                    part->cells[i].second["entities"].size());
    std::printf("%zu cells, %zu entities moved out of %s\n", part->world.cells.size(), part->moved_entities, rel.c_str());
    if (p.has("dry-run")) return 0;
    for (const auto& [cell_rel, doc] : part->cells) {
        const fs::Path out = proj->root() / cell_rel;
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        if (auto st = fs::write_text_atomic(out, json::stringify(doc)); !st) return error(st.error().message);
    }
    if (auto st = world::save_world_file(part->world, proj->root() / world_rel); !st) return error(st.error().message);
    if (auto st = fs::write_text_atomic(*path, json::stringify(part->persistent)); !st) return error(st.error().message);
    std::printf("wrote %s and rewrote %s as the persistent scene\n", world_rel.c_str(), rel.c_str());
    return 0;
}

// Hidden: crashes on purpose so tests can check the crash handlers end to end.
int cmd_crash_test(const std::vector<std::string>& a) {
    if (a.empty() || !crash::trigger(a[0])) return error("crash-test needs segv, abort, terminate or assert");
    return 0;
}

// ---- victus debug-adapter (ADR-0099) ------------------------------------------------------------

// Messages from the debugger client, read by a background thread from stdin.
class DapInbox {
public:
    void push(json::Value v) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(v));
        }
        cv_.notify_one();
    }
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
    }
    // Blocks until a message arrives (nullopt: stdin closed).
    std::optional<json::Value> wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        return take();
    }
    std::optional<json::Value> poll() {
        std::lock_guard lock(mutex_);
        return take();
    }
    bool closed() {
        std::lock_guard lock(mutex_);
        return closed_ && queue_.empty();
    }

private:
    std::optional<json::Value> take() {
        if (queue_.empty()) return std::nullopt;
        json::Value v = std::move(queue_.front());
        queue_.pop_front();
        return v;
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<json::Value> queue_;
    bool closed_{false};
};

// Collects script logs and warnings for the client's debug console (flushed on the main thread).
class DapLogSink final : public log::Sink {
public:
    void write(const log::Record& r) override {
        if (r.category != "script" && r.level < log::Level::Warn) return;
        std::lock_guard lock(mutex_);
        pending_.emplace_back(r.level >= log::Level::Warn ? "stderr" : "stdout", r.message + "\n");
    }
    void flush_to(script::DapSession& dap) {
        std::vector<std::pair<std::string, std::string>> out;
        {
            std::lock_guard lock(mutex_);
            out.swap(pending_);
        }
        for (const auto& [category, text] : out) dap.output(category, text);
    }

private:
    std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> pending_;
};

int cmd_debug_adapter(const std::vector<std::string>& a) {
    cli::Parser p("victus debug-adapter",
                  "Debug Adapter Protocol server on stdin/stdout: breakpoints, stepping and inspection of a "
                  "project's Luau scripts in a headless game. Started by editors such as VS Code (tools/vscode-victus); "
                  "the `launch` request takes {project, scene?, frames?, stopOnEntry?, saves?, noDebug?}.");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);

    // The protocol owns the real stdout; anything else printing to stdout goes to stderr.
    std::fflush(stdout);
#ifdef _WIN32
    const int out_fd = _dup(1);
    _dup2(2, 1);
    _setmode(out_fd, _O_BINARY);
    _setmode(0, _O_BINARY);
#else
    const int out_fd = dup(1);
    dup2(2, 1);
#endif
    if (out_fd < 0) return error("cannot duplicate stdout");
    std::mutex out_mutex;
    auto send = [&](const json::Value& m) {
        const std::string bytes = script::dap_encode(m);
        std::lock_guard lock(out_mutex);
        std::size_t done = 0;
        while (done < bytes.size()) {
#ifdef _WIN32
            const int n = _write(out_fd, bytes.data() + done, static_cast<unsigned>(bytes.size() - done));
#else
            const auto n = ::write(out_fd, bytes.data() + done, bytes.size() - done);
#endif
            if (n <= 0) return;
            done += static_cast<std::size_t>(n);
        }
    };

    DapInbox inbox;
    std::thread([&inbox] {
        script::DapDecoder decoder;
        char buf[4096];
        for (;;) {
#ifdef _WIN32
            const int n = _read(0, buf, sizeof buf);
#else
            const auto n = ::read(0, buf, sizeof buf);
#endif
            if (n <= 0) break;
            decoder.feed(std::string_view(buf, static_cast<std::size_t>(n)));
            for (;;) {
                auto m = decoder.next();
                if (!m || !*m) {
                    if (!m) std::fprintf(stderr, "victus debug-adapter: %s\n", m.error().message.c_str());
                    break;
                }
                inbox.push(std::move(**m));
            }
        }
        inbox.close();
    }).detach(); // blocked in read() until the client closes stdin; the process may exit first

    script::DapSession dap(send, [&inbox] { return inbox.wait(); });
    auto sink = std::make_shared<DapLogSink>();
    log::logger().add_sink(sink);
    std::unique_ptr<Headless> game;
    std::uint64_t frames = 0;
    bool running = false;

    auto handle = [&](const json::Value& m) {
        if (dap.handle(m)) return;
        const std::string command = m["command"].as_string_or("");
        if (command != "launch") {
            dap.respond(m, false, json::Value::object(), "unsupported request: " + command);
            return;
        }
        if (game) {
            dap.respond(m, false, json::Value::object(), "already launched");
            return;
        }
        const json::Value& args = m["arguments"];
        const std::string project = args["project"].as_string_or("");
        if (project.empty()) {
            dap.respond(m, false, json::Value::object(), "launch needs \"project\": the project folder");
            return;
        }
        const bool no_debug = args["noDebug"].as_bool(false);
        auto hs = std::make_unique<Headless>();
        if (auto st = hs->open(project, args["scene"].as_string_or(""), args["saves"].as_string_or(""),
                               player::kFixedRunSeed, !no_debug);
            !st) {
            dap.respond(m, false, json::Value::object(), st.error().message);
            return;
        }
        game = std::move(hs);
        frames = static_cast<std::uint64_t>(std::max<std::int64_t>(0, args["frames"].as_int(0)));
        if (!no_debug) dap.attach(*game->host, game->proj->root() / "assets");
        if (args["stopOnEntry"].as_bool(false)) game->host->request_pause();
        dap.respond(m, true);
        dap.event("initialized");
    };

    std::uint64_t f = 0;
    std::optional<Pacer> pacer;
    while (!dap.disconnected()) {
        if (!running) {
            auto m = inbox.wait();
            if (!m) break;
            handle(*m);
            running = game && dap.configured();
            if (running) pacer.emplace(60.0);
            continue;
        }
        while (auto m = inbox.poll()) handle(*m);
        if (dap.disconnected() || inbox.closed()) break;
        game->sim.tick(game->scene, 1.0f / 60.0f);
        game->input.end_frame();
        sink->flush_to(dap);
        dap.poll();
        if (frames && ++f >= frames) {
            json::Value body = json::Value::object();
            body.set("exitCode", 0);
            dap.event("exited", std::move(body));
            break;
        }
        pacer->wait();
    }
    sink->flush_to(dap);
    dap.detach();
    log::logger().remove_sink(sink);
    dap.event("terminated");
    game.reset();
    std::fflush(stderr);
    // The stdin reader thread may still be blocked: leave without running its destructors.
    std::_Exit(0);
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
    // Crash reports and session logs (ADR-0072). Only long-running commands keep a log file.
    std::unique_ptr<crash::Session> session;
    if (cmd != "version" && cmd != "--version" && cmd != "help" && cmd != "--help" && cmd != "-h" && cmd != "templates") {
        crash::Options co;
        co.app = "victus";
        co.log_file = cmd == "run" || cmd == "serve" || cmd == "simulate" || cmd == "crash-test";
        if (auto s = crash::Session::start(co)) session = std::move(*s);
        else std::fprintf(stderr, "victus: warning: crash reporting is off: %s\n", s.error().message.c_str());
    }
    if (cmd == "new") return cmd_new(args);
    if (cmd == "templates") return cmd_templates();
    if (cmd == "info") return cmd_info(args);
    if (cmd == "import") return cmd_import(args);
    if (cmd == "run") return cmd_run(args);
    if (cmd == "simulate") return cmd_simulate(args);
    if (cmd == "serve") return cmd_serve(args);
    if (cmd == "debug-adapter") return cmd_debug_adapter(args);
    if (cmd == "check") return cmd_check(args);
    if (cmd == "migrate") return cmd_migrate(args);
    if (cmd == "compare") return cmd_compare(args);
    if (cmd == "plugins") return cmd_plugins(args);
    if (cmd == "export") return cmd_export(args);
    if (cmd == "pack") return cmd_pack(args);
    if (cmd == "diagnose") return cmd_diagnose(args);
    if (cmd == "world") return cmd_world(args);
    if (cmd == "bake-lighting") return cmd_bake_lighting(args);
    if (cmd == "crash-test") return cmd_crash_test(args);
    if (cmd == "script-api") return cmd_script_api(args);
    if (cmd == "version" || cmd == "--version")
        return std::printf("VICTUS %s (script API %s)\n", build::version(), script::to_string(script::kScriptApiVersion).c_str()), 0;
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return std::printf("%s", kUsage), 0;
    std::fprintf(stderr, "victus: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
