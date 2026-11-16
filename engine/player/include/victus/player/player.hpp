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
    bool hot_reload{false};      // re-read changed scripts while running (development)
};

struct PlayerReport {
    std::uint64_t frames{0};
    double seconds{0};
    std::string backend;
    std::uint64_t script_errors{0};
};

[[nodiscard]] Result<PlayerReport> run(const PlayerOptions& options);

} // namespace victus::player
