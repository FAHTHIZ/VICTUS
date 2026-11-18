// SPDX-License-Identifier: Apache-2.0
// Standalone game player shipped with exported games. Runs the project in the folder given on
// the command line, or the `game/` folder next to the executable when none is given.
#include "victus/core/cli.hpp"
#include "victus/core/fs.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/player.hpp"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    victus::cli::Parser p("victus_player", "VICTUS game player");
    p.positional("project", "project folder (default: <executable dir>/game)")
        .option("frames", "exit after N frames", "0")
        .option("screenshot", "write the last frame to this PNG")
        .option("quality", "rendering quality: low, medium, high or ultra (default: project setting)")
        .flag("fullscreen", "run fullscreen")
        .option("connect", "join a multiplayer server at host[:port]")
        .flag("host", "host a multiplayer session and play in it (listen server)")
        .option("port", "UDP port to host on", "27960")
        .option("max-clients", "players who may join a hosted session", "8")
        .option("key", "host secret key file, created if missing (default: <project>/.victus/server.key)")
        .option("name", "player name shown to others when connected", "player")
        .option("server-key", "refuse servers whose public key differs")
        .option("password", "password of a protected server (or of the hosted session)")
        .flag("insecure", "no encryption: join a server started with --insecure, or host one")
        .option("saves", "save games folder (default: the per-user VICTUS saves folder)");
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
    o.quality = p.get("quality");
    o.connect = p.get("connect");
    o.player_name = p.get("name");
    o.server_key = p.get("server-key");
    o.net_password = p.get("password");
    o.insecure = p.has("insecure");
    o.host = p.has("host");
    o.host_port = static_cast<std::uint16_t>(p.get_number("port", 27960));
    o.max_clients = static_cast<std::uint16_t>(p.get_number("max-clients", 8));
    o.host_key = p.get("key");
    o.saves = p.get("saves");
    auto r = victus::player::run(o);
    if (!r) {
        std::fprintf(stderr, "victus_player: %s\n", r.error().message.c_str());
        // Exported games usually have no console: show fatal device errors in a dialog too.
        if (r.error().code == victus::ErrorCode::DeviceError) victus::platform::show_error_dialog("VICTUS", r.error().message);
        return r.error().code == victus::ErrorCode::DeviceError ? 3 : 1;
    }
    return 0;
}
