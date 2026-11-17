// SPDX-License-Identifier: Apache-2.0
// Asset hot reload: changed textures are re-imported and swapped in the GPU cache, broken saves
// keep the previous version, and files that cannot be swapped are reported.
#include "gpu_fixture.hpp"
#include "victus/render/image.hpp"
#include "victus/runtime/hot_reload.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

using namespace victus;
using victus::test::gpu;
namespace sfs = std::filesystem;

namespace {
constexpr std::uint32_t kRed = 0xff0000ffu, kGreen = 0xff00ff00u, kBlue = 0xffff0000u;

// Writes a solid 8x8 PNG and moves its modification time forward, so coarse file-system clocks
// still see a change.
void write_png(const sfs::path& file, std::uint32_t rgba, int bump_seconds) {
    auto png = render::encode_png(render::make_checker(8, 1, rgba, rgba));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(file, *png).ok());
    sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(bump_seconds));
}

void write_text(const sfs::path& file, std::string_view text, int bump_seconds) {
    REQUIRE(fs::write_atomic(file, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())).ok());
    sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(bump_seconds));
}

std::array<std::uint8_t, 4> first_texel(rhi::TextureHandle tex) {
    auto px = gpu().ctx->device().read_texture(tex);
    REQUIRE(px.ok());
    REQUIRE(px->size() >= 4);
    return {(*px)[0], (*px)[1], (*px)[2], (*px)[3]};
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

TEST_CASE("hot reload: textures swap in place, broken saves keep the old version") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const sfs::path project = sfs::temp_directory_path() / ("victus_hot_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets" / "textures");
    sfs::create_directories(project / "assets" / "scripts");
    sfs::create_directories(project / "assets" / "scenes");
    write_png(project / "assets" / "textures" / "wall.png", kRed, 0);
    write_text(project / "assets" / "scripts" / "a.luau", "return {}", 0);
    write_text(project / "assets" / "scenes" / "main.vscene", "{}", 0);
    write_text(project / "assets" / "music.wav", "RIFF", 0);

    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    REQUIRE((*db)->import_all().failed == 0);
    const auto* rec = (*db)->find_path("textures/wall.png");
    REQUIRE(rec);
    const Uuid wall = rec->uuid;
    runtime::GpuAssetCache cache(*gpu().ctx, db->get());
    auto tex = cache.texture(wall);
    REQUIRE(tex.ok());
    CHECK(first_texel(*tex) == std::array<std::uint8_t, 4>{255, 0, 0, 255});

    runtime::AssetHotReloader reloader(**db, 0.5);
    CHECK(reloader.check().empty()); // baseline: nothing changed

    // A new colour is re-imported and the cache hands out the new texture.
    write_png(project / "assets" / "textures" / "wall.png", kGreen, 2);
    auto r = reloader.check();
    CHECK(contains(r.changed, "textures/wall.png"));
    REQUIRE(r.reimported.size() == 1);
    CHECK(r.reimported[0] == wall);
    CHECK(r.failed.empty());
    runtime::AssetHotReloader::apply(r, &cache, nullptr);
    tex = cache.texture(wall);
    REQUIRE(tex.ok());
    CHECK(first_texel(*tex) == std::array<std::uint8_t, 4>{0, 255, 0, 255});

    // A half-written file fails to import: reported, and the green version stays in use.
    write_text(project / "assets" / "textures" / "wall.png", "not a png", 4);
    r = reloader.check();
    CHECK(r.reimported.empty());
    REQUIRE(r.failed.size() == 1);
    CHECK(r.failed[0].find("textures/wall.png") != std::string::npos);
    runtime::AssetHotReloader::apply(r, &cache, nullptr);
    tex = cache.texture(wall);
    REQUIRE(tex.ok());
    CHECK(first_texel(*tex) == std::array<std::uint8_t, 4>{0, 255, 0, 255});
    // Loading from disk again (e.g. after the cache was cleared) also falls back to the last good import.
    auto cooked = (*db)->load_texture(wall);
    REQUIRE(cooked.ok());
    REQUIRE_FALSE(cooked->mips.empty());
    CHECK(cooked->mips[0].pixels[1] == 255); // still green

    // Saving a good file again recovers.
    write_png(project / "assets" / "textures" / "wall.png", kBlue, 6);
    r = reloader.check();
    REQUIRE(r.reimported.size() == 1);
    runtime::AssetHotReloader::apply(r, &cache, nullptr);
    CHECK(first_texel(*cache.texture(wall)) == std::array<std::uint8_t, 4>{0, 0, 255, 255});

    // Scripts are left to the script host; audio is reported for reload; scenes need a restart.
    write_text(project / "assets" / "scripts" / "a.luau", "return { x = 1 }", 8);
    CHECK(reloader.check().empty());
    write_text(project / "assets" / "music.wav", "RIFF2", 8);
    write_text(project / "assets" / "scenes" / "main.vscene", "{ }", 8);
    r = reloader.check();
    CHECK(r.reimported.empty());
    CHECK(contains(r.audio, "music.wav"));
    CHECK(contains(r.restart_needed, "scenes/main.vscene"));
    CHECK(r.lines().size() == 2);
    // Behaviour trees are reported so the AI system restarts the entities using them.
    write_text(project / "assets" / "guard.vbt", R"({"root": {"type": "succeed"}})", 9);
    r = reloader.check();
    CHECK(contains(r.behavior_trees, "guard.vbt"));
    CHECK(r.restart_needed.empty());
    // Sequences are reported for SequencerSystem::reload.
    write_text(project / "assets" / "intro.vseq", R"({"tracks": []})", 9);
    r = reloader.check();
    CHECK(contains(r.sequences, "intro.vseq"));
    CHECK(r.restart_needed.empty());

    // New files get metadata and are imported; poll() rate-limits scans.
    const auto scans = reloader.scans();
    write_png(project / "assets" / "textures" / "floor.png", kRed, 10);
    r = reloader.poll(100.0);
    CHECK(reloader.scans() == scans + 1);
    REQUIRE(r.reimported_paths.size() == 1);
    CHECK(r.reimported_paths[0] == "textures/floor.png");
    REQUIRE((*db)->find_path("textures/floor.png"));
    CHECK(reloader.poll(100.2).empty());
    CHECK(reloader.scans() == scans + 1);
    (void)reloader.poll(100.6);
    CHECK(reloader.scans() == scans + 2);

    std::error_code ec;
    sfs::remove_all(project, ec);
}
