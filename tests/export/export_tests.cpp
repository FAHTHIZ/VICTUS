// SPDX-License-Identifier: Apache-2.0
// `victus export`: a project becomes a pack that the player's packaged mode can run without
// sources, importers or loose files. These tests need no GPU: they check the pack contents,
// packaged asset loading (cooked texture + model), scenes, scripts and audio read through the mount.
#include "../assets/gltf_writer.hpp"
#include "doctest.h"
#include "victus/assets/database.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/pack.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/player/export.hpp"
#include "victus/project/project.hpp"
#include "victus/render/image.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/script/script.hpp"

#include <cmath>
#include <filesystem>

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

void write_bytes(const fs::Path& p, const std::vector<std::uint8_t>& b) {
    std::filesystem::create_directories(p.parent_path());
    REQUIRE(fs::write_atomic(p, b).ok());
}

// A 2D template project plus a textured glTF model and a standalone texture, so the export
// contains every asset kind: cooked textures/models, scripts, audio, scenes.
fs::Path make_project(const fs::Path& root) {
    const fs::Path dir = root / "src";
    auto proj = project::Project::create(dir, "Export Test!", "2d");
    REQUIRE(proj.ok());
    test::GltfFixture fx;
    write_bytes(dir / "assets" / "models" / "quad.bin", fx.bin);
    auto png = render::encode_png(render::make_checker(32, 4, 0xff0000ffu, 0xff00ff00u));
    REQUIRE(png.ok());
    write_bytes(dir / "assets" / "models" / "albedo.png", *png);
    write_bytes(dir / "assets" / "textures" / "logo.png", *png);
    fx.add_texture("albedo.png");
    fx.set_buffer_uri("quad.bin");
    REQUIRE(fs::write_text_atomic(dir / "assets" / "models" / "quad.gltf", json::stringify(fx.doc)).ok());
    return dir;
}

} // namespace

TEST_CASE("export: executable names are file-system safe") {
    CHECK(player::executable_base_name("Export Test!") == "Export_Test");
    CHECK(player::executable_base_name("  My   Game 2 ") == "My_Game_2");
    CHECK(player::executable_base_name("../../etc") == "etc");
    CHECK(player::executable_base_name("***") == "Game");
    CHECK(player::executable_base_name("v1.2-beta") == "v1.2-beta");
    CHECK(player::third_party_notices().find("Jolt Physics") != std::string_view::npos);
    CHECK(player::third_party_notices().find("Box2D") != std::string_view::npos);
    CHECK(player::third_party_notices().find("Recast & Detour") != std::string_view::npos);
    CHECK(player::third_party_notices().find("Luau") != std::string_view::npos);
    CHECK(player::third_party_notices().find("Dear ImGui") != std::string_view::npos);
    CHECK(player::third_party_notices().find("SIL OPEN FONT LICENSE") != std::string_view::npos);
    CHECK(player::third_party_notices().find("Permission is hereby granted") != std::string_view::npos);
}

