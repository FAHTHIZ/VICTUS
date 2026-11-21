// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/hash.hpp"
#include "victus/core/pack.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string_view>

using namespace victus;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        fs::unmount_all();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string as_text(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }

void patch_byte(const fs::Path& p, std::uint64_t offset, std::uint8_t xor_mask) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(f.good());
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ static_cast<char>(xor_mask));
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&c, 1);
}

} // namespace

TEST_CASE("pack: write, open, read, list; output is reproducible") {
    TempDir dir("victus_pack");
    pack::PackWriter w;
    REQUIRE(w.add_text("victus.project", "{\"name\":\"x\"}").ok());
    REQUIRE(w.add_text("assets/scripts/a.luau", "return {}").ok());
    REQUIRE(w.add_text("assets\\audio\\b.wav", "RIFF").ok()); // backslashes normalized
    std::vector<std::uint8_t> big(100000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 31u);
    REQUIRE(w.add("big.bin", big).ok());
    REQUIRE(w.add_text("empty.txt", "").ok());
    CHECK_FALSE(w.add_text("../escape.txt", "x").ok());
    CHECK_FALSE(w.add_text("/abs.txt", "x").ok());
    CHECK(w.size() == 5);
    REQUIRE(w.write(dir.path / "a.vpak").ok());
    REQUIRE(w.write(dir.path / "b.vpak").ok());
    CHECK(fs::read_binary(dir.path / "a.vpak").value() == fs::read_binary(dir.path / "b.vpak").value());

    auto p = pack::Pack::open(dir.path / "a.vpak");
    REQUIRE(p.ok());
    CHECK((*p)->entries().size() == 5);
    CHECK((*p)->contains("assets/audio/b.wav"));
    CHECK_FALSE((*p)->contains("assets/audio"));
    CHECK(as_text((*p)->read("assets/scripts/a.luau").value()) == "return {}");
    CHECK((*p)->read("big.bin").value() == big);
    CHECK((*p)->read("empty.txt").value().empty());
    CHECK((*p)->read("missing").error().code == ErrorCode::NotFound);
    CHECK((*p)->read("big.bin", 10).error().code == ErrorCode::LimitExceeded);
    for (const auto& e : (*p)->entries()) CHECK(e.offset % 16 == 0);
    const auto under = (*p)->list("assets");
    REQUIRE(under.size() == 2);
    CHECK(under[0] == "assets/audio/b.wav");
    CHECK(under[1] == "assets/scripts/a.luau");
    CHECK((*p)->list().size() == 5);
}

TEST_CASE("pack: corruption is detected") {
    TempDir dir("victus_pack_bad");
    pack::PackWriter w;
    REQUIRE(w.add_text("a.txt", "hello world").ok());
    REQUIRE(w.add_text("b.txt", "second").ok());
    const fs::Path good = dir.path / "good.vpak";
    REQUIRE(w.write(good).ok());
    auto open_copy = [&](const char* name, auto mutate) {
        const fs::Path p = dir.path / name;
        std::filesystem::copy_file(good, p, std::filesystem::copy_options::overwrite_existing);
        mutate(p);
        return pack::Pack::open(p);
    };
    // Payload byte flipped: the pack opens, but reading that entry fails its checksum.
    auto payload = open_copy("payload.vpak", [&](const fs::Path& p) {
        auto g = pack::Pack::open(good);
        patch_byte(p, (*g)->find("a.txt")->offset + 2, 0x20);
    });
    REQUIRE(payload.ok());
    CHECK((*payload)->read("a.txt").error().code == ErrorCode::ParseError);
    CHECK(as_text((*payload)->read("b.txt").value()) == "second");
    // Header and TOC damage is rejected on open.
    CHECK_FALSE(open_copy("magic.vpak", [](const fs::Path& p) { patch_byte(p, 0, 0xff); }).ok());
    CHECK(open_copy("version.vpak", [](const fs::Path& p) { patch_byte(p, 4, 0x07); }).error().code == ErrorCode::VersionMismatch);
    CHECK_FALSE(open_copy("toc.vpak", [](const fs::Path& p) { patch_byte(p, std::filesystem::file_size(p) - 3, 0x01); }).ok());
    CHECK_FALSE(open_copy("trunc.vpak", [](const fs::Path& p) { std::filesystem::resize_file(p, std::filesystem::file_size(p) - 8); }).ok());
    CHECK_FALSE(open_copy("tiny.vpak", [](const fs::Path& p) { std::filesystem::resize_file(p, 12); }).ok());
    CHECK(pack::Pack::open(dir.path / "missing.vpak").error().code == ErrorCode::NotFound);
}

