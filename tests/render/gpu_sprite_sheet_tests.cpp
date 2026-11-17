// SPDX-License-Identifier: Apache-2.0
// Sprite sheets end to end: a .vsheet asset animates a Sprite, the batch draws the current cell,
// and editing the sheet hot-reloads its clips (Slice 39).
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/render/image.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/scene_view.hpp"

#include <chrono>
#include <cmath>
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

std::string sheet_json(const Uuid& tex, std::string_view frames) {
    return R"({"format":"victus.spritesheet","version":1,"texture":")" + tex.to_string() +
           R"(","columns":2,"rows":1,"clips":{"blink":{"frames":)" + std::string(frames) + R"(,"fps":1}}})";
}

// Draws the scene's sprites into a W x H target and returns the centre pixel.
std::uint32_t draw_centre(scene::Scene& s, runtime::GpuAssetCache& cache, const char* name) {
    constexpr std::uint32_t W = 64, H = 64;
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto batch = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    render::SpriteBatch& sb = *batch.value();
    s.update_transforms();
    sb.begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    CHECK(runtime::extract_sprites(s, cache, sb) == 1);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    const auto px = dev.read_texture(target).value();
    test::save_output(name, W, H, px);
    dev.destroy(target);
    return test::pixel(px, W, W / 2, H / 2);
}

} // namespace

TEST_CASE("gpu: sprite sheet frames animate a sprite and hot-reload") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const sfs::path project = sfs::temp_directory_path() / ("victus_sheet_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets" / "sprites");
    // 16x8 sheet: left cell red, right cell green.
    render::Image img{.width = 16, .height = 8, .pixels = {}};
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const bool left = x < 8;
            img.pixels.insert(img.pixels.end(), {std::uint8_t(left ? 255 : 0), std::uint8_t(left ? 0 : 255), 0, 255});
        }
    REQUIRE(render::save_png(project / "assets" / "sprites" / "blink.png", img).ok());
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    REQUIRE((*db)->import_all().failed == 0);
    const Uuid tex = (*db)->find_path("sprites/blink.png")->uuid;
    write_text(project / "assets" / "sprites" / "blink.vsheet", sheet_json(tex, "[0, 1]"), 0);
    (void)(*db)->refresh();
    const auto* rec = (*db)->find_path("sprites/blink.vsheet");
    REQUIRE(rec);
    CHECK(rec->kind == assets::AssetKind::SpriteSheet);
    const Uuid sheet = rec->uuid;

    runtime::GpuAssetCache cache(*gpu().ctx, db->get());
    animation::AnimationSystem anim(animation::AnimationSettings{.sheets = assets::sprite_sheet_provider(**db)});
    runtime::AssetHotReloader reloader(**db, 0.0);
    scene::Scene s;
    const scene::Entity e = s.create("Blinker");
    auto& sp = s.world().add<scene::SpriteComponent>(e);
    sp.size = {1.5f, 1.5f};
    auto& a = s.world().add<animation::SpriteAnimatorComponent>(e);
    a.sheet = sheet;
    a.clip = "blink";

    anim.update(s, 0.0f);
    auto c = draw_centre(s, cache, "sprite_sheet_frame0");
    CHECK(channel(c, 0) > 200);
    CHECK(channel(c, 1) < 40);
    anim.update(s, 1.2f); // 1 fps: the second (green) frame
    c = draw_centre(s, cache, "sprite_sheet_frame1");
    CHECK(channel(c, 1) > 200);
    CHECK(channel(c, 0) < 40);

    // Editing the clip to show only frame 0 is picked up after hot reload.
    CHECK(reloader.check().empty());
    write_text(project / "assets" / "sprites" / "blink.vsheet", sheet_json(tex, "[0]"), 3);
    const auto changes = reloader.check();
    REQUIRE(changes.sprite_sheets.size() == 1);
    CHECK(changes.sprite_sheets[0] == "sprites/blink.vsheet");
    runtime::AssetHotReloader::apply(changes, &cache, &anim);
    anim.update(s, 0.0f);
    CHECK(anim.sprite_status(s, e).frame == 0);
    c = draw_centre(s, cache, "sprite_sheet_reloaded");
    CHECK(channel(c, 0) > 200);

    // A sheet pointing at a missing texture is reported by the database.
    write_text(project / "assets" / "sprites" / "broken.vsheet", sheet_json(Uuid::generate(), "[0]"), 0);
    (void)(*db)->refresh();
    const Uuid broken = (*db)->find_path("sprites/broken.vsheet")->uuid;
    auto bad = (*db)->load_sprite_sheet(broken);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("broken.vsheet") != std::string::npos);
    std::error_code ec;
    sfs::remove_all(project, ec);
}