TEST_CASE("export: pack contains cooked assets and runs in packaged mode") {
    TempDir tmp("victus_export");
    const fs::Path src = make_project(tmp.path);
    const fs::Path out = tmp.path / "out";

    player::ExportOptions o;
    o.project = src;
    o.output = out;
    o.include_player = false;
    auto r = player::export_game(o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->cooked_assets == 3); // quad.gltf, albedo.png, logo.png
    CHECK(r->executable.empty());
    CHECK(std::filesystem::is_regular_file(out / "game" / "game.vpak"));
    CHECK(std::filesystem::is_regular_file(out / "THIRD_PARTY_NOTICES.txt"));
    auto manifest = json::parse(fs::read_text(out / "export.json").value());
    REQUIRE(manifest.ok());
    CHECK((*manifest)["name"].as_string() == "Export Test!");
    CHECK((*manifest)["files"].size() == r->files);

    auto pk = pack::Pack::open(r->pack);
    REQUIRE(pk.ok());
    CHECK((*pk)->contains("victus.project"));
    CHECK((*pk)->contains("scenes/main.vscene"));
    CHECK((*pk)->contains("assets/scripts/platformer.luau"));
    CHECK((*pk)->contains("assets/audio/jump.wav"));
    CHECK((*pk)->contains(".victus/assets.index.json"));
    // Sources of cooked kinds, .meta sidecars and the import cache of the source project stay out.
    for (const auto& e : (*pk)->entries()) {
        CHECK_FALSE(e.path.ends_with(".png"));
        CHECK_FALSE(e.path.ends_with(".gltf"));
        CHECK_FALSE(e.path.ends_with(".meta"));
    }

    // Packaged mode: a fresh folder holding only the pack, mounted like the player does.
    const fs::Path game = tmp.path / "shipped" / "game";
    std::filesystem::create_directories(game);
    std::filesystem::copy_file(r->pack, game / "game.vpak");
    auto shipped = pack::Pack::open(game / "game.vpak");
    REQUIRE(shipped.ok());
    fs::mount(game, *shipped);
    REQUIRE(project::Project::is_project_dir(game));
    auto proj = project::Project::open(game);
    REQUIRE(proj.ok());
    CHECK(proj->settings().name == "Export Test!");
    CHECK(proj->scenes() == std::vector<std::string>{"scenes/main.vscene"});

    auto db = assets::AssetDatabase::open_packaged(game);
    REQUIRE(db.ok());
    CHECK((*db)->packaged());
    const auto* logo = (*db)->find_path("textures/logo.png");
    REQUIRE(logo);
    auto tex = (*db)->load_texture(logo->uuid);
    REQUIRE(tex.ok());
    REQUIRE(tex->mips.size() == 6); // 32x32 down to 1x1
    CHECK(tex->mips[0].width == 32);
    CHECK(tex->mips[0].height == 32);
    const auto* model = (*db)->find_path("models/quad.gltf");
    REQUIRE(model);
    auto m = (*db)->load_model(model->uuid);
    REQUIRE(m.ok());
    CHECK(m->meshes.size() == 1);
    CHECK(m->textures.size() == 1);
    const auto imported = (*db)->import_all();
    CHECK(imported.imported + imported.failed + imported.up_to_date == 0);
    CHECK_FALSE((*db)->set_settings(logo->uuid, json::Value::object()).ok());
    CHECK_FALSE(std::filesystem::exists(game / ".victus")); // nothing written next to the pack

    project::register_components();
    scene::Scene scene;
    REQUIRE(scene.load_file(game / "scenes" / "main.vscene").ok());
    CHECK(scene.find_by_name("Player") != scene::kNullEntity);

    // Scripts load from the pack.
    script::HostOptions so;
    so.assets_root = (*db)->assets_dir();
    auto host = script::ScriptHost::create(so);
    REQUIRE(host.ok());
    CHECK((*host)->describe("scripts/platformer.luau").ok());

    // Audio clips decode from the pack (memory VFS) and mix to non-silent output.
    audio::EngineOptions ao;
    ao.assets_root = (*db)->assets_dir();
    ao.null_device = true;
    auto eng = audio::AudioEngine::create(ao);
    REQUIRE(eng.ok());
    REQUIRE((*eng)->play_one_shot("audio/coin.wav").ok());
    std::vector<float> buf(2048 * (*eng)->channels());
    (*eng)->render(buf.data(), 2048);
    float peak = 0;
    for (const float s : buf) peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.01f);
    {
        audio::OneShot streamed;
        streamed.bus = audio::Bus::Music;
        CHECK((*eng)->play_one_shot("audio/music.wav", streamed).ok());
    }
    fs::unmount(game);

    // Re-exporting over a previous export works; exporting into an unrelated folder does not.
    CHECK(player::export_game(o).ok());
    std::filesystem::create_directories(tmp.path / "busy");
    REQUIRE(fs::write_text_atomic(tmp.path / "busy" / "notes.txt", "keep me").ok());
    o.output = tmp.path / "busy";
    CHECK(player::export_game(o).error().code == ErrorCode::AlreadyExists);
    o.output = src / "build";
    CHECK(player::export_game(o).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("export: the no-code templates ship with every behaviour and asset they use") {
    TempDir tmp("victus_export_nocode");
    project::register_components();
    for (const char* id : {"nocode", "nocode3d"}) {
        CAPTURE(id);
        const fs::Path src = tmp.path / id / "src";
        REQUIRE(project::Project::create(src, "No Code", id).ok());
        player::ExportOptions o;
        o.project = src;
        o.output = tmp.path / id / "out";
        o.include_player = false;
        auto r = player::export_game(o);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        const fs::Path game = tmp.path / id / "shipped";
        std::filesystem::create_directories(game);
        std::filesystem::copy_file(r->pack, game / "game.vpak");
        auto pk = pack::Pack::open(game / "game.vpak");
        REQUIRE(pk.ok());
        fs::mount(game, *pk);
        auto proj = project::Project::open(game);
        REQUIRE(proj.ok());
        auto db = assets::AssetDatabase::open_packaged(game);
        REQUIRE(db.ok());
        auto path = proj->startup_scene_path();
        REQUIRE(path.ok());
        scene::Scene scene;
        REQUIRE(scene.load_file(*path).ok());
        // The level is all there, as behaviours rather than scripts.
        auto& w = scene.world();
        const scene::Entity player = scene.find_by_name("Player");
        REQUIRE_FALSE(player.is_null());
        CHECK(w.get<script::HealthComponent>(player));
        CHECK((w.get<script::PlatformerMovementComponent>(player) || w.get<script::TopDownMovementComponent>(player)));
        const scene::Entity rules = scene.find_by_name("Game Rules");
        REQUIRE_FALSE(rules.is_null());
        CHECK(w.get<script::GameRulesComponent>(rules));
        std::size_t coins = 0, goals = 0, patrols = 0, scripts = 0;
        std::vector<scene::Entity> all = scene.roots();
        for (std::size_t i = 0; i < all.size(); ++i)
            for (const scene::Entity c : scene.children(all[i])) all.push_back(c);
        CHECK(all.size() > 20);
        for (const scene::Entity e : all) {
            coins += w.get<script::CollectibleComponent>(e) ? 1 : 0;
            goals += w.get<script::GoalComponent>(e) ? 1 : 0;
            patrols += w.get<script::PatrolComponent>(e) ? 1 : 0;
            scripts += w.get<script::ScriptComponent>(e) ? 1 : 0;
            // Every material and model the level points at was packed.
            if (const auto* m = w.get<scene::MeshRendererComponent>(e); m && !m->material_asset.is_nil())
                CHECK((*db)->find(m->material_asset));
        }
        CHECK(coins >= 5);
        CHECK(goals == 1);
        CHECK(patrols >= 1);
        CHECK(scripts == 0);
        if (std::string_view(id) == "nocode3d") {
            CHECK((*pk)->contains("assets/shaders/lava.vshader"));
            CHECK((*pk)->contains("assets/materials/lava.vmat"));
        }
        fs::unmount(game);
    }
}

TEST_CASE("export: broken scripts and missing references fail the export") {
    TempDir tmp("victus_export_bad");
    const fs::Path src = make_project(tmp.path);
    player::ExportOptions o;
    o.project = src;
    o.output = tmp.path / "out";
    o.include_player = false;

    REQUIRE(fs::write_text_atomic(src / "assets" / "scripts" / "broken.luau", "local M = {}\nfunction M:update(dt\nreturn M\n").ok());
    auto r = player::export_game(o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("broken.luau") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(o.output / "game" / "game.vpak"));
    std::filesystem::remove(src / "assets" / "scripts" / "broken.luau");
    std::filesystem::remove(src / "assets" / "scripts" / "broken.luau.meta");

    // A scene entity referencing a script that does not exist.
    std::filesystem::remove(src / "assets" / "scripts" / "coin.luau");
    std::filesystem::remove(src / "assets" / "scripts" / "coin.luau.meta");
    r = player::export_game(o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("missing script 'scripts/coin.luau'") != std::string::npos);

    // A missing player executable is reported when the player is requested.
    REQUIRE(fs::write_text_atomic(src / "assets" / "scripts" / "coin.luau", "local M = {}\nreturn M\n").ok());
    o.include_player = true;
    o.player_executable = tmp.path / "no_such_player";
    r = player::export_game(o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::NotFound);
}

TEST_CASE("export: materials ship verbatim and broken material references fail the export") {
    TempDir tmp("victus_export_mat");
    const fs::Path src = make_project(tmp.path);
    player::ExportOptions o;
    o.project = src;
    o.output = tmp.path / "out";
    o.include_player = false;
    auto db = assets::AssetDatabase::open(src);
    REQUIRE(db.ok());
    const Uuid logo = (*db)->find_path("textures/logo.png")->uuid;

    assets::MaterialAsset m;
    m.base_color_texture = logo;
    m.uv_scale = {2, 2};
    REQUIRE(assets::save_material_file(src / "assets" / "materials" / "tiles.vmat", m).ok());
    auto r = player::export_game(o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    {
        // Materials are used from source: the file ships as-is.
        auto pack = pack::Pack::open(r->pack);
        REQUIRE(pack.ok());
        CHECK((*pack)->contains("assets/materials/tiles.vmat"));
    }

    // A material pointing at a texture that does not exist.
    m.normal_texture = Uuid::generate();
    REQUIRE(assets::save_material_file(src / "assets" / "materials" / "tiles.vmat", m).ok());
    std::filesystem::remove_all(o.output);
    r = player::export_game(o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("Normal texture") != std::string::npos);

    // A malformed material file.
    REQUIRE(fs::write_text_atomic(src / "assets" / "materials" / "tiles.vmat", "{\"victus_material\": 1, \"metallic\": \"x\"}").ok());
    r = player::export_game(o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("metallic") != std::string::npos);
}

TEST_CASE("export: the scene's baked lightmap ships with it; a missing bake is a warning") {
    TempDir tmp("victus_export_lightmap");
    const fs::Path src = make_project(tmp.path);
    project::register_components();
    scene::Scene s;
    REQUIRE(s.load_file(src / "scenes" / "main.vscene").ok());
    const scene::Entity env = s.create("Baked Environment");
    s.world().add<gameplay::EnvironmentComponent>(env).lightmap = "scenes/main.vlightmap";
    REQUIRE(s.save_file(src / "scenes" / "main.vscene").ok());

    player::ExportOptions o;
    o.project = src;
    o.include_player = false;

    // Not baked yet: the game still exports (realtime lighting) and says why it looks different.
    o.output = tmp.path / "out_missing";
    auto missing = player::export_game(o);
    REQUIRE_MESSAGE(missing.ok(), (missing.ok() ? "" : missing.error().message));
    bool warned = false;
    for (const auto& w : missing->warnings) warned = warned || w.find("main.vlightmap") != std::string::npos;
    CHECK(warned);

    write_bytes(src / "scenes" / "main.vlightmap", {1, 2, 3, 4});
    o.output = tmp.path / "out";
    auto r = player::export_game(o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    auto pk = pack::Pack::open(r->pack);
    REQUIRE(pk.ok());
    CHECK((*pk)->contains("scenes/main.vlightmap"));
}
