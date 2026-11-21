// SPDX-License-Identifier: Apache-2.0
// Android support that runs on the host: installing the bundled game pack (absolute paths stand in
// for APK assets, both go through SDL file IO) and the background/foreground lifecycle gate.
#include "doctest.h"
#include "victus/core/pack.hpp"
#include "victus/core/hash.hpp"
#include "victus/platform/platform.hpp"
#include "victus/player/export.hpp"
#include "victus/player/mobile.hpp"

#include <filesystem>
#include <fstream>

using namespace victus;

namespace {
struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_pack(const fs::Path& path, const std::string& text, std::size_t big = 0) {
    pack::PackWriter w;
    REQUIRE(w.add_text("victus.project", text).ok());
    if (big) REQUIRE(w.add("big.bin", std::vector<std::uint8_t>(big, 0x5a)).ok());
    pack::WriteOptions o;
    o.compress = false; // keep the pack larger than one copy chunk
    REQUIRE(w.write(path, o).ok());
}

std::filesystem::file_time_type mtime(const fs::Path& p) { return std::filesystem::last_write_time(p); }
} // namespace

TEST_CASE("mobile: the bundled pack is installed once and reinstalled when it changes") {
    TempDir d("victus_mobile_install");
    const fs::Path bundled = d.path / "apk" / "game.vpak";
    std::filesystem::create_directories(bundled.parent_path());
    // Larger than one 1 MiB copy chunk so the streaming path is exercised.
    write_pack(bundled, "{\"v\":1}", (3u << 20) + 123);
    const fs::Path dest = d.path / "data" / "game";

    auto first = player::install_bundled_pack(bundled.string(), dest);
    REQUIRE(first.ok());
    CHECK(first->copied);
    CHECK(first->game_dir == dest);
    const fs::Path installed = dest / std::string(player::kPackFile);
    CHECK(std::filesystem::file_size(installed) == first->bytes);
    CHECK(first->bytes == std::filesystem::file_size(bundled));
    CHECK_FALSE(std::filesystem::exists(installed.string() + ".tmp"));
    auto opened = pack::Pack::open(installed);
    REQUIRE(opened.ok());
    CHECK((*opened)->contains("big.bin"));

    const auto stamp = mtime(installed);
    auto again = player::install_bundled_pack(bundled.string(), dest);
    REQUIRE(again.ok());
    CHECK_FALSE(again->copied); // same size and header: kept
    CHECK(mtime(installed) == stamp);

    // An app update ships a different pack of the same size: the TOC hash in the header differs.
    write_pack(bundled, "{\"v\":2}", (3u << 20) + 123);
    REQUIRE(std::filesystem::file_size(bundled) == first->bytes);
    auto updated = player::install_bundled_pack(bundled.string(), dest);
    REQUIRE(updated.ok());
    CHECK(updated->copied);
    auto reopened = pack::Pack::open(installed);
    REQUIRE(reopened.ok());
    auto text = (*reopened)->read("victus.project");
    REQUIRE(text.ok());
    CHECK(std::string(text->begin(), text->end()) == "{\"v\":2}");

    // A damaged install (truncated by the user or the OS) is replaced.
    std::filesystem::resize_file(installed, 100);
    auto repaired = player::install_bundled_pack(bundled.string(), dest);
    REQUIRE(repaired.ok());
    CHECK(repaired->copied);
    CHECK(std::filesystem::file_size(installed) == first->bytes);
}

TEST_CASE("mobile: missing or foreign bundled files are rejected without writing") {
    TempDir d("victus_mobile_reject");
    const fs::Path dest = d.path / "data";
    CHECK_FALSE(player::install_bundled_pack((d.path / "nope.vpak").string(), dest).ok());
    const fs::Path junk = d.path / "junk.vpak";
    { std::ofstream(junk, std::ios::binary) << std::string(200, 'x'); }
    auto r = player::install_bundled_pack(junk.string(), dest);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(std::filesystem::exists(dest / std::string(player::kPackFile)));
}

TEST_CASE("mobile: bundled prefix reads report the full size") {
    TempDir d("victus_mobile_prefix");
    const fs::Path f = d.path / "a.bin";
    { std::ofstream(f, std::ios::binary) << "0123456789"; }
    std::uint64_t size = 0;
    auto head = platform::Platform::read_bundled_prefix(f.string(), 4, &size);
    REQUIRE(head.ok());
    CHECK(size == 10);
    CHECK(std::string(head->begin(), head->end()) == "0123");
    auto all = platform::Platform::read_bundled_prefix(f.string(), 100, &size);
    REQUIRE(all.ok());
    CHECK(all->size() == 10);
}

TEST_CASE("mobile: the lifecycle gate suspends in the background and counts resumes") {
    using platform::EventType;
    player::LifecycleGate g;
    CHECK_FALSE(g.suspended());
    CHECK_FALSE(g.on_event(EventType::WindowResized));
    CHECK(g.on_event(EventType::AppWillEnterBackground));
    CHECK(g.suspended());
    CHECK_FALSE(g.on_event(EventType::AppWillEnterBackground)); // repeated: no change
    CHECK_FALSE(g.on_event(EventType::AppLowMemory));
    CHECK(g.low_memory_warnings() == 1);
    CHECK(g.on_event(EventType::AppDidEnterForeground));
    CHECK_FALSE(g.suspended());
    CHECK(g.resumes() == 1);
    CHECK_FALSE(g.on_event(EventType::AppDidEnterForeground));
    CHECK(g.resumes() == 1);
}
