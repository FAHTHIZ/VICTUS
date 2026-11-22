// SPDX-License-Identifier: Apache-2.0
#include "victus/core/pack.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/os_random.hpp"
#include "victus/core/profiler.hpp"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

namespace victus::pack {
namespace {

constexpr char kMagic[4] = {'V', 'P', 'A', 'K'};
constexpr char kSigMagic[4] = {'V', 'S', 'I', 'G'};
constexpr std::string_view kSigContext = "VICTUS-VPAK-SIG-1";
constexpr std::string_view kKeyFileTag = "victus-signing-key-1";
constexpr std::size_t kHeaderSizeV1 = 40;
constexpr std::size_t kHeaderSizeV2 = 64;
constexpr std::size_t kSignatureSize = 4 + 4 + 32 + 64;
constexpr std::uint32_t kFlagSigned = 1u;
constexpr std::uint64_t kAlign = 16;
constexpr std::uint32_t kMaxEntries = 1u << 20;
// Entries smaller than this are never worth a zstd frame header and a decode call.
constexpr std::size_t kMinCompressBytes = 64;

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

Digest blake2b256(std::span<const std::uint8_t> data) {
    Digest d{};
    crypto_blake2b(d.data(), d.size(), data.data(), data.size());
    return d;
}

struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t pos{0};
    bool ok{true};
    template <class T>
    T get() {
        if (pos > b.size() || b.size() - pos < sizeof(T)) {
            ok = false;
            return T{};
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<std::uint64_t>(b[pos + i]) << (8 * i);
        pos += sizeof(T);
        return static_cast<T>(v);
    }
    std::string str(std::size_t n) {
        if (pos > b.size() || b.size() - pos < n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        return s;
    }
    template <std::size_t N>
    std::array<std::uint8_t, N> bytes() {
        std::array<std::uint8_t, N> a{};
        if (pos > b.size() || b.size() - pos < N) {
            ok = false;
            return a;
        }
        std::memcpy(a.data(), b.data() + pos, N);
        pos += N;
        return a;
    }
};

Error corrupt(const fs::Path& p, std::string_view what) {
    return make_error(ErrorCode::ParseError, std::format("pack '{}' is corrupt: {}", p.string(), what));
}

bool read_at(std::ifstream& in, std::uint64_t offset, std::span<std::uint8_t> out) {
    in.seekg(static_cast<std::streamoff>(offset));
    return out.empty() || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
}

std::vector<std::uint8_t> signed_message(std::span<const std::uint8_t> header) {
    std::vector<std::uint8_t> msg(kSigContext.begin(), kSigContext.end());
    msg.insert(msg.end(), header.begin(), header.end());
    return msg;
}

struct ZstdDctxDeleter {
    void operator()(ZSTD_DCtx* c) const noexcept { ZSTD_freeDCtx(c); }
};
struct ZstdCctxDeleter {
    void operator()(ZSTD_CCtx* c) const noexcept { ZSTD_freeCCtx(c); }
};

// One stored entry produced by the writer.
struct Stored {
    std::vector<std::uint8_t> compressed; // empty: stored raw
    Codec codec{Codec::Stored};
};

Stored compress_entry(ZSTD_CCtx* cctx, std::span<const std::uint8_t> raw, const WriteOptions& o) {
    Stored s;
    if (!o.compress || raw.size() < kMinCompressBytes) return s;
    std::vector<std::uint8_t> out(ZSTD_compressBound(raw.size()));
    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, std::clamp(o.level, 1, ZSTD_maxCLevel()));
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0); // the TOC digest already covers the frame
    const std::size_t n = ZSTD_compress2(cctx, out.data(), out.size(), raw.data(), raw.size());
    if (ZSTD_isError(n)) return s;
    const double saving = 1.0 - static_cast<double>(n) / static_cast<double>(raw.size());
    if (saving < o.min_saving) return s;
    out.resize(n);
    s.compressed = std::move(out);
    s.codec = Codec::Zstd;
    return s;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> parse_hex(std::string_view hex) {
    if (hex.size() != 2 * N) return std::nullopt;
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        const int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

template <std::size_t N>
std::string hex_of(const std::array<std::uint8_t, N>& a) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(2 * N);
    for (std::uint8_t b : a) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

} // namespace

// ---- signing keys -------------------------------------------------------------------------

PublicKey SigningKey::public_key() const {
    std::array<std::uint8_t, 32> seed_copy = seed; // key_pair wipes its seed argument
    std::array<std::uint8_t, 64> secret{};
    PublicKey pub{};
    crypto_ed25519_key_pair(secret.data(), pub.data(), seed_copy.data());
    crypto_wipe(secret.data(), secret.size());
    return pub;
}

Result<SigningKey> generate_signing_key() {
    SigningKey k;
    if (auto st = os_random_bytes(k.seed); !st) return std::move(st).error();
    return k;
}

Status save_signing_key(const SigningKey& key, const fs::Path& path) {
    std::string text = std::format("{}\n{}\n", kKeyFileTag, hex_of(key.seed));
    auto st = fs::write_text_atomic(path, text);
    crypto_wipe(text.data(), text.size());
    if (!st) return st;
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return {};
}

Result<SigningKey> load_signing_key(const fs::Path& path) {
    auto text = fs::read_text(path);
    if (!text) return std::move(text).error();
    std::string_view t = *text;
    const auto nl = t.find('\n');
    if (nl == std::string_view::npos || t.substr(0, nl) != kKeyFileTag)
        return make_error(ErrorCode::ParseError, std::format("'{}' is not a VICTUS signing key", path.string()));
    std::string_view hex = t.substr(nl + 1);
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.remove_suffix(1);
    auto seed = parse_hex<32>(hex);
    crypto_wipe(text->data(), text->size());
    if (!seed) return make_error(ErrorCode::ParseError, std::format("'{}': malformed signing key", path.string()));
    SigningKey k;
    k.seed = *seed;
    return k;
}

std::string to_hex(const PublicKey& key) { return hex_of(key); }

Result<PublicKey> public_key_from_hex(std::string_view hex) {
    auto k = parse_hex<32>(hex);
    if (!k) return make_error(ErrorCode::InvalidArgument, "a public key is 64 hexadecimal digits");
    return *k;
}

// ---- writer -------------------------------------------------------------------------------

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

Status PackWriter::write(const fs::Path& path, const WriteOptions& options, WriteStats* stats) const {
    if (files_.size() > kMaxEntries) return make_error(ErrorCode::LimitExceeded, "too many pack entries");
    std::vector<const std::string*> names;
    names.reserve(files_.size());
    for (const auto& [k, _] : files_) names.push_back(&k);
    std::sort(names.begin(), names.end(), [](const std::string* a, const std::string* b) { return *a < *b; });

    // Entries compress independently, so workers can take them in any order and the archive
    // stays byte-identical.
    std::vector<Stored> stored(names.size());
    if (options.compress) {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned workers = static_cast<unsigned>(std::min<std::size_t>(hw, std::max<std::size_t>(names.size(), 1)));
        std::atomic<std::size_t> next{0};
        auto work = [&] {
            std::unique_ptr<ZSTD_CCtx, ZstdCctxDeleter> cctx(ZSTD_createCCtx());
            if (!cctx) return;
            for (std::size_t i = next++; i < names.size(); i = next++)
                stored[i] = compress_entry(cctx.get(), files_.at(*names[i]), options);
        };
        std::vector<std::jthread> pool;
        for (unsigned t = 1; t < workers; ++t) pool.emplace_back(work);
        work();
    }

    WriteStats ws;
    std::vector<std::uint8_t> out(kHeaderSizeV2, 0);
    std::vector<std::uint8_t> toc;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& raw = files_.at(*names[i]);
        const bool packed = stored[i].codec != Codec::Stored;
        const std::span<const std::uint8_t> bytes = packed ? std::span<const std::uint8_t>(stored[i].compressed) : raw;
        while (out.size() % kAlign) out.push_back(0);
        const std::uint64_t offset = out.size();
        out.insert(out.end(), bytes.begin(), bytes.end());
        put_u16(toc, static_cast<std::uint16_t>(names[i]->size()));
        toc.insert(toc.end(), names[i]->begin(), names[i]->end());
        put_u64(toc, offset);
        put_u64(toc, bytes.size());
        put_u64(toc, raw.size());
        put_u32(toc, static_cast<std::uint32_t>(stored[i].codec));
        const Digest d = blake2b256(bytes);
        toc.insert(toc.end(), d.begin(), d.end());
        ws.payload_bytes += raw.size();
        ws.stored_bytes += bytes.size();
        ws.compressed_entries += packed ? 1 : 0;
    }
    ws.entries = names.size();
    while (out.size() % kAlign) out.push_back(0);
    const std::uint64_t toc_offset = out.size();
    out.insert(out.end(), toc.begin(), toc.end());

