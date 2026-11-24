// SPDX-License-Identifier: Apache-2.0
// Tilemaps end to end: a .vtileset asset resolves through the database, painted cells are drawn
// by the sprite batch, and editing the tile set hot-reloads it (Slice 42).
#include "gpu_fixture.hpp"
#include "victus/physics/physics.hpp"
#include "victus/render/image.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/tilemap/tilemap.hpp"

#include <chrono>
#include <filesystem>

using namespace victus;
using victus::test::channel;
using victus::test::gpu;
namespace sfs = std::filesystem;

namespace {

void write_text(const sfs::path& file, std::string_view text, int bump_seconds) {
    REQUIRE(fs::write_atomic(file, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())).ok());
    sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(bump_seconds));
}

std::string tileset_json(const Uuid& tex, std::string_view collision) {
    return R"({"format":"victus.tileset","version":1,"texture":")" + tex.to_string() +
           R"(","columns":2,"rows":1,"collision":")" + std::string(collision) + R"("})";
}

struct Frame {
    std::vector<std::uint8_t> px;
    std::uint32_t sprites{0};
};

Frame draw(scene::Scene& s, runtime::GpuAssetCache& cache, const char* name) {
    constexpr std::uint32_t W = 64, H = 64;
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto batch = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    render::SpriteBatch& sb = *batch.value();
    s.update_transforms();
    sb.begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    Frame f;
    f.sprites = runtime::extract_sprites(s, cache, sb);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    f.px = dev.read_texture(target).value();
    test::save_output(name, W, H, f.px);
    dev.destroy(target);
    return f;
}

} // namespace

TEST_CASE("gpu: tilemap cells draw their tiles and the tile set hot-reloads") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const sfs::path project = sfs::temp_directory_path() / ("victus_tilemap_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets" / "tiles");
    // 16x8 tile texture: tile 0 red, tile 1 green.
    render::Image img{.width = 16, .height = 8, .pixels = {}};
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const bool left = x < 8;
            img.pixels.insert(img.pixels.end(), {std::uint8_t(left ? 255 : 0), std::uint8_t(left ? 0 : 255), 0, 255});
        }
    REQUIRE(render::save_png(project / "assets" / "tiles" / "tiles.png", img).ok());
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    REQUIRE((*db)->import_all().failed == 0);
    const Uuid tex = (*db)->find_path("tiles/tiles.png")->uuid;
    write_text(project / "assets" / "tiles" / "level.vtileset", tileset_json(tex, "solid"), 0);
    (void)(*db)->refresh();
    const auto* rec = (*db)->find_path("tiles/level.vtileset");
    REQUIRE(rec);
    CHECK(rec->kind == assets::AssetKind::Tileset);
    CHECK(std::string_view(assets::to_string(assets::AssetKind::Tileset)) == "tileset");
    CHECK(assets::kind_for_extension(".VTileset") == assets::AssetKind::Tileset);
    const Uuid tsid = rec->uuid;

    runtime::GpuAssetCache cache(*gpu().ctx, db->get());
    tilemap::TilemapSystem tiles(assets::tileset_provider(**db));
    runtime::AssetHotReloader reloader(**db, 0.0);
    scene::Scene s;
    tilemap::register_components();
    const scene::Entity e = s.create("Level");
    s.world().get<scene::Transform>(e)->position = {0, -0.5f, 0};
    auto& m = s.world().add<tilemap::TilemapComponent>(e);
    m.tileset = tsid;
    tilemap::set_tile(m, -1, 0, 0); // red at x in [-1, 0)
    tilemap::set_tile(m, 0, 0, 1);  // green at x in [0, 1)

    tiles.update(s);
    REQUIRE(s.world().get<tilemap::TilemapRuntime>(e)->tileset);
    Frame f = draw(s, cache, "tilemap_two_tiles");
    CHECK(f.sprites == 2);
    const std::uint32_t left = test::pixel(f.px, 64, 16, 32), right = test::pixel(f.px, 64, 48, 32);
    CHECK(channel(left, 0) > 200);
    CHECK(channel(left, 1) < 40);
    CHECK(channel(right, 1) > 200);
    CHECK(channel(right, 0) < 40);
    const std::uint32_t above = test::pixel(f.px, 64, 32, 4); // outside the row: background
    CHECK(channel(above, 0) < 20);
    CHECK(channel(above, 1) < 20);

    // Erasing a cell stops drawing it; swapping tiles swaps colours.
    auto* mm = s.world().get<tilemap::TilemapComponent>(e);
    tilemap::set_tile(*mm, -1, 0, 1);
    tilemap::set_tile(*mm, 0, 0, -1);
    f = draw(s, cache, "tilemap_edited");
    CHECK(f.sprites == 1);
    CHECK(channel(test::pixel(f.px, 64, 16, 32), 1) > 200);
    CHECK(channel(test::pixel(f.px, 64, 48, 32), 1) < 20);

    // Camera culling (ADR-0256): a 100x100 block beside the view adds no sprites...
    for (std::int32_t y = 0; y < 100; ++y)
        for (std::int32_t x = 5; x < 105; ++x) tilemap::set_tile(*mm, x, y, 1);
    f = draw(s, cache, "tilemap_culled");
    CHECK(f.sprites == 1);
    // ...and moving the map over it draws only the cells around the view (local x ~ [49, 51]).
    s.world().get<scene::Transform>(e)->position = {-50, -0.5f, 0};
    f = draw(s, cache, "tilemap_culled_moved");
    CHECK(f.sprites >= 4);
    CHECK(f.sprites <= 25);
    CHECK(channel(test::pixel(f.px, 64, 32, 32), 1) > 200);
    // A view wider than the painted range tests every cell instead of looking up the range.
    s.world().get<scene::Transform>(e)->scale = {0.005f, 0.005f, 1};
    s.world().get<scene::Transform>(e)->position = {0, -0.5f, 0};
    f = draw(s, cache, "tilemap_culled_zoomed_out");
    CHECK(f.sprites == 10001);
    s.world().get<scene::Transform>(e)->scale = {1, 1, 1};
    for (std::int32_t y = 0; y < 100; ++y)
        for (std::int32_t x = 5; x < 105; ++x) tilemap::set_tile(*mm, x, y, -1);
    f = draw(s, cache, "tilemap_edited_again");
    CHECK(f.sprites == 1);

    // Editing the tile set is reported and clears the system's cache.
    CHECK(s.world().get<physics::StaticShapes2DComponent>(e));
    CHECK(reloader.check().empty());
    write_text(project / "assets" / "tiles" / "level.vtileset", tileset_json(tex, "none"), 3);
    const auto changes = reloader.check();
    REQUIRE(changes.tilesets.size() == 1);
    CHECK(changes.tilesets[0] == "tiles/level.vtileset");
    runtime::AssetHotReloader::apply(changes, &cache, nullptr, &tiles);
    tiles.update(s);
    CHECK(s.world().get<tilemap::TilemapRuntime>(e)->tileset->default_collision == tilemap::TileCollision::None);
    CHECK(s.world().get<tilemap::TilemapRuntime>(e)->boxes == 0);

    // A tile set pointing at a missing texture is reported by the database.
    write_text(project / "assets" / "tiles" / "broken.vtileset", tileset_json(Uuid::generate(), "none"), 0);
    (void)(*db)->refresh();
    auto bad = (*db)->load_tileset((*db)->find_path("tiles/broken.vtileset")->uuid);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("broken.vtileset") != std::string::npos);
    std::error_code ec;
    sfs::remove_all(project, ec);
}
