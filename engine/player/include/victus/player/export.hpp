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
#include "victus/core/pack.hpp"
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
    // Texture profile of the target platform (assets::texture_export_profile): "all" ships the
    // cooked cache as is, "desktop" drops ASTC, "android" ships ASTC at up to 1024 px (ADR-0149).
    std::string target{"all"};
    // Pack compression (ADR-0154): zstd per entry, entries that do not shrink by 3 % stay raw.
    bool compress{true};
    int compression_level{12};
    // Ed25519 key file (pack::save_signing_key) to sign game.vpak with; empty = unsigned. The
    // public key is recorded in export.json, and the player then refuses a pack not signed by it.
    fs::Path signing_key;
};

struct ExportReport {
    std::size_t files{0};             // entries in game.vpak
    std::size_t cooked_assets{0};     // textures/models shipped in cooked form
    std::uint64_t payload_bytes{0};   // sum of entry sizes
    std::uint64_t pack_bytes{0};      // size of game.vpak on disk
    fs::Path pack;
    fs::Path executable;              // empty when include_player is false
    std::size_t plugins{0};           // native plugins copied to game/plugins (ADR-0078)
    std::string target{"all"};
    std::uint64_t cooked_bytes_in{0};  // cooked asset bytes before the target profile
    std::uint64_t cooked_bytes_out{0}; // ... and as packed
    std::size_t compressed_entries{0};  // entries stored as zstd frames
    std::string signer;                // hex Ed25519 public key, empty when unsigned
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

// How the player opens an exported game's pack (ADR-0154): `explicit_key` (hex) or else the
// `signing_public_key` of the export.json next to `game_dir` becomes the only trusted signer.
// Neither = any unsigned pack, or a pack with a valid signature, is accepted. This catches a
// pack replaced or corrupted on disk; it is not a defence against someone who can also edit
// export.json or the executable (platform code signing covers those).
[[nodiscard]] Result<pack::OpenOptions> pack_open_options(const fs::Path& game_dir, std::string_view explicit_key = {});

} // namespace victus::player
