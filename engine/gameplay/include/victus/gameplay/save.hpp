// SPDX-License-Identifier: Apache-2.0
// Save games: named slots of JSON data in a per-game folder.
//
// Each slot is one `<slot>.vsave` file:
//
//     { "format": "victus.save", "version": 1, "data_version": 3, "saved_at": 1763280000,
//       "label": "Chapter 2", "checksum": "9f2c...", "data": { ... } }
//
// - Writes are atomic (temporary file + rename) and the previous good file is kept as
//   `<slot>.vsave.bak`. A slot whose file is unreadable, truncated or fails its checksum is
//   recovered from the backup (SaveFile::recovered tells the game).
// - `data_version` is the game's own save schema version. Reading a slot written by an older
//   version runs the registered migrations in order (v1 -> v2 -> ... -> current); a slot from a
//   newer version is refused rather than misread.
// - Slot names are 1-64 characters of letters, digits, '_', '-' and '.', not starting with '.'.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace victus::gameplay {

inline constexpr std::uint32_t kSaveFormatVersion = 1;
inline constexpr std::string_view kSaveExtension = ".vsave";

// Upgrades data written with schema version `from` to `from + 1`.
using SaveMigration = std::function<Result<json::Value>(json::Value data)>;

struct SaveFile {
    json::Value data;
    std::uint32_t data_version{1}; // the version the data was written with (before migration)
    std::int64_t saved_at{0};      // seconds since the Unix epoch
    std::string label;
    bool recovered{false};         // the slot was damaged and its backup was read instead
    bool migrated{false};          // migrations ran
};

struct SlotInfo {
    std::string slot;
    std::string label;
    std::int64_t saved_at{0};
    std::uint32_t data_version{1};
    std::uint64_t bytes{0};
    bool damaged{false}; // the main file is unreadable (a backup may still load)
};

[[nodiscard]] bool valid_slot_name(std::string_view slot) noexcept;

// Builds / parses the on-disk document (exposed for tests and tools).
[[nodiscard]] json::Value make_save_document(const json::Value& data, std::uint32_t data_version,
                                             std::int64_t saved_at, std::string_view label);
[[nodiscard]] Result<SaveFile> parse_save_document(std::string_view text);

class SaveStore {
public:
    // `directory` is created on the first write. `data_version` is the current schema version.
    explicit SaveStore(std::filesystem::path directory, std::uint32_t data_version = 1);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return dir_; }
    [[nodiscard]] std::uint32_t data_version() const noexcept { return data_version_; }
    void set_data_version(std::uint32_t version) noexcept { data_version_ = version < 1 ? 1 : version; }
    // Registers the migration from `from` to `from + 1` (replaces an earlier one).
    void add_migration(std::uint32_t from, SaveMigration migration);

    [[nodiscard]] Status write(std::string_view slot, const json::Value& data, std::string_view label = {});
    // Reads a slot (recovering from the backup when needed). With `run_migrations` false the data
    // is returned as written (checkpoints, tools).
    [[nodiscard]] Result<SaveFile> read(std::string_view slot, bool run_migrations = true) const;
    [[nodiscard]] bool exists(std::string_view slot) const;
    // Deletes the slot and its backup. Removing a missing slot is not an error.
    [[nodiscard]] Status remove(std::string_view slot);
    // Every slot in the folder, newest first.
    [[nodiscard]] std::vector<SlotInfo> list() const;

    [[nodiscard]] std::filesystem::path slot_path(std::string_view slot) const;

private:
    [[nodiscard]] Result<SaveFile> migrate(SaveFile file) const;
    std::filesystem::path dir_;
    std::uint32_t data_version_{1};
    std::map<std::uint32_t, SaveMigration> migrations_;
};

// The default save folder of a game: `<user data>/saves/<game>` with the name reduced to
// filename-safe characters.
[[nodiscard]] std::filesystem::path default_save_directory(std::string_view game_name);

} // namespace victus::gameplay