TEST_CASE("gpu: nearest texture filter keeps pixel art crisp") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    // 2x1 texture: red | green, magnified to 64 px wide.
    render::Image img{.width = 2, .height = 1, .pixels = {255, 0, 0, 255, 0, 255, 0, 255}};
    auto tex = ctx.create_texture(img, {false, false, "rg"});
    REQUIRE(tex.ok());
    constexpr std::uint32_t W = 64, H = 16;
    auto batch = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    render::SpriteBatch& sb = *batch.value();
    const auto draw = [&](bool nearest_sprite, bool pixel_art) {
        sb.begin(math::Mat4::orthographic(0, 64, 0, 16, -1, 1), pixel_art);
        render::Sprite s;
        s.texture = *tex;
        s.position = {32, 8};
        s.size = {64, 16};
        s.nearest = nearest_sprite;
        sb.draw(s);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(sb.prepare(cmd).ok());
        rhi::RenderPassDesc rp;
        rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
        auto pass = cmd.begin_render_pass(rp);
        sb.render(pass, cmd);
        pass.end();
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    // Just left of the middle: linear blends red and green, nearest is pure red.
    const auto linear = draw(false, false);
    const auto lp = test::pixel(linear, W, 30, 8);
    CHECK(channel(lp, 1) > 30);
    CHECK(channel(lp, 0) < 230);
    const auto nearest = draw(true, false);
    const auto np = test::pixel(nearest, W, 30, 8);
    CHECK(channel(np, 0) == 255);
    CHECK(channel(np, 1) == 0);
    CHECK(channel(test::pixel(nearest, W, 33, 8), 1) == 255);
    const auto forced = draw(false, true); // begin(.., pixel_art = true) applies to every sprite
    CHECK(channel(test::pixel(forced, W, 30, 8), 1) == 0);

    // Linear and nearest sprites of one texture are split into two batches.
    sb.begin(math::Mat4::orthographic(0, 64, 0, 16, -1, 1));
    render::Sprite a;
    a.texture = *tex;
    render::Sprite b = a;
    b.nearest = true;
    sb.draw(a);
    sb.draw(b);
    sb.draw(a);
    {
        auto cmd = dev.begin_commands();
        REQUIRE(sb.prepare(cmd).ok());
        cmd.submit_and_wait();
    }
    CHECK(sb.stats().batches == 2);
    dev.destroy(*tex);

    // The asset cache reads "filter": "nearest" from a texture's import settings.
    const sfs::path project = sfs::temp_directory_path() / ("victus_filter_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets");
    REQUIRE(render::save_png(project / "assets" / "pixel.png", img).ok());
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    REQUIRE((*db)->import_all().failed == 0);
    const Uuid id = (*db)->find_path("pixel.png")->uuid;
    runtime::GpuAssetCache cache(ctx, db->get());
    CHECK_FALSE(cache.texture_nearest(id));
    assets::TextureImportSettings ts = assets::TextureImportSettings::from_json((*db)->find(id)->settings);
    CHECK_FALSE(ts.nearest);
    ts.nearest = true;
    REQUIRE((*db)->set_settings(id, ts.to_json()).ok());
    CHECK((*db)->find(id)->settings["filter"].as_string() == "nearest");
    CHECK_FALSE(cache.texture_nearest(id)); // cached until evicted
    cache.evict(id);
    CHECK(cache.texture_nearest(id));
    CHECK_FALSE(cache.texture_nearest(Uuid::generate()));
    scene::Scene s;
    const auto e = s.create("Pixel");
    s.world().add<scene::SpriteComponent>(e).texture_asset = id;
    s.world().get<scene::SpriteComponent>(e)->size = {64, 16};
    s.world().get<scene::Transform>(e)->position = {32, 8, 0};
    s.update_transforms();
    sb.begin(math::Mat4::orthographic(0, 64, 0, 16, -1, 1));
    CHECK(runtime::extract_sprites(s, cache, sb) == 1);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    const auto px = dev.read_texture(target).value();
    test::save_output("sprite_nearest_asset", W, H, px);
    CHECK(channel(test::pixel(px, W, 30, 8), 1) == 0); // sRGB-imported red stays pure red: no blending
    CHECK(channel(test::pixel(px, W, 30, 8), 0) > 200);
    dev.destroy(target);
    std::error_code ec;
    sfs::remove_all(project, ec);
}

TEST_CASE("2d camera: pixel snap rounds to whole screen pixels and round-trips") {
    scene::Scene s;
    const auto cam = s.create("Camera");
    auto& c = s.world().add<scene::CameraComponent>(cam);
    c.projection = scene::CameraComponent::Projection::Orthographic;
    c.ortho_height = 10.0f; // 72 px per unit at 720 px
    s.world().get<scene::Transform>(cam)->position = {1.2345f, -0.3333f, 10};
    s.update_transforms();
    auto loose = runtime::scene_camera_2d(s, 720.0f);
    REQUIRE(loose);
    CHECK(loose->position.x == doctest::Approx(1.2345f));
    s.world().get<scene::CameraComponent>(cam)->pixel_snap = true;
    auto snapped = runtime::scene_camera_2d(s, 720.0f);
    REQUIRE(snapped);
    const float px = snapped->position.x * snapped->pixels_per_unit, py = snapped->position.y * snapped->pixels_per_unit;
    CHECK(px == doctest::Approx(std::round(px)).epsilon(1e-5));
    CHECK(py == doctest::Approx(std::round(py)).epsilon(1e-5));
    CHECK(std::abs(snapped->position.x - 1.2345f) <= 0.5f / 72.0f + 1e-5f);

    const auto doc = s.save();
    scene::Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* c2 = t.world().get<scene::CameraComponent>(t.find_by_name("Camera"));
    REQUIRE(c2);
    CHECK(c2->pixel_snap);
}
