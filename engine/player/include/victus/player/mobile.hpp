// SPDX-License-Identifier: Apache-2.0
// Mobile app support for the player: unpacking the game pack shipped inside the app bundle and
// suspending the game while the OS has it in the background.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/platform/platform.hpp"

#include <cstdint>
#include <string>

namespace victus::player {

struct BundledInstall {
    fs::Path game_dir;       // folder holding game.vpak; pass it as PlayerOptions::project
    bool copied{false};      // false = the installed pack already matched the bundled one
    std::uint64_t bytes{0};  // pack size
};

// Packs are opened by file path, but APK assets are not files, so the bundled pack is streamed to
// `<dest_dir>/game.vpak` on first launch and again whenever it changes (an app update). The
// installed pack is reused when its size and 40-byte header match the bundled pack; the header
// holds the hash of the table of contents, which holds the hash of every payload, so any content
// change is detected without reading the whole pack. A bundled file that is not a VICTUS pack is
// rejected before anything is written.
[[nodiscard]] Result<BundledInstall> install_bundled_pack(const std::string& bundled, const fs::Path& dest_dir);

// Tracks whether the OS has sent the app to the background. While suspended the player stops
// rendering (the surface may be gone), mutes audio and pauses the simulation unless it is in a
// networked session; on resume the first frame's delta time is reset so nothing jumps.
class LifecycleGate {
public:
    // Returns true when the event changed the suspended state.
    bool on_event(platform::EventType type) noexcept {
        const bool before = suspended_;
        if (type == platform::EventType::AppWillEnterBackground) suspended_ = true;
        else if (type == platform::EventType::AppDidEnterForeground) suspended_ = false;
        else if (type == platform::EventType::AppLowMemory) ++low_memory_warnings_;
        if (before && !suspended_) ++resumes_;
        return before != suspended_;
    }
    [[nodiscard]] bool suspended() const noexcept { return suspended_; }
    [[nodiscard]] std::uint32_t resumes() const noexcept { return resumes_; }
    [[nodiscard]] std::uint32_t low_memory_warnings() const noexcept { return low_memory_warnings_; }

private:
    bool suspended_{false};
    std::uint32_t resumes_{0};
    std::uint32_t low_memory_warnings_{0};
};

} // namespace victus::player
