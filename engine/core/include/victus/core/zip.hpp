// SPDX-License-Identifier: Apache-2.0
// Minimal ZIP archives (PKWARE APPNOTE 6.3.x subset): entries are stored (method 0, no
// compression), no ZIP64, no encryption. Enough for diagnostics bundles that any OS can open.
// The reader accepts only that subset and bounds-checks every offset (untrusted input).
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::zip {

// IEEE 802.3 CRC-32 as used by ZIP, gzip and PNG.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept;

class Writer {
public:
    // `name` is a relative '/'-separated path (no "..", no leading '/', no drive letters).
    // `unix_time` is stored as an MS-DOS date/time (2-second resolution, 1980..2107).
    [[nodiscard]] Status add(std::string_view name, std::span<const std::uint8_t> data, std::int64_t unix_time);
    [[nodiscard]] Status add(std::string_view name, std::string_view text, std::int64_t unix_time);
    [[nodiscard]] std::size_t entries() const noexcept { return central_count_; }
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    // Appends the central directory and returns the archive bytes; the writer is then empty.
    [[nodiscard]] std::vector<std::uint8_t> finish();

private:
    std::vector<std::uint8_t> out_, central_;
    std::vector<std::string> names_;
    std::size_t central_count_{0};
};

struct Entry {
    std::string name;
    std::vector<std::uint8_t> data;
};
// Reads a stored-only archive written by Writer (or any tool using method 0 without ZIP64);
// checks CRCs. Rejects unsafe names, compressed entries and inconsistent offsets.
[[nodiscard]] Result<std::vector<Entry>> read(std::span<const std::uint8_t> archive);

} // namespace victus::zip
