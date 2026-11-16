// SPDX-License-Identifier: Apache-2.0
// VICTUS pack archives (`.vpak`): the read-only container exported games ship their content in.
//
// Layout (little-endian):
//   header  40 bytes: "VPAK", u32 version, u32 entry count, u32 reserved,
//                     u64 TOC offset, u64 TOC size, u64 FNV-1a 64 of the TOC bytes
//   data    entry payloads, each aligned to 16 bytes
//   TOC     per entry: u16 path length, path bytes ('/'-separated, sanitized relative path),
//                      u64 offset, u64 stored size, u64 FNV-1a 64 of the payload, u32 codec
//
// Every offset/size is validated against the file size and every path is re-sanitized on open,
// so a corrupted or hostile pack cannot read outside itself or escape a mount root. Payload
// hashes are verified on every read. Codec 0 = stored; other codecs are rejected (reserved for
// future compression).
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::pack {

inline constexpr std::uint32_t kPackVersion = 1;

struct EntryInfo {
    std::string path;
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t hash{0};
    std::uint32_t codec{0};
};

class PackWriter {
public:
    // Adds (or replaces) a file. The path is sanitized; invalid paths are rejected.
    [[nodiscard]] Status add(std::string_view path, std::span<const std::uint8_t> bytes);
    [[nodiscard]] Status add_text(std::string_view path, std::string_view text);
    [[nodiscard]] Status add_file(std::string_view path, const fs::Path& source);
    [[nodiscard]] std::size_t size() const noexcept { return files_.size(); }
    [[nodiscard]] std::uint64_t payload_bytes() const noexcept;
    // Writes the archive atomically; entries are stored in sorted path order (reproducible output).
    [[nodiscard]] Status write(const fs::Path& path) const;

private:
    std::unordered_map<std::string, std::vector<std::uint8_t>> files_;
};

class Pack {
public:
    [[nodiscard]] static Result<std::shared_ptr<Pack>> open(const fs::Path& path);

    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] const EntryInfo* find(std::string_view path) const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> read(std::string_view path, std::uint64_t max_bytes = 1ull << 32) const;
    // Sorted entry paths, optionally restricted to those under `prefix` ("dir" matches "dir/...").
    [[nodiscard]] std::vector<std::string> list(std::string_view prefix = {}) const;
    [[nodiscard]] const std::vector<EntryInfo>& entries() const noexcept { return entries_; }
    [[nodiscard]] const fs::Path& file_path() const noexcept { return path_; }

private:
    Pack() = default;
    fs::Path path_;
    std::uint64_t file_size_{0};
    std::vector<EntryInfo> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    mutable std::mutex io_mutex_;
};

} // namespace victus::pack
