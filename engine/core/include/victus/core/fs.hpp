// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::fs {

using Path = std::filesystem::path;

[[nodiscard]] Result<std::vector<std::uint8_t>> read_binary(const Path& path, std::uint64_t max_bytes = 1ull << 32);
[[nodiscard]] Result<std::string> read_text(const Path& path, std::uint64_t max_bytes = 1ull << 30);
// Writes via a sibling temporary file and rename so a crash never leaves a truncated file.
[[nodiscard]] Status write_atomic(const Path& path, std::span<const std::uint8_t> bytes);
[[nodiscard]] Status write_text_atomic(const Path& path, std::string_view text);

// Normalizes a project-relative path with forward slashes; rejects absolute paths and
// any '..' escape. Used for every path that comes from project, asset or mod data.
[[nodiscard]] Result<std::string> sanitize_relative(std::string_view relative);
// Resolves a sanitized relative path under root.
[[nodiscard]] Result<Path> resolve_under(const Path& root, std::string_view relative);

[[nodiscard]] Path executable_path();
[[nodiscard]] Path executable_dir();
// Per-user writable directory for VICTUS settings (recent projects, layouts, logs).
[[nodiscard]] Path user_data_dir();

} // namespace victus::fs
