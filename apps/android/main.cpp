// SPDX-License-Identifier: Apache-2.0
// Android entry point. SDLActivity loads libSDL3.so and this libmain.so, then calls SDL_main on its
// own thread. The exported game ships inside the APK as assets/game.vpak; it is installed to the
// app's internal storage (packs are opened by path) and run fullscreen with the mobile quality tier.
#include "victus/platform/main.hpp"

#include "victus/core/log.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/export.hpp"
#include "victus/player/mobile.hpp"
#include "victus/player/player.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace {
int fail(const std::string& message) {
    VICTUS_LOG_ERROR("android", "{}", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game failed to start", message.c_str(), nullptr);
    return 1;
}
} // namespace

int main(int /*argc*/, char** /*argv*/) {
    const std::string data = victus::platform::Platform::user_data_dir("VICTUS", "game");
    if (data.empty()) return fail("no writable app storage");
    auto installed = victus::player::install_bundled_pack(std::string(victus::player::kPackFile),
                                                          victus::fs::Path(data) / "game");
    if (!installed) return fail(installed.error().message);
    victus::player::PlayerOptions o;
    o.project = installed->game_dir;
    o.fullscreen = true;
    auto r = victus::player::run(o);
    if (!r) return fail(r.error().message);
    VICTUS_LOG_INFO("android", "game ended after {} frames", r->frames);
    return 0;
}
