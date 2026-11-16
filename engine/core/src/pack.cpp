// SPDX-License-Identifier: Apache-2.0
#include "victus/core/pack.hpp"

#include "victus/core/hash.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace victus::pack {
namespace {

constexpr char kMagic[4] = {'V', 'P', 'A', 'K'};
constexpr std::size_t kHeaderSize = 40;
constexpr std::uint64_t kAlign = 16;
constexpr std::uint32_t kMaxEntries = 1u << 20;

void put_u16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t pos{0};
    bool ok{true};
    template <class T>
    T get() {
        if (b.size() - pos < sizeof(T) || pos > b.size()) {
            ok = false;
            return T{};
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<std::uint64_t>(b[pos + i]) << (8 * i);
        pos += sizeof(T);
        return static_cast<T>(v);
    }
    std::string str(std::size_t n) {
        if (b.size() - pos < n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        return s;
    }
};

Error corrupt(const fs::Path& p, std::string_view what) {
    return make_error(ErrorCode::ParseError, std::format("pack '{}' is corrupt: {}", p.string(), what));
}

} // namespace

Status PackWriter::add(std::string_view path, std::span<const std::uint8_t> bytes) {
    auto clean = fs::sanitize_relative(path);
    if (!clean) return std::move(clean).error();
    if (clean->size() > 0xffff) return make_error(ErrorCode::LimitExceeded, "pack path too long");
    files_[*clean] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    return {};
}

Status PackWriter::add_text(std::string_view path, std::string_view text) {
    return add(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Status PackWriter::add_file(std::string_view path, const fs::Path& source) {
    auto bytes = fs::read_binary(source);
    if (!bytes) return std::move(bytes).error();
    return add(path, *bytes);
}

std::uint64_t PackWriter::payload_bytes() const noexcept {
    std::uint64_t n = 0;
    for (const auto& [_, v] : files_) n += v.size();
    return n;
}

Status PackWriter::write(const fs::Path& path) const {
    if (files_.size() > kMaxEntries) return make_error(ErrorCode::LimitExceeded, "too many pack entries");
    std::vector<const std::string*> names;
    names.reserve(files_.size());
    for (const auto& [k, _] : files_) names.push_back(&k);
    std::sort(names.begin(), names.end(), [](const std::string* a, const std::string* b) { return *a < *b; });

    std::vector<std::uint8_t> out(kHeaderSize, 0);
    std::vector<std::uint8_t> toc;
    for (const std::string* name : names) {
        const auto& data = files_.at(*name);
        while (out.size() % kAlign) out.push_back(0);
        const std::uint64_t offset = out.size();
        out.insert(out.end(), data.begin(), data.end());
        put_u16(toc, static_cast<std::uint16_t>(name->size()));
        toc.insert(toc.end(), name->begin(), name->end());
        put_u64(toc, offset);
        put_u64(toc, data.size());
        put_u64(toc, fnv1a64(std::span<const std::uint8_t>(data)));
        put_u32(toc, 0);
    }
    while (out.size() % kAlign) out.push_back(0);
    const std::uint64_t toc_offset = out.size();
    out.insert(out.end(), toc.begin(), toc.end());

    std::vector<std::uint8_t> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    put_u32(header, kPackVersion);
    put_u32(header, static_cast<std::uint32_t>(names.size()));
    put_u32(header, 0);
    put_u64(header, toc_offset);
    put_u64(header, toc.size());
    put_u64(header, fnv1a64(std::span<const std::uint8_t>(toc)));
    std::memcpy(out.data(), header.data(), kHeaderSize);
    return fs::write_atomic(path, out);
}

Result<std::shared_ptr<Pack>> Pack::open(const fs::Path& path) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::NotFound, std::format("cannot open pack '{}': {}", path.string(), ec.message()));
    if (file_size < kHeaderSize) return corrupt(path, "truncated header");
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error(ErrorCode::IoError, std::format("cannot open pack '{}'", path.string()));
    std::vector<std::uint8_t> header(kHeaderSize);
    if (!in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(kHeaderSize))) return corrupt(path, "truncated header");
    if (std::memcmp(header.data(), kMagic, 4) != 0) return corrupt(path, "bad magic");
    Reader h{header, 4};
    const auto version = h.get<std::uint32_t>();
    const auto count = h.get<std::uint32_t>();
    (void)h.get<std::uint32_t>();
    const auto toc_offset = h.get<std::uint64_t>();
    const auto toc_size = h.get<std::uint64_t>();
    const auto toc_hash = h.get<std::uint64_t>();
    if (version != kPackVersion)
        return make_error(ErrorCode::VersionMismatch, std::format("pack '{}' has version {}; expected {}", path.string(), version, kPackVersion));
    if (count > kMaxEntries) return corrupt(path, "entry count");
    if (toc_offset < kHeaderSize || toc_offset > file_size || toc_size > file_size - toc_offset) return corrupt(path, "TOC range");
    std::vector<std::uint8_t> toc(static_cast<std::size_t>(toc_size));
    in.seekg(static_cast<std::streamoff>(toc_offset));
    if (toc_size && !in.read(reinterpret_cast<char*>(toc.data()), static_cast<std::streamsize>(toc_size))) return corrupt(path, "TOC read");
    if (fnv1a64(std::span<const std::uint8_t>(toc)) != toc_hash) return corrupt(path, "TOC checksum");

