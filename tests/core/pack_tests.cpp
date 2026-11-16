// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/hash.hpp"
#include "victus/core/pack.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>

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
