// SPDX-License-Identifier: Apache-2.0
// Sprite sheets end to end: a .vsheet asset animates a Sprite, the batch draws the current cell,
// and editing the sheet hot-reloads its clips (Slice 39).
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/render/image.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"

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
