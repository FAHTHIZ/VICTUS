// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::pack {
class Pack;
}

namespace victus::fs {

using Path = std::filesystem::path;

[[nodiscard]] Result<std::vector<std::uint8_t>> read_binary(const Path& path, std::uint64_t max_bytes = 1ull << 32);
[[nodiscard]] Result<std::string> read_text(const Path& path, std::uint64_t max_bytes = 1ull << 30);
// Writes via a hidden sibling temporary file and rename so a crash never leaves a truncated
// file. The data is flushed to the device (fsync / FlushFileBuffers) and read back before the
// rename, and the directory entry is flushed after it, so the new file survives power loss and a
// failed or short write (disk full, permissions) reports an error and leaves `path` untouched.
[[nodiscard]] Status write_atomic(const Path& path, std::span<const std::uint8_t> bytes);
[[nodiscard]] Status write_text_atomic(const Path& path, std::string_view text);

// ---- transactional saves with generation history ----------------------------------------------
// Checks a document's bytes (e.g. that a scene parses). Return ErrorCode::VersionMismatch for a
// readable file from a newer engine: such files are kept, never treated as damaged.
using Validator = std::function<Status(std::string_view bytes)>;

inline constexpr std::uint32_t kDefaultHistory = 2;
inline constexpr std::uint32_t kMaxHistory = 16;

struct SafeWriteOptions {
    std::uint32_t history{kDefaultHistory}; // prior verified generations kept (0..kMaxHistory)
    Validator validate;                     // optional; checked on the new bytes and the current file
};

// Generation `g` (1 = the previous save) of `path`: the hidden sibling ".<name>.vbak<g>". Hidden
// files are skipped by asset scans and file listings.
[[nodiscard]] Path history_path(const Path& path, std::uint32_t generation);
// Where a current file that failed validation is preserved (".<name>.damaged") instead of being
// overwritten or rotated into the history.
[[nodiscard]] Path damaged_path(const Path& path);

// Saves `bytes` to `path` as a transaction:
//   1. validates the new bytes (nothing is touched when they are rejected);
//   2. writes, flushes and reads back a temporary file;
//   3. copies the current file into generation 1 (shifting older ones; the oldest drops off)
//      when it validates, or moves it to damaged_path() when it does not;
//   4. atomically renames the temporary over `path` and flushes the directory.
// `path` always holds a complete file: the old one until step 4, the new one after it. An
// interruption at any point leaves the current file or a verified generation to recover.
[[nodiscard]] Status write_safely(const Path& path, std::string_view bytes, const SafeWriteOptions& options = {});

struct Recovered {
    std::string bytes;
    std::uint32_t generation{0};        // 0 = `path` itself, g = history_path(path, g)
    std::vector<std::string> rejected;  // "<file>: <reason>" for newer files that failed
};
// Reads the newest valid copy: `path`, then generations 1..history. A VersionMismatch from the
// validator stops the search (an older generation must not silently replace newer data).
[[nodiscard]] Result<Recovered> read_with_recovery(const Path& path, const Validator& validate,
                                                   std::uint32_t history = kDefaultHistory,
                                                   std::uint64_t max_bytes = 1ull << 30);
// Existing generations of `path`, newest first.
[[nodiscard]] std::vector<std::uint32_t> generations(const Path& path, std::uint32_t history = kMaxHistory);
// Makes generation `g` the current file again through write_safely, so the replaced file becomes
// generation 1 and the restore itself can be undone.
[[nodiscard]] Status restore_generation(const Path& path, std::uint32_t generation, const SafeWriteOptions& options = {});

// Before a migrated document is saved over its older original (ADR-0100): copies `path` to
// premigration_path(path) unless that copy already exists, so the oldest original is kept for
// rollback. No-op when `path` does not exist.
[[nodiscard]] Path premigration_path(const Path& path);
[[nodiscard]] Status keep_premigration_copy(const Path& path);

namespace testing {
// One-shot fault injection for the next atomic/safe write in this process (tests only).
enum class WriteFault : std::uint8_t {
    None,
    NoSpace,         // the data write fails as on a full disk
    CorruptTemp,     // a byte of the temporary flips after writing (read-back must catch it)
    CrashBeforeSwap, // the process "dies" after history rotation, before the final rename
};
void inject_write_fault(WriteFault fault) noexcept;
} // namespace testing

// Normalizes a project-relative path with forward slashes; rejects absolute paths and
// any '..' escape. Used for every path that comes from project, asset or mod data.
[[nodiscard]] Result<std::string> sanitize_relative(std::string_view relative);
// Resolves a sanitized relative path under root.
[[nodiscard]] Result<Path> resolve_under(const Path& root, std::string_view relative);

// Virtual mounts: a pack mounted at `root` makes its entries readable as `root/<entry>` through
// read_binary/read_text/exists/list_files (mounted content wins over loose files). Used by the
// player to run exported games from `game.vpak` without touching game code. Thread-safe.
void mount(const Path& root, std::shared_ptr<const pack::Pack> pack);
void unmount(const Path& root);
void unmount_all();
[[nodiscard]] bool is_mounted(const Path& root);
// True when `path` names an entry of a mounted pack (reads come from the pack, not the disk).
[[nodiscard]] bool is_virtual(const Path& path);
// True for a regular file on disk or an entry of a mounted pack.
[[nodiscard]] bool exists(const Path& path);
// Recursive file listing under `dir` (relative '/'-separated paths, sorted, de-duplicated),
// merging loose files and mounted pack entries. Hidden entries ('.'-prefixed) are skipped.
[[nodiscard]] std::vector<std::string> list_files(const Path& dir);

[[nodiscard]] Path executable_path();
[[nodiscard]] Path executable_dir();
// Per-user writable directory for VICTUS settings (recent projects, layouts, logs).
[[nodiscard]] Path user_data_dir();

} // namespace victus::fs
