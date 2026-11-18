// SPDX-License-Identifier: Apache-2.0
#include "victus/core/zip.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <set>

namespace victus::zip {

namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50u, kCentralSig = 0x02014b50u, kEndSig = 0x06054b50u;
constexpr std::uint16_t kVersion = 20, kUtf8Flag = 0x0800;

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kTable = make_table();

void u16(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v & 0xff));
    o.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}
void u32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    u16(o, v & 0xffffu);
    u16(o, v >> 16);
}

bool safe_name(std::string_view n) {
    if (n.empty() || n.size() > 0xffff || n.front() == '/' || n.back() == '/') return false;
    if (n.find('\\') != std::string_view::npos || n.find(':') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start <= n.size()) {
        const std::size_t end = std::min(n.find('/', start), n.size());
        const std::string_view part = n.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    for (char c : n)
        if (static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

void dos_time(std::int64_t unix_time, std::uint16_t& time, std::uint16_t& date) {
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(unix_time < 315532800 ? 315532800 : unix_time); // 1980-01-01
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const int year = std::min(tm.tm_year + 1900, 2107);
    time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

std::uint32_t rd16(std::span<const std::uint8_t> b, std::size_t at) { return b[at] | (static_cast<std::uint32_t>(b[at + 1]) << 8); }
std::uint32_t rd32(std::span<const std::uint8_t> b, std::size_t at) { return rd16(b, at) | (rd16(b, at + 2) << 16); }

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
    crc = ~crc;
    for (std::uint8_t b : data) crc = kTable[(crc ^ b) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

Status Writer::add(std::string_view name, std::span<const std::uint8_t> data, std::int64_t unix_time) {
    if (!safe_name(name)) return make_error(ErrorCode::InvalidArgument, "zip: unsafe entry name '" + std::string(name) + "'");
    for (const auto& n : names_)
        if (n == name) return make_error(ErrorCode::AlreadyExists, "zip: duplicate entry '" + std::string(name) + "'");
    if (central_count_ >= 0xffff) return make_error(ErrorCode::LimitExceeded, "zip: too many entries");
    const std::uint64_t offset = out_.size();
    if (offset + 30 + name.size() + data.size() >= 0xffffffffull)
        return make_error(ErrorCode::LimitExceeded, "zip: archive would exceed 4 GiB (ZIP64 is not supported)");
    std::uint16_t t = 0, d = 0;
    dos_time(unix_time, t, d);
    const std::uint32_t crc = crc32(data);
    const auto size = static_cast<std::uint32_t>(data.size());
    const auto name_len = static_cast<std::uint32_t>(name.size());
    u32(out_, kLocalSig);
    u16(out_, kVersion);
    u16(out_, kUtf8Flag);
    u16(out_, 0); // stored
    u16(out_, t);
    u16(out_, d);
    u32(out_, crc);
    u32(out_, size);
    u32(out_, size);
    u16(out_, name_len);
    u16(out_, 0);
    out_.insert(out_.end(), name.begin(), name.end());
    out_.insert(out_.end(), data.begin(), data.end());

    u32(central_, kCentralSig);
    u16(central_, kVersion); // made by: MS-DOS / FAT attributes, version 2.0
    u16(central_, kVersion);
    u16(central_, kUtf8Flag);
    u16(central_, 0);
    u16(central_, t);
    u16(central_, d);
    u32(central_, crc);
    u32(central_, size);
    u32(central_, size);
    u16(central_, name_len);
    u16(central_, 0); // extra
    u16(central_, 0); // comment
    u16(central_, 0); // disk
    u16(central_, 0); // internal attributes
    u32(central_, 0); // external attributes
    u32(central_, static_cast<std::uint32_t>(offset));
    central_.insert(central_.end(), name.begin(), name.end());
    names_.emplace_back(name);
    ++central_count_;
    return {};
}

Status Writer::add(std::string_view name, std::string_view text, std::int64_t unix_time) {
    return add(name, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), unix_time);
}

std::vector<std::uint8_t> Writer::finish() {
    std::vector<std::uint8_t> out = std::move(out_);
    const auto cd_offset = static_cast<std::uint32_t>(out.size());
    const auto cd_size = static_cast<std::uint32_t>(central_.size());
    out.insert(out.end(), central_.begin(), central_.end());
    u32(out, kEndSig);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<std::uint32_t>(central_count_));
    u16(out, static_cast<std::uint32_t>(central_count_));
    u32(out, cd_size);
    u32(out, cd_offset);
    u16(out, 0);
    out_.clear();
    central_.clear();
    names_.clear();
    central_count_ = 0;
    return out;
}

Result<std::vector<Entry>> read(std::span<const std::uint8_t> b) {
    const auto bad = [](const std::string& why) { return make_error(ErrorCode::ParseError, "zip: " + why); };
    if (b.size() < 22) return bad("too small");
    // End of central directory: last 22 bytes plus at most a 64 KiB comment.
    std::size_t eocd = std::string::npos;
    const std::size_t lowest = b.size() > 22 + 0xffff ? b.size() - 22 - 0xffff : 0;
    for (std::size_t at = b.size() - 22 + 1; at-- > lowest;)
        if (rd32(b, at) == kEndSig) {
            eocd = at;
            break;
        }
    if (eocd == std::string::npos) return bad("no end of central directory");
    const std::uint32_t count = rd16(b, eocd + 10), cd_size = rd32(b, eocd + 12), cd_offset = rd32(b, eocd + 16);
    if (rd16(b, eocd + 4) != 0 || rd16(b, eocd + 6) != 0 || rd16(b, eocd + 8) != count) return bad("multi-disk archives are not supported");
    if (static_cast<std::uint64_t>(cd_offset) + cd_size > eocd) return bad("central directory out of bounds");
    std::vector<Entry> out;
    std::set<std::string> seen;
    std::size_t at = cd_offset;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > static_cast<std::size_t>(cd_offset) + cd_size || rd32(b, at) != kCentralSig) return bad("bad central directory entry");
        const std::uint32_t flags = rd16(b, at + 8), method = rd16(b, at + 10), crc = rd32(b, at + 16), csize = rd32(b, at + 20),
                            usize = rd32(b, at + 24), nlen = rd16(b, at + 28), xlen = rd16(b, at + 30), clen = rd16(b, at + 32),
                            local = rd32(b, at + 42);
        if (at + 46 + nlen > b.size()) return bad("entry name out of bounds");
        std::string name(reinterpret_cast<const char*>(b.data() + at + 46), nlen);
        if (!safe_name(name)) return bad("unsafe entry name '" + name + "'");
        if (!seen.insert(name).second) return bad("duplicate entry '" + name + "'");
        if (flags & 0x1u) return bad(name + ": encrypted entries are not supported");
        if (method != 0 || csize != usize) return bad(name + ": compressed entries are not supported");
        if (static_cast<std::uint64_t>(local) + 30 > b.size() || rd32(b, local) != kLocalSig) return bad(name + ": bad local header");
        const std::uint64_t data_at = static_cast<std::uint64_t>(local) + 30 + rd16(b, local + 26) + rd16(b, local + 28);
        if (data_at + usize > cd_offset) return bad(name + ": data out of bounds");
        Entry e;
        e.name = std::move(name);
        e.data.assign(b.begin() + static_cast<std::ptrdiff_t>(data_at), b.begin() + static_cast<std::ptrdiff_t>(data_at + usize));
        if (crc32(e.data) != crc) return bad(e.name + ": CRC mismatch");
        out.push_back(std::move(e));
        at += 46 + nlen + xlen + clen;
    }
    return out;
}

} // namespace victus::zip
