// SPDX-License-Identifier: Apache-2.0
// Game export: turns a project into a self-contained, runnable folder.
//
//   <output>/<Executable>[.exe]          copy of victus_player
//   <output>/game/game.vpak              project file, scenes, scripts, audio and cooked assets
//   <output>/THIRD_PARTY_NOTICES.txt     licenses of everything linked into the player
//   <output>/export.json                 manifest: engine version, file list, sizes, hashes
//
// Source textures and models are not shipped: only their cooked form (already mip-mapped and
// GPU-ready) is packed, so players never run importers. The exported player mounts game.vpak
// over its game folder (fs::mount) and opens the asset database in packaged mode.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace victus::player {

struct ExportOptions {
    fs::Path project;               // project folder or victus.project
    fs::Path output;                // created if missing; must be empty or a previous export
    fs::Path player_executable;     // empty = victus_player next to the running executable
    std::string executable_name;    // empty = sanitized project name
    bool include_player{true};      // false = pack only (e.g. when the platform build adds its own)
};

struct ExportReport {
    std::size_t files{0};             // entries in game.vpak
    std::size_t cooked_assets{0};     // textures/models shipped in cooked form
    std::uint64_t payload_bytes{0};   // sum of entry sizes
    std::uint64_t pack_bytes{0};      // size of game.vpak on disk
    fs::Path pack;
    fs::Path executable;              // empty when include_player is false
    std::vector<std::string> warnings;
};

// Imports stale assets, validates every scene and script, then writes the export.
[[nodiscard]] Result<ExportReport> export_game(const ExportOptions& options);

// Name of the game folder next to an exported executable, and of the pack inside it.
inline constexpr std::string_view kGameFolder = "game";
inline constexpr std::string_view kPackFile = "game.vpak";

// License texts of the third-party code linked into the player (generated at build time).
[[nodiscard]] std::string_view third_party_notices() noexcept;

// File-system safe executable base name for a project name ("My Game!" -> "My_Game").
[[nodiscard]] std::string executable_base_name(std::string_view project_name);

} // namespace victus::player
