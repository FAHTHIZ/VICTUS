// SPDX-License-Identifier: Apache-2.0
// Asset references (ADR-0175): UUID and path references, rename/move with path rewrites,
// delete, unreferenced assets and refusal cases.
#include "victus/assets/references.hpp"
#include "victus/core/fs.hpp"

#include "doctest.h"

#include <algorithm>
#include <filesystem>

using namespace victus;
namespace sfs = std::filesystem;

namespace {

struct Proj {
    fs::Path root;
    Proj() {
        root = sfs::temp_directory_path() / ("victus_refs_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(root / "assets" / "audio");
        sfs::create_directories(root / "assets" / "textures");
        sfs::create_directories(root / "assets" / "models");
        sfs::create_directories(root / "scenes");
    }
    ~Proj() {
        std::error_code ec;
        sfs::remove_all(root, ec);
    }
    Proj(const Proj&) = delete;
    Proj& operator=(const Proj&) = delete;
    void write(const std::string& rel, const std::string& text) const { REQUIRE(fs::write_text_atomic(root / rel, text).ok()); }
    std::string read(const std::string& rel) const { return fs::read_text(root / rel).value_or(""); }
};

} // namespace

TEST_CASE("asset references: users by UUID and by path, dependencies, unreferenced, rename and delete") {
    Proj p;
    p.write("assets/textures/brick.png", "\x89PNG not really");
    p.write("assets/audio/hit.wav", "RIFF");
    p.write("assets/audio/unused.wav", "RIFF");
    p.write("assets/models/door.gltf", "{}");
    auto db = assets::AssetDatabase::open(p.root);
    REQUIRE(db.ok());
    auto& d = **db;
    const Uuid brick = d.find_path("textures/brick.png")->uuid;
    const Uuid hit = d.find_path("audio/hit.wav")->uuid;
    const Uuid unused = d.find_path("audio/unused.wav")->uuid;
    const Uuid door = d.find_path("models/door.gltf")->uuid;

    // A material names the texture by UUID, a scene names the material by UUID and the sound by
    // path, a script names the sound by path with single quotes.
    p.write("assets/materials/wall.vmat", "{\"victus_material\":1,\"base_color_texture\":\"" + brick.to_string() + "\"}");
    REQUIRE(d.refresh().errors.empty());
    const Uuid wall = d.find_path("materials/wall.vmat")->uuid;
    p.write("scenes/main.vscene", "{\"entities\":[{\"uuid\":\"" + Uuid::generate().to_string() + "\",\"material\":\"" + wall.to_string() +
                                      "\",\"sound\":\"audio/hit.wav\",\"model\":\"" + door.to_string() + "\"}]}");
    p.write("assets/scripts/player.luau", "local s = 'audio/hit.wav'\nlocal t = \"assets/audio/hit.wav\"\n");
    REQUIRE(d.refresh().errors.empty());
    const auto idx = assets::ReferenceIndex::build(d);
    CHECK(idx.files_scanned() >= 3);

    const auto brick_users = idx.users(brick);
    REQUIRE(brick_users.size() == 1);
    CHECK(brick_users[0].file == "assets/materials/wall.vmat");
    CHECK_FALSE(brick_users[0].by_path);

    const auto hit_users = idx.users(hit);
    REQUIRE(hit_users.size() == 2);
    CHECK(hit_users[0].file == "assets/scripts/player.luau");
    CHECK(hit_users[0].by_path);
    CHECK(hit_users[0].count == 2);
    CHECK(hit_users[1].file == "scenes/main.vscene");

    CHECK(idx.dependencies("scenes/main.vscene").size() == 3); // material, door, sound; entity UUID ignored
    CHECK(idx.users(wall).size() == 1);

    const auto orphans = idx.unreferenced(d);
    CHECK(std::find(orphans.begin(), orphans.end(), unused) != orphans.end());
    CHECK(std::find(orphans.begin(), orphans.end(), brick) == orphans.end());

    // Rename the sound into another folder: UUID kept, both path references rewritten.
    auto rr = assets::rename_asset(d, hit, "audio/sfx/impact.wav");
    REQUIRE_MESSAGE(rr.ok(), rr.error().message);
    CHECK(rr->rewritten.size() == 2);
    CHECK(sfs::exists(p.root / "assets/audio/sfx/impact.wav"));
    CHECK(sfs::exists(p.root / "assets/audio/sfx/impact.wav.meta"));
    CHECK_FALSE(sfs::exists(p.root / "assets/audio/hit.wav"));
    REQUIRE(d.find(hit));
    CHECK(d.find(hit)->path == "audio/sfx/impact.wav");
    CHECK(p.read("scenes/main.vscene").find("\"audio/sfx/impact.wav\"") != std::string::npos);
    CHECK(p.read("assets/scripts/player.luau") == "local s = 'audio/sfx/impact.wav'\nlocal t = \"assets/audio/sfx/impact.wav\"\n");
    CHECK(assets::ReferenceIndex::build(d).users(hit).size() == 2);

    // UUID references survive a texture rename untouched.
    REQUIRE(assets::rename_asset(d, brick, "textures/brick_wall.png").ok());
    CHECK(assets::ReferenceIndex::build(d).users(brick).size() == 1);

    // Refusals change nothing.
    CHECK_FALSE(assets::rename_asset(d, unused, "audio/sfx/impact.wav").ok());   // exists
    CHECK_FALSE(assets::rename_asset(d, unused, "../escape.wav").ok());          // unsafe
    CHECK_FALSE(assets::rename_asset(d, unused, "audio/.hidden.wav").ok());      // hidden
    CHECK_FALSE(assets::rename_asset(d, unused, "audio/unused.ogg").ok());       // type change
    CHECK_FALSE(assets::rename_asset(d, door, "props/door.gltf").ok());          // companions
    CHECK(assets::rename_asset(d, door, "models/front_door.gltf").ok());         // same folder is fine
    CHECK(sfs::exists(p.root / "assets/audio/unused.wav"));

    // Delete removes source, .meta and the record.
    REQUIRE(assets::delete_asset(d, unused).ok());
    CHECK_FALSE(sfs::exists(p.root / "assets/audio/unused.wav"));
    CHECK_FALSE(sfs::exists(p.root / "assets/audio/unused.wav.meta"));
    CHECK(d.find(unused) == nullptr);
    CHECK_FALSE(assets::delete_asset(d, unused).ok());
}
