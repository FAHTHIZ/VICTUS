// SPDX-License-Identifier: Apache-2.0
// The VICTUS game player: opens a project, loads its startup scene and runs it in a window with
// the gameplay simulation. Used by `victus run`, the standalone player executable shipped with
// exported games, and automated smoke tests (frame limit + screenshot).
#pragma once
#include "victus/core/profiler.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>

namespace victus::player {

// Script random seed of fixed-frame runs (`victus run --frames`, `victus simulate`) that do not
// choose one, so the same project, scene and frame count always produce the same game.
inline constexpr std::uint64_t kFixedRunSeed = 1;

struct PlayerOptions {
    fs::Path project;            // project folder or victus.project
    std::string scene;           // project-relative; empty = startup scene
    int width{0}, height{0};     // 0 = project settings
    std::uint64_t max_frames{0}; // 0 = until closed; >0 also makes time deterministic (1/60 s)
    std::string screenshot;      // PNG written after the last frame (requires max_frames)
    bool fullscreen{false};
    // Interactive runs load textures and models on background jobs and upload them under a
    // per-frame budget (V.A.U.L.T., ADR-0087); fixed-frame, recorded and replayed runs always
    // load synchronously so captures and screenshots stay deterministic.
    bool async_assets{true};
    bool hot_reload{false};      // reload changed scripts, textures, models and audio while running (development)
    std::string quality;         // low|medium|high|ultra; empty = project rendering.quality
    std::string connect;         // host[:port] of a `victus serve` server; empty = play offline
    std::string player_name{"player"}; // name shown to other players when connected
    std::string net_conditions;  // link emulation for testing, e.g. "latency=80,loss=0.05"
    std::string server_key;      // expected server public key (hex); empty = accept and report it
    std::string net_password;    // sent to servers started with --password (over the encrypted channel)
    bool insecure{false};        // connect without encryption (to servers started with --insecure)
    // Listen server: host a session on UDP `host_port` and play in it (others join with
    // connect). Encrypted with `host_key` (created if missing; empty = `<project>/.victus/
    // server.key`, or next to the user's saves for exported games) unless `insecure`;
    // `net_password` protects it.
    bool host{false};
    std::uint16_t host_port{0};  // 0 = net::kDefaultPort
    std::uint16_t max_clients{8};
    fs::Path host_key;
    // Save games folder; empty = `<user data>/saves/<game>` for exported games and
    // `<project>/.victus/saves` when running a project.
    fs::Path saves;
    // Regression captures (offline only): record this session's input, frame times, script
    // random seed and state digests to a .vcapture file, or replay one (keyboard ignored; the run
    // ends with the capture and reports the first frame whose state digest differs).
    fs::Path record;
    // Script random seed (math.random). 0 = kFixedRunSeed when max_frames is set (fixed-step
    // runs repeat exactly) and Luau's time-based seed in interactive sessions. Recordings store
    // the seed they used; replays take it from the capture.
    std::uint64_t random_seed{0};
    // Profiling (ADR-0061): write a frame-time/scope summary (JSON) and/or a Chrome/Perfetto
    // trace of the session (the first 600 frames) when it ends.
    fs::Path profile;
    fs::Path trace;
    fs::Path replay;
};

struct PlayerReport {
    std::uint64_t frames{0};
    double seconds{0};
    std::string backend;
    std::uint64_t script_errors{0};
    bool packaged{false};        // ran from an exported game.vpak
    std::size_t plugins_loaded{0}; // native plugins initialized from <project>/plugins (ADR-0078)
    std::string quality;         // quality tier used
    std::string net_status;      // last multiplayer status when --connect was used
    std::uint64_t assets_streamed{0}; // textures/models uploaded by the background loader
    std::size_t assets_reloaded{0}; // textures/models/audio hot-reloaded while running
    std::uint64_t digests_checked{0}; // replay: state digests compared
    std::string replay_mismatch;    // replay: first divergence ("frame N: expected X, got Y")
    std::uint64_t scene_loads{0};   // scene transitions scripts made (scene.load/reload)
    std::string final_scene;        // the scene running when the player stopped
    std::uint32_t world_cells_peak{0}; // world streaming: most cells active at once (ADR-0090)
    std::uint64_t world_cell_loads{0}; // world streaming: cells activated
    profiler::FrameReport profile;  // with PlayerOptions::profile or trace
};

[[nodiscard]] Result<PlayerReport> run(const PlayerOptions& options);

} // namespace victus::player
