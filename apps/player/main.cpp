// SPDX-License-Identifier: Apache-2.0
// Standalone game player shipped with exported games. Runs the project in the folder given on
// the command line, or the `game/` folder next to the executable when none is given.
#include "victus/core/cli.hpp"
#include "victus/core/fs.hpp"
#include "victus/player/player.hpp"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    victus::cli::Parser p("victus_player", "VICTUS game player");
    p.positional("project", "project folder (default: <executable dir>/game)")
        .option("frames", "exit after N frames", "0")
        .option("screenshot", "write the last frame to this PNG")
        .flag("fullscreen", "run fullscreen");
    if (auto st = p.parse(argc, argv); !st) return std::fprintf(stderr, "%s\n%s", st.error().message.c_str(), p.help().c_str()), 2;
    victus::player::PlayerOptions o;
    o.project = p.get("project");
    if (o.project.empty()) {
        std::error_code ec;
        const auto exe = std::filesystem::canonical(argc > 0 ? argv[0] : ".", ec);
        o.project = (ec ? std::filesystem::current_path() : exe.parent_path()) / "game";
    }
    o.max_frames = static_cast<std::uint64_t>(p.get_number("frames", 0));
    o.screenshot = p.get("screenshot");
    o.fullscreen = p.has("fullscreen");
    auto r = victus::player::run(o);
    if (!r) return std::fprintf(stderr, "victus_player: %s\n", r.error().message.c_str()), 1;
    return 0;
}
