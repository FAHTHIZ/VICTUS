// SPDX-License-Identifier: Apache-2.0
// VICTUS pack archives (`.vpak`): the read-only container exported games ship their content in.
//
// Version 2 (written since ADR-0154; little-endian):
//   header  64 bytes: "VPAK", u32 version (2), u32 entry count, u32 flags (bit 0: signed),
//                     u64 TOC offset, u64 TOC size, BLAKE2b-256 of the TOC bytes (32)
//   data    entry payloads as stored (raw or one zstd frame), each aligned to 16 bytes
//   TOC     per entry: u16 path length, path bytes ('/'-separated, sanitized relative path),
//                      u64 offset, u64 stored size, u64 size, u32 codec (0 stored, 1 zstd),
//                      BLAKE2b-256 of the stored bytes (32)
//   signature (flag bit 0 only), directly after the TOC and ending the file: "VSIG", u32 0,
//                      Ed25519 public key (32), Ed25519 signature (64) over
//                      "VICTUS-VPAK-SIG-1" || the 64 header bytes (RFC 8032, SHA-512)
// The header digest covers the TOC, the TOC digests cover every payload, so the signature covers
// every byte a reader uses (only the zero padding between payloads is unauthenticated, and it is
// never read). Version 1 (40-byte header, FNV-1a 64 checksums, stored entries only, no
// signature) is still read.
//
// Every offset/size is validated against the file size and every path is re-sanitized on open,
// so a corrupted or hostile pack cannot read outside itself or escape a mount root. Payload
// digests are verified before anything is decompressed, and a zstd frame must decode to exactly
// the recorded size, so a hostile pack cannot make a reader allocate more than `max_bytes`.
//
// Threat model of signatures: a pack signed with a key the reader trusts cannot be modified or
// replaced by someone without the secret key. A reader that trusts no key (OpenOptions empty)
// still rejects a signed pack whose signature is invalid, but accepts unsigned packs. Signing does
// not protect a game whose executable can itself be replaced; that is the job of the platform's
// code signing.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::pack {

inline constexpr std::uint32_t kPackVersion = 2;    // written
inline constexpr std::uint32_t kPackMinVersion = 1; // oldest version still read
// Leading bytes that identify a pack's contents (the v2 header ends with the TOC digest; for v1
// they are the header plus the start of the first payload). Used to detect a changed bundled pack.
inline constexpr std::size_t kPackIdentityBytes = 64;

enum class Codec : std::uint32_t { Stored = 0, Zstd = 1 };

using Digest = std::array<std::uint8_t, 32>;    // BLAKE2b-256
using PublicKey = std::array<std::uint8_t, 32>; // Ed25519

struct EntryInfo {
    std::string path;
    std::uint64_t offset{0};
    std::uint64_t size{0};        // bytes after decompression (what read() returns)
    std::uint64_t stored_size{0}; // bytes in the archive
    std::uint64_t hash{0};        // version 1: FNV-1a 64 of the payload
    Digest digest{};              // version 2: BLAKE2b-256 of the stored bytes
    Codec codec{Codec::Stored};
};

// An Ed25519 signing key (the 32-byte RFC 8032 seed). Keep it out of version control.
struct SigningKey {
    std::array<std::uint8_t, 32> seed{};
    [[nodiscard]] PublicKey public_key() const;
};
[[nodiscard]] Result<SigningKey> generate_signing_key();
// Text file: "victus-signing-key-1\n<64 hex digits>\n", written with owner-only permissions.
[[nodiscard]] Status save_signing_key(const SigningKey& key, const fs::Path& path);
[[nodiscard]] Result<SigningKey> load_signing_key(const fs::Path& path);
[[nodiscard]] std::string to_hex(const PublicKey& key);
[[nodiscard]] Result<PublicKey> public_key_from_hex(std::string_view hex);

struct WriteOptions {
    bool compress{true};
    int level{12}; // zstd level 1..19; decoding speed does not depend on it
    // An entry is stored raw unless compression saves at least this fraction of its size, so
    // already-compressed data (Ogg, MP3, PNG, JPEG) costs no decode time.
    double min_saving{0.03};
    const SigningKey* signing_key{nullptr};
};

struct WriteStats {
    std::uint64_t payload_bytes{0}; // sum of entry sizes
    std::uint64_t stored_bytes{0};  // sum of stored sizes
    std::size_t compressed_entries{0};
    std::size_t entries{0};
};

class PackWriter {
public:
    // Adds (or replaces) a file. The path is sanitized; invalid paths are rejected.
    [[nodiscard]] Status add(std::string_view path, std::span<const std::uint8_t> bytes);
    [[nodiscard]] Status add_text(std::string_view path, std::string_view text);
    [[nodiscard]] Status add_file(std::string_view path, const fs::Path& source);
    [[nodiscard]] std::size_t size() const noexcept { return files_.size(); }
    [[nodiscard]] std::uint64_t payload_bytes() const noexcept;
    // Writes the archive atomically; entries are stored in sorted path order and compression is
    // deterministic, so the same input gives byte-identical archives.
    [[nodiscard]] Status write(const fs::Path& path, const WriteOptions& options = {}, WriteStats* stats = nullptr) const;

private:
    std::unordered_map<std::string, std::vector<std::uint8_t>> files_;
};

struct OpenOptions {
    // When non-empty, the pack must be signed by one of these keys.
    std::vector<PublicKey> trusted_keys;
    // Reject unsigned packs even when no key is listed (any valid signature is accepted).
    bool require_signature{false};
};

class Pack {
public:
    [[nodiscard]] static Result<std::shared_ptr<Pack>> open(const fs::Path& path, const OpenOptions& options = {});

    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] const EntryInfo* find(std::string_view path) const;
    // Thread-safe: each read uses its own file handle, so readers do not serialize.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read(std::string_view path, std::uint64_t max_bytes = 1ull << 32) const;
    // Sorted entry paths, optionally restricted to those under `prefix` ("dir" matches "dir/...").
    [[nodiscard]] std::vector<std::string> list(std::string_view prefix = {}) const;
    [[nodiscard]] const std::vector<EntryInfo>& entries() const noexcept { return entries_; }
    [[nodiscard]] const fs::Path& file_path() const noexcept { return path_; }
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    // The verified signer, or nothing for an unsigned pack.
    [[nodiscard]] const std::optional<PublicKey>& signer() const noexcept { return signer_; }

private:
    Pack() = default;
    fs::Path path_;
    std::uint64_t file_size_{0};
    std::uint32_t version_{0};
    std::optional<PublicKey> signer_;
    std::vector<EntryInfo> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

} // namespace victus::pack