    std::vector<std::uint8_t> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    put_u32(header, kPackVersion);
    put_u32(header, static_cast<std::uint32_t>(names.size()));
    put_u32(header, options.signing_key ? kFlagSigned : 0u);
    put_u64(header, toc_offset);
    put_u64(header, toc.size());
    const Digest toc_digest = blake2b256(toc);
    header.insert(header.end(), toc_digest.begin(), toc_digest.end());
    std::memcpy(out.data(), header.data(), kHeaderSizeV2);

    if (options.signing_key) {
        std::array<std::uint8_t, 32> seed = options.signing_key->seed;
        std::array<std::uint8_t, 64> secret{};
        PublicKey pub{};
        crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
        const auto msg = signed_message(header);
        std::array<std::uint8_t, 64> sig{};
        crypto_ed25519_sign(sig.data(), secret.data(), msg.data(), msg.size());
        crypto_wipe(secret.data(), secret.size());
        out.insert(out.end(), kSigMagic, kSigMagic + 4);
        put_u32(out, 0);
        out.insert(out.end(), pub.begin(), pub.end());
        out.insert(out.end(), sig.begin(), sig.end());
    }
    if (stats) *stats = ws;
    return fs::write_atomic(path, out);
}

// ---- reader -------------------------------------------------------------------------------

Result<std::shared_ptr<Pack>> Pack::open(const fs::Path& path, const OpenOptions& options) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::NotFound, std::format("cannot open pack '{}': {}", path.string(), ec.message()));
    if (file_size < kHeaderSizeV1) return corrupt(path, "truncated header");
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error(ErrorCode::IoError, std::format("cannot open pack '{}'", path.string()));
    std::vector<std::uint8_t> header(std::min<std::uint64_t>(kHeaderSizeV2, file_size));
    if (!read_at(in, 0, header)) return corrupt(path, "truncated header");
    if (std::memcmp(header.data(), kMagic, 4) != 0) return corrupt(path, "bad magic");
    Reader h{header, 4};
    const auto version = h.get<std::uint32_t>();
    if (version < kPackMinVersion || version > kPackVersion)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("pack '{}' has version {}; this build reads {} to {}", path.string(), version, kPackMinVersion, kPackVersion));
    const std::size_t header_size = version == 1 ? kHeaderSizeV1 : kHeaderSizeV2;
    if (file_size < header_size) return corrupt(path, "truncated header");
    const auto count = h.get<std::uint32_t>();
    const auto flags = h.get<std::uint32_t>();
    const auto toc_offset = h.get<std::uint64_t>();
    const auto toc_size = h.get<std::uint64_t>();
    std::uint64_t toc_hash_v1 = 0;
    Digest toc_digest{};
    if (version == 1) toc_hash_v1 = h.get<std::uint64_t>();
    else toc_digest = h.bytes<32>();
    if (!h.ok) return corrupt(path, "truncated header");
    if (version >= 2 && (flags & ~kFlagSigned) != 0) return corrupt(path, std::format("unknown header flags {:#x}", flags));
    const bool is_signed = version >= 2 && (flags & kFlagSigned) != 0;
    if (count > kMaxEntries) return corrupt(path, "entry count");
    if (toc_offset < header_size || toc_offset > file_size || toc_size > file_size - toc_offset) return corrupt(path, "TOC range");
    const std::uint64_t toc_end = toc_offset + toc_size;
    if (version >= 2) {
        // Nothing may follow the TOC except exactly one signature block.
        const std::uint64_t expected = toc_end + (is_signed ? kSignatureSize : 0);
        if (expected != file_size) return corrupt(path, is_signed ? "signature block size" : "trailing bytes after the TOC");
    }

    std::optional<PublicKey> signer;
    if (is_signed) {
        std::array<std::uint8_t, kSignatureSize> block{};
        if (!read_at(in, toc_end, block)) return corrupt(path, "signature read");
        Reader s{block};
        const auto magic = s.bytes<4>();
        const auto reserved = s.get<std::uint32_t>();
        const auto pub = s.bytes<32>();
        const auto sig = s.bytes<64>();
        if (std::memcmp(magic.data(), kSigMagic, 4) != 0 || reserved != 0) return corrupt(path, "signature block");
        const auto msg = signed_message(std::span(header.data(), kHeaderSizeV2));
        if (crypto_ed25519_check(sig.data(), pub.data(), msg.data(), msg.size()) != 0)
            return make_error(ErrorCode::ParseError, std::format("pack '{}' has an invalid signature (modified after signing?)", path.string()));
        signer = pub;
    }
    if (!options.trusted_keys.empty()) {
        if (!signer)
            return make_error(ErrorCode::InvalidArgument, std::format("pack '{}' is not signed, but a signed pack is required", path.string()));
        if (std::find(options.trusted_keys.begin(), options.trusted_keys.end(), *signer) == options.trusted_keys.end())
            return make_error(ErrorCode::InvalidArgument,
                              std::format("pack '{}' is signed by an untrusted key {}", path.string(), to_hex(*signer)));
    } else if (options.require_signature && !signer) {
        return make_error(ErrorCode::InvalidArgument, std::format("pack '{}' is not signed, but a signed pack is required", path.string()));
    }

    std::vector<std::uint8_t> toc(static_cast<std::size_t>(toc_size));
    if (!read_at(in, toc_offset, toc)) return corrupt(path, "TOC read");
    if (version == 1 ? fnv1a64(std::span<const std::uint8_t>(toc)) != toc_hash_v1 : blake2b256(toc) != toc_digest)
        return corrupt(path, "TOC checksum");

    std::shared_ptr<Pack> p(new Pack());
    p->path_ = path;
    p->file_size_ = file_size;
    p->version_ = version;
    p->signer_ = signer;
    p->entries_.reserve(count);
    Reader r{toc};
    for (std::uint32_t i = 0; i < count; ++i) {
        EntryInfo e;
        const auto len = r.get<std::uint16_t>();
        e.path = r.str(len);
        e.offset = r.get<std::uint64_t>();
        std::uint32_t codec = 0;
        if (version == 1) {
            e.stored_size = e.size = r.get<std::uint64_t>();
            e.hash = r.get<std::uint64_t>();
            codec = r.get<std::uint32_t>();
        } else {
            e.stored_size = r.get<std::uint64_t>();
            e.size = r.get<std::uint64_t>();
            codec = r.get<std::uint32_t>();
            e.digest = r.bytes<32>();
        }
        if (!r.ok) return corrupt(path, "TOC truncated");
        auto clean = fs::sanitize_relative(e.path);
        if (!clean || *clean != e.path) return corrupt(path, std::format("invalid entry path '{}'", e.path));
        if (e.offset < header_size || e.offset > toc_offset || e.stored_size > toc_offset - e.offset)
            return corrupt(path, std::format("entry '{}' range", e.path));
        const bool known = codec == static_cast<std::uint32_t>(Codec::Stored) || (version >= 2 && codec == static_cast<std::uint32_t>(Codec::Zstd));
        if (!known) return corrupt(path, std::format("entry '{}' uses unknown codec {}", e.path, codec));
        e.codec = static_cast<Codec>(codec);
        if (e.codec == Codec::Stored && e.size != e.stored_size) return corrupt(path, std::format("entry '{}' size", e.path));
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
    VICTUS_PROFILE_SCOPE("pack.read");
    const EntryInfo* e = find(path);
    if (!e) return make_error(ErrorCode::NotFound, std::format("'{}' not in pack '{}'", path, path_.string()));
    if (e->size > max_bytes)
        return make_error(ErrorCode::LimitExceeded, std::format("'{}' is {} bytes; limit is {}", path, e->size, max_bytes));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(e->stored_size));
    {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return make_error(ErrorCode::IoError, std::format("cannot open pack '{}'", path_.string()));
        if (!read_at(in, e->offset, data))
            return make_error(ErrorCode::IoError, std::format("short read of '{}' from pack '{}'", path, path_.string()));
    }
    const bool intact = [&] {
        VICTUS_PROFILE_SCOPE("pack.verify");
        return version_ == 1 ? fnv1a64(std::span<const std::uint8_t>(data)) == e->hash : blake2b256(data) == e->digest;
    }();
    if (!intact) return make_error(ErrorCode::ParseError, std::format("'{}' in pack '{}' failed its checksum", path, path_.string()));
    if (e->codec == Codec::Stored) return data;

    // Only digest-verified frames reach the decoder; the frame must be one zstd frame that
    // declares and decodes to exactly the recorded size.
    const auto bad = [&](std::string_view why) {
        return make_error(ErrorCode::ParseError, std::format("'{}' in pack '{}': {}", path, path_.string(), why));
    };
    if (ZSTD_getFrameContentSize(data.data(), data.size()) != e->size) return bad("compressed frame size mismatch");
    if (ZSTD_findFrameCompressedSize(data.data(), data.size()) != data.size()) return bad("trailing bytes after the compressed frame");
    thread_local std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) return make_error(ErrorCode::OutOfMemory, "cannot create a zstd decoder");
    VICTUS_PROFILE_SCOPE("pack.decompress");
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(e->size));
    const std::size_t n = ZSTD_decompressDCtx(dctx.get(), raw.data(), raw.size(), data.data(), data.size());
    if (ZSTD_isError(n)) return bad(std::format("decompression failed: {}", ZSTD_getErrorName(n)));
    if (n != raw.size()) return bad("decompressed size mismatch");
    return raw;
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