namespace {
void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    for (int i = 0; i < 2; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::vector<std::uint8_t> compressible(std::size_t n) {
    std::vector<std::uint8_t> v;
    const std::string line = "{\"entity\": 17, \"component\": \"Transform\", \"position\": [1.0, 2.0, 3.0]},\n";
    while (v.size() < n) v.insert(v.end(), line.begin(), line.end());
    v.resize(n);
    return v;
}
std::vector<std::uint8_t> noise(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    for (auto& b : v) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return v;
}
} // namespace

TEST_CASE("pack v2: compressible entries are zstd frames, incompressible ones stay raw") {
    TempDir dir("victus_pack_zstd");
    const auto text = compressible(200000);
    const auto rnd = noise(50000);
    pack::PackWriter w;
    REQUIRE(w.add("scene.json", text).ok());
    REQUIRE(w.add("music.ogg", rnd).ok());
    REQUIRE(w.add_text("tiny.txt", "x").ok());
    pack::WriteStats st;
    REQUIRE(w.write(dir.path / "a.vpak", {}, &st).ok());
    CHECK(st.entries == 3);
    CHECK(st.compressed_entries == 1);
    CHECK(st.payload_bytes == text.size() + rnd.size() + 1);
    CHECK(st.stored_bytes < st.payload_bytes / 2);
    // Deterministic, including with parallel compression.
    REQUIRE(w.write(dir.path / "b.vpak").ok());
    CHECK(fs::read_binary(dir.path / "a.vpak").value() == fs::read_binary(dir.path / "b.vpak").value());

    auto p = pack::Pack::open(dir.path / "a.vpak");
    REQUIRE(p.ok());
    CHECK((*p)->version() == pack::kPackVersion);
    CHECK_FALSE((*p)->signer().has_value());
    const auto* scene = (*p)->find("scene.json");
    REQUIRE(scene);
    CHECK(scene->codec == pack::Codec::Zstd);
    CHECK(scene->size == text.size());
    CHECK(scene->stored_size < text.size() / 10);
    CHECK((*p)->find("music.ogg")->codec == pack::Codec::Stored);
    CHECK((*p)->find("tiny.txt")->codec == pack::Codec::Stored);
    CHECK((*p)->read("scene.json").value() == text);
    CHECK((*p)->read("music.ogg").value() == rnd);
    // max_bytes is checked against the decoded size before decompressing.
    CHECK((*p)->read("scene.json", text.size() - 1).error().code == ErrorCode::LimitExceeded);

    // Uncompressed packs are larger and still read the same.
    pack::WriteOptions raw;
    raw.compress = false;
    pack::WriteStats rst;
    REQUIRE(w.write(dir.path / "raw.vpak", raw, &rst).ok());
    CHECK(rst.compressed_entries == 0);
    CHECK(rst.stored_bytes == rst.payload_bytes);
    auto rp = pack::Pack::open(dir.path / "raw.vpak");
    REQUIRE(rp.ok());
    CHECK((*rp)->read("scene.json").value() == text);
    CHECK(std::filesystem::file_size(dir.path / "raw.vpak") > std::filesystem::file_size(dir.path / "a.vpak"));
}

TEST_CASE("pack v2: a damaged zstd frame never decodes to wrong bytes") {
    TempDir dir("victus_pack_zstd_bad");
    pack::PackWriter w;
    REQUIRE(w.add("scene.json", compressible(100000)).ok());
    const fs::Path good = dir.path / "good.vpak";
    REQUIRE(w.write(good).ok());
    auto good_pack = pack::Pack::open(good); // keep the pack alive: find() points into it
    REQUIRE(good_pack.ok());
    const auto* e = (*good_pack)->find("scene.json");
    REQUIRE(e);
    REQUIRE(e->codec == pack::Codec::Zstd);
    for (std::uint64_t at : {std::uint64_t{0}, std::uint64_t{5}, e->stored_size / 2, e->stored_size - 1}) {
        const fs::Path p = dir.path / "bad.vpak";
        std::filesystem::copy_file(good, p, std::filesystem::copy_options::overwrite_existing);
        patch_byte(p, e->offset + at, 0x41);
        auto opened = pack::Pack::open(p);
        REQUIRE(opened.ok()); // payload damage is found on read
        CHECK((*opened)->read("scene.json").error().code == ErrorCode::ParseError);
    }
}

TEST_CASE("pack v2: Ed25519 signatures") {
    TempDir dir("victus_pack_sig");
    auto key = pack::generate_signing_key();
    REQUIRE(key.ok());
    auto other = pack::generate_signing_key();
    REQUIRE(other.ok());
    CHECK(key->public_key() != other->public_key());

    // Key files round-trip and are not readable by others on POSIX.
    const fs::Path key_file = dir.path / "game.key";
    REQUIRE(pack::save_signing_key(*key, key_file).ok());
    auto loaded = pack::load_signing_key(key_file);
    REQUIRE(loaded.ok());
    CHECK(loaded->seed == key->seed);
#ifndef _WIN32
    const auto perms = std::filesystem::status(key_file).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    REQUIRE(fs::write_text_atomic(dir.path / "junk.key", "not a key\n").ok());
    CHECK_FALSE(pack::load_signing_key(dir.path / "junk.key").ok());
    const auto hex = pack::to_hex(key->public_key());
    CHECK(hex.size() == 64);
    CHECK(pack::public_key_from_hex(hex).value() == key->public_key());
    CHECK_FALSE(pack::public_key_from_hex("abc").ok());

    pack::PackWriter w;
    REQUIRE(w.add_text("victus.project", "{\"name\":\"signed\"}").ok());
    REQUIRE(w.add("scene.json", compressible(30000)).ok());
    pack::WriteOptions o;
    o.signing_key = &*key;
    const fs::Path signed_pak = dir.path / "signed.vpak";
    REQUIRE(w.write(signed_pak, o).ok());
    // Ed25519 is deterministic, so signed packs are reproducible too.
    REQUIRE(w.write(dir.path / "signed2.vpak", o).ok());
    CHECK(fs::read_binary(signed_pak).value() == fs::read_binary(dir.path / "signed2.vpak").value());
    const fs::Path unsigned_pak = dir.path / "unsigned.vpak";
    REQUIRE(w.write(unsigned_pak).ok());

    auto any = pack::Pack::open(signed_pak);
    REQUIRE(any.ok());
    REQUIRE((*any)->signer().has_value());
    CHECK(*(*any)->signer() == key->public_key());
    pack::OpenOptions trust;
    trust.trusted_keys.push_back(key->public_key());
    CHECK(pack::Pack::open(signed_pak, trust).ok());
    pack::OpenOptions trust_other;
    trust_other.trusted_keys.push_back(other->public_key());
    CHECK(pack::Pack::open(signed_pak, trust_other).error().code == ErrorCode::InvalidArgument);
    CHECK(pack::Pack::open(unsigned_pak, trust).error().code == ErrorCode::InvalidArgument);
    pack::OpenOptions must;
    must.require_signature = true;
    CHECK(pack::Pack::open(signed_pak, must).ok());
    CHECK_FALSE(pack::Pack::open(unsigned_pak, must).ok());

    // Any modified byte of a signed pack is detected: header/TOC/signature on open, payload on read.
    const auto size = std::filesystem::file_size(signed_pak);
    const auto* scene = (*any)->find("scene.json");
    REQUIRE(scene);
    int rejected_on_open = 0, rejected_on_read = 0;
    for (std::uint64_t at = 0; at < size; at += (at < 64 || at + 128 > size) ? 1 : 97) {
        // Alignment padding is never read, so it carries no meaning to protect.
        const bool padding = std::none_of((*any)->entries().begin(), (*any)->entries().end(), [&](const pack::EntryInfo& e) {
            return at >= e.offset && at < e.offset + e.stored_size;
        }) && at >= 64 && at < (*any)->entries().back().offset + (*any)->entries().back().stored_size;
        if (padding) continue;
        const fs::Path p = dir.path / "tampered.vpak";
        std::filesystem::copy_file(signed_pak, p, std::filesystem::copy_options::overwrite_existing);
        patch_byte(p, at, 0x01);
        auto opened = pack::Pack::open(p, trust);
        if (!opened) {
            ++rejected_on_open;
            continue;
        }
        bool all_ok = true;
        for (const auto& e : (*opened)->entries()) all_ok = all_ok && (*opened)->read(e.path).ok();
        CHECK_FALSE(all_ok);
        ++rejected_on_read;
    }
    CHECK(rejected_on_open > 64);
    CHECK(rejected_on_read > 0);
    // Stripping the signature (clearing the flag and cutting the block) breaks the trust check.
    {
        const fs::Path p = dir.path / "stripped.vpak";
        std::filesystem::copy_file(signed_pak, p, std::filesystem::copy_options::overwrite_existing);
        patch_byte(p, 12, 0x01); // flags bit 0
        std::filesystem::resize_file(p, size - 104);
        auto opened = pack::Pack::open(p);
        REQUIRE(opened.ok()); // a valid unsigned pack...
        CHECK_FALSE(pack::Pack::open(p, trust).ok()); // ...that a reader trusting the key refuses
    }
}

TEST_CASE("pack: version 1 archives are still read") {
    TempDir dir("victus_pack_v1");
    // Hand-built v1 archive: 40-byte header, FNV-1a 64 checksums, stored entries.
    const std::string a = "hello v1", b = "{\"name\":\"old\"}";
    std::vector<std::uint8_t> file(40, 0);
    auto align = [&] { while (file.size() % 16) file.push_back(0); };
    align();
    const std::uint64_t off_a = file.size();
    file.insert(file.end(), a.begin(), a.end());
    align();
    const std::uint64_t off_b = file.size();
    file.insert(file.end(), b.begin(), b.end());
    std::vector<std::uint8_t> toc;
    auto entry = [&](std::string_view path, std::uint64_t off, const std::string& data) {
        put_u16(toc, static_cast<std::uint16_t>(path.size()));
        toc.insert(toc.end(), path.begin(), path.end());
        put_u64(toc, off);
        put_u64(toc, data.size());
        put_u64(toc, fnv1a64(std::string_view(data)));
        put_u32(toc, 0);
    };
    entry("a.txt", off_a, a);
    entry("victus.project", off_b, b);
    const std::uint64_t toc_off = file.size();
    file.insert(file.end(), toc.begin(), toc.end());
    std::vector<std::uint8_t> header;
    header.insert(header.end(), {'V', 'P', 'A', 'K'});
    put_u32(header, 1);
    put_u32(header, 2);
    put_u32(header, 0);
    put_u64(header, toc_off);
    put_u64(header, toc.size());
    put_u64(header, fnv1a64(std::span<const std::uint8_t>(toc)));
    REQUIRE(header.size() == 40);
    std::copy(header.begin(), header.end(), file.begin());
    const fs::Path p = dir.path / "v1.vpak";
    REQUIRE(fs::write_atomic(p, file).ok());

    auto opened = pack::Pack::open(p);
    REQUIRE(opened.ok());
    CHECK((*opened)->version() == 1);
    CHECK(as_text((*opened)->read("a.txt").value()) == a);
    CHECK(as_text((*opened)->read("victus.project").value()) == b);
    CHECK((*opened)->find("a.txt")->hash == fnv1a64(std::string_view(a)));
    // A v1 pack can never satisfy a signature requirement.
    pack::OpenOptions must;
    must.require_signature = true;
    CHECK_FALSE(pack::Pack::open(p, must).ok());
    // v1 payload corruption is still caught.
    patch_byte(p, off_a, 0x20);
    CHECK((*pack::Pack::open(p))->read("a.txt").error().code == ErrorCode::ParseError);
}

TEST_CASE("fs: mounted packs overlay a folder") {
    TempDir dir("victus_mount");
    const fs::Path game = dir.path / "game";
    std::filesystem::create_directories(game / "scenes");
    REQUIRE(fs::write_text_atomic(game / "loose.txt", "loose").ok());
    REQUIRE(fs::write_text_atomic(game / "shadowed.txt", "disk").ok());
    REQUIRE(fs::write_text_atomic(game / "scenes" / "disk.vscene", "{}").ok());
    pack::PackWriter w;
    REQUIRE(w.add_text("victus.project", "{}").ok());
    REQUIRE(w.add_text("shadowed.txt", "pack").ok());
    REQUIRE(w.add_text("scenes/main.vscene", "{\"a\":1}").ok());
    REQUIRE(w.add_text("assets/scripts/x.luau", "return {}").ok());
    REQUIRE(w.write(dir.path / "game.vpak").ok());
    auto p = pack::Pack::open(dir.path / "game.vpak");
    REQUIRE(p.ok());

    CHECK_FALSE(fs::exists(game / "victus.project"));
    fs::mount(game, *p);
    CHECK(fs::is_mounted(game));
    CHECK(fs::is_mounted(game / "scenes" / ".."));
    CHECK(fs::exists(game / "victus.project"));
    CHECK(fs::is_virtual(game / "victus.project"));
    CHECK_FALSE(fs::is_virtual(game / "loose.txt"));
    CHECK(fs::exists(game / "loose.txt"));
    CHECK_FALSE(fs::exists(game / "assets")); // directories are not files
    CHECK(fs::read_text(game / "shadowed.txt").value() == "pack");
    CHECK(fs::read_text(game / "loose.txt").value() == "loose");
    CHECK(fs::read_text(game / "scenes" / "main.vscene").value() == "{\"a\":1}");
    CHECK(fs::read_text(game / "assets" / "scripts" / ".." / "scripts" / "x.luau").value() == "return {}");
    CHECK_FALSE(fs::read_text(game / "nope.txt").ok());

    const auto scenes = fs::list_files(game / "scenes");
    REQUIRE(scenes.size() == 2);
    CHECK(scenes[0] == "disk.vscene");
    CHECK(scenes[1] == "main.vscene");
    const auto all = fs::list_files(game);
    CHECK(std::find(all.begin(), all.end(), "assets/scripts/x.luau") != all.end());
    CHECK(std::find(all.begin(), all.end(), "loose.txt") != all.end());

    fs::unmount(game);
    CHECK_FALSE(fs::is_mounted(game));
    CHECK_FALSE(fs::exists(game / "victus.project"));
    CHECK(fs::read_text(game / "shadowed.txt").value() == "disk");
}
