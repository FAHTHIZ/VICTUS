// SPDX-License-Identifier: Apache-2.0
// Project-wide data migration (ADR-0100): finds the project file, scenes, prefabs and world
// descriptors that an older VICTUS wrote, reports what loading them would upgrade, and with
// `write` saves them in the current format after keeping each original as "<file>.premigration".
// Files from a newer VICTUS are reported and never touched. Used by `victus migrate` and the
// Studio's open-project check.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace victus::player {

struct MigrationEntry {
    std::string file;   // project-relative, '/' separators
    std::string format; // "victus.scene", ...
    std::uint32_t from{0};
    std::uint32_t to{0};
    std::vector<std::string> notes; // document and component steps, renames, kept newer data
    std::string error;              // could not be read or upgraded (newer version, damaged, failing step)
    bool written{false};            // saved in the current format (write mode)
};

struct MigrationScan {
    std::size_t scanned{0};
    std::vector<MigrationEntry> entries; // only files with something to report
    [[nodiscard]] std::size_t pending() const noexcept;  // upgradeable, not yet written
    [[nodiscard]] std::size_t failures() const noexcept; // entries with an error
};

[[nodiscard]] Result<MigrationScan> migrate_project(const fs::Path& project_dir, bool write);

} // namespace victus::player
