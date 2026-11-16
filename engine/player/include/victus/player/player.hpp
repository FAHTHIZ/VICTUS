// SPDX-License-Identifier: Apache-2.0
// The VICTUS game player: opens a project, loads its startup scene and runs it in a window with
// the gameplay simulation. Used by `victus run`, the standalone player executable shipped with
// exported games, and automated smoke tests (frame limit + screenshot).
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>

namespace victus::player {

struct PlayerOptions {
    fs::Path project;            // project folder or victus.project
    std::string scene;           // project-relative; empty = startup scene
    int width{0}, height{0};     // 0 = project settings
    std::uint64_t max_frames{0}; // 0 = until closed; >0 also makes time deterministic (1/60 s)
    std::string screenshot;      // PNG written after the last frame (requires max_frames)
    bool fullscreen{false};
    bool hot_reload{false};      // reload changed scripts, textures, models and audio while running (development)
    std::string quality;         // low|medium|high|ultra; empty = project rendering.quality
    std::string connect;         // host[:port] of a `victus serve` server; empty = play offline
    std::string player_name{"player"}; // name shown to other players when connected
    std::string net_conditions;  // link emulation for testing, e.g. "latency=80,loss=0.05"
};

struct PlayerReport {
    std::uint64_t frames{0};
    double seconds{0};
    std::string backend;
    std::uint64_t script_errors{0};
    bool packaged{false};        // ran from an exported game.vpak
    std::string quality;         // quality tier used
    std::string net_status;      // last multiplayer status when --connect was used
    std::size_t assets_reloaded{0}; // textures/models/audio hot-reloaded while running
};

[[nodiscard]] Result<PlayerReport> run(const PlayerOptions& options);

} // namespace victus::player