    std::shared_ptr<Pack> p(new Pack());
    p->path_ = path;
    p->file_size_ = file_size;
    p->entries_.reserve(count);
    Reader r{toc};
    for (std::uint32_t i = 0; i < count; ++i) {
        EntryInfo e;
        const auto len = r.get<std::uint16_t>();
        e.path = r.str(len);
        e.offset = r.get<std::uint64_t>();
        e.size = r.get<std::uint64_t>();
        e.hash = r.get<std::uint64_t>();
        e.codec = r.get<std::uint32_t>();
        if (!r.ok) return corrupt(path, "TOC truncated");
        auto clean = fs::sanitize_relative(e.path);
        if (!clean || *clean != e.path) return corrupt(path, std::format("invalid entry path '{}'", e.path));
        if (e.offset < kHeaderSize || e.offset > toc_offset || e.size > toc_offset - e.offset)
            return corrupt(path, std::format("entry '{}' range", e.path));
        if (e.codec != 0) return corrupt(path, std::format("entry '{}' uses unknown codec {}", e.path, e.codec));
        if (!p->index_.emplace(e.path, p->entries_.size()).second) return corrupt(path, std::format("duplicate entry '{}'", e.path));
        p->entries_.push_back(std::move(e));
    }
    if (r.pos != toc.size()) return corrupt(path, "trailing TOC bytes");
    return p;
}

const EntryInfo* Pack::find(std::string_view path) const {
    const auto it = index_.find(std::string(path));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

bool Pack::contains(std::string_view path) const { return find(path) != nullptr; }

Result<std::vector<std::uint8_t>> Pack::read(std::string_view path, std::uint64_t max_bytes) const {
    const EntryInfo* e = find(path);
    if (!e) return make_error(ErrorCode::NotFound, std::format("'{}' not in pack '{}'", path, path_.string()));
    if (e->size > max_bytes)
        return make_error(ErrorCode::LimitExceeded, std::format("'{}' is {} bytes; limit is {}", path, e->size, max_bytes));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(e->size));
    {
        std::lock_guard lock(io_mutex_);
        std::ifstream in(path_, std::ios::binary);
        if (!in) return make_error(ErrorCode::IoError, std::format("cannot open pack '{}'", path_.string()));
        in.seekg(static_cast<std::streamoff>(e->offset));
        if (e->size && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(e->size)))
            return make_error(ErrorCode::IoError, std::format("short read of '{}' from pack '{}'", path, path_.string()));
    }
    if (fnv1a64(std::span<const std::uint8_t>(data)) != e->hash)
        return make_error(ErrorCode::ParseError, std::format("'{}' in pack '{}' failed its checksum", path, path_.string()));
    return data;
}

std::vector<std::string> Pack::list(std::string_view prefix) const {
    std::vector<std::string> out;
    std::string pre(prefix);
    if (!pre.empty() && pre.back() != '/') pre.push_back('/');
    for (const auto& e : entries_)
        if (pre.empty() || e.path.starts_with(pre)) out.push_back(e.path);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace victus::pack
