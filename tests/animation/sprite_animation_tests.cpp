// SPDX-License-Identifier: Apache-2.0
// Sprite sheets (.vsheet) and SpriteAnimator playback (Slice 39).
#include "victus/animation/animation.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <filesystem>

using namespace victus;
using namespace victus::animation;
using scene::Entity;
using scene::Scene;
using scene::SpriteComponent;

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }

Result<SpriteSheet> parse_text(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE(doc.ok());
    return parse_sprite_sheet(*doc);
}

std::shared_ptr<SpriteSheet> walk_sheet(const Uuid& texture) {
    auto s = std::make_shared<SpriteSheet>();
    s->texture = texture;
    s->columns = 4;
    s->rows = 2;
    s->clips.push_back({.name = "walk", .frames = {0, 1, 2, 3}, .fps = 4, .loop = true});
    s->clips.push_back({.name = "die", .frames = {4, 5, 6}, .fps = 10, .loop = false});
    return s;
}

} // namespace

TEST_CASE("sprite sheet: parse, ranges and round trip") {
    const Uuid tex = Uuid::generate();
    auto sheet = parse_text(R"({"format":"victus.spritesheet","version":1,"texture":")" + tex.to_string() +
                            R"(","columns":4,"rows":2,"clips":{"idle":{"frames":[0,1,2,1],"fps":6},
                               "run":{"from":4,"to":7,"fps":12},"back":{"from":3,"to":1,"loop":false}}})");
    REQUIRE(sheet.ok());
    CHECK(sheet->texture == tex);
    CHECK(sheet->frame_count() == 8);
    REQUIRE(sheet->clips.size() == 3);
    const SpriteClip* idle = sheet->find("idle");
    REQUIRE(idle);
    CHECK(idle->frames == std::vector<std::uint32_t>{0, 1, 2, 1});
    CHECK(near(idle->fps, 6));
    CHECK(idle->loop);
    const SpriteClip* run = sheet->find("run");
    REQUIRE(run);
    CHECK(run->frames == std::vector<std::uint32_t>{4, 5, 6, 7});
    const SpriteClip* back = sheet->find("back");
    REQUIRE(back);
    CHECK(back->frames == std::vector<std::uint32_t>{3, 2, 1});
    CHECK_FALSE(back->loop);
    CHECK(sheet->find("missing") == nullptr);

    auto again = parse_sprite_sheet(to_json(*sheet));
    REQUIRE(again.ok());
    CHECK(again->columns == 4);
    CHECK(again->rows == 2);
    CHECK(again->texture == tex);
    REQUIRE(again->find("run"));
    CHECK(again->find("run")->frames == run->frames);
    CHECK(again->find("back")->loop == false);

    const auto path = std::filesystem::temp_directory_path() / ("victus_sheet_" + Uuid::generate().to_string() + ".vsheet");
    REQUIRE(save_sprite_sheet_file(path, *sheet).ok());
    auto loaded = load_sprite_sheet_file(path);
    std::filesystem::remove(path);
    REQUIRE(loaded.ok());
    CHECK(loaded->find("idle")->frames == idle->frames);
}

TEST_CASE("sprite sheet: validation errors name the problem") {
    const auto bad = [](std::string_view text, std::string_view needle) {
        auto r = parse_text(text);
        REQUIRE_FALSE(r.ok());
        const std::string msg = r.error().message;
        INFO(msg);
        CHECK(msg.find(needle) != std::string::npos);
    };
    bad(R"({"format":"other","version":1,"columns":1,"rows":1})", "victus.spritesheet");
    bad(R"({"format":"victus.spritesheet","version":9,"columns":1,"rows":1})", "version");
    bad(R"({"format":"victus.spritesheet","version":1,"columns":0,"rows":1})", "columns");
    bad(R"({"format":"victus.spritesheet","version":1,"columns":1000,"rows":1000})", "frames");
    bad(R"({"format":"victus.spritesheet","version":1,"columns":2,"rows":1,"clips":{"a":{"frames":[0,2]}}})", "a");
    bad(R"({"format":"victus.spritesheet","version":1,"columns":2,"rows":1,"clips":{"a":{"frames":[]}}})", "a");
    bad(R"({"format":"victus.spritesheet","version":1,"columns":2,"rows":1,"clips":{"a":{"frames":[0],"fps":0}}})", "fps");
}

TEST_CASE("sprite sheet: frame uv and clip timing") {
    SpriteSheet s;
    s.columns = 4;
    s.rows = 2;
    const math::Vec4 f0 = s.frame_uv(0);
    CHECK(near(f0.x, 0));
    CHECK(near(f0.y, 0));
    CHECK(near(f0.z, 0.25f));
    CHECK(near(f0.w, 0.5f));
    const math::Vec4 f6 = s.frame_uv(6); // row 1, column 2
    CHECK(near(f6.x, 0.5f));
    CHECK(near(f6.y, 0.5f));
    CHECK(near(f6.z, 0.75f));
    CHECK(near(f6.w, 1.0f));
    const math::Vec4 clamped = s.frame_uv(99); // past the grid: last frame
    CHECK(near(clamped.x, 0.75f));
    CHECK(near(clamped.w, 1.0f));

    const SpriteClip loop{.name = "l", .frames = {0, 1, 2, 3}, .fps = 4, .loop = true};
    CHECK(near(loop.duration(), 1.0f));
    CHECK(clip_frame_at(loop, 0.0f) == 0);
    CHECK(clip_frame_at(loop, 0.26f) == 1);
    CHECK(clip_frame_at(loop, 0.99f) == 3);
    CHECK(clip_frame_at(loop, 1.01f) == 0); // wraps
    CHECK(clip_frame_at(loop, 2.6f) == 2);
    CHECK(clip_frame_at(loop, -0.1f) == 3); // negative counts from the end
    const SpriteClip once{.name = "o", .frames = {0, 1, 2}, .fps = 10, .loop = false};
    CHECK(clip_frame_at(once, 0.15f) == 1);
    CHECK(clip_frame_at(once, 5.0f) == 2); // clamps on the last frame
    CHECK(clip_frame_at(once, -1.0f) == 0);
}

TEST_CASE("sprite animator: drives Sprite texture and uv") {
    const Uuid tex = Uuid::generate(), sheet_id = Uuid::generate();
    AnimationSystem anim;
    anim.add_sheet(sheet_id, walk_sheet(tex));
    Scene s;
    const Entity hero = s.create("Hero");
    s.world().add<SpriteComponent>(hero);
    auto& a = s.world().add<SpriteAnimatorComponent>(hero);
    a.sheet = sheet_id;
    a.clip = "walk";

    anim.update_sprites(s, 0); // preview: first frame, texture copied from the sheet
    const auto* sp = s.world().get<SpriteComponent>(hero);
    CHECK(sp->texture_asset == tex);
    CHECK(near(sp->uv.z, 0.25f));
    anim.update(s, 0.3f); // 4 fps: second frame
    CHECK(near(sp->uv.x, 0.25f));
    auto st = anim.sprite_status(s, hero);
    CHECK(st.valid);
    CHECK(st.clip == "walk");
    CHECK(st.frame == 1);
    CHECK(st.clip_frame == 1);
    CHECK(st.playing);
    CHECK(st.looping);
    CHECK(near(st.duration, 1.0f));
    CHECK_FALSE(st.finished);

    CHECK(anim.sprite_clips(s, hero) == std::vector<std::string>{"walk", "die"});
    CHECK_FALSE(anim.play_sprite(s, hero, "fly"));
    REQUIRE(anim.play_sprite(s, hero, "die"));
    st = anim.sprite_status(s, hero);
    CHECK(st.clip == "die");
    CHECK(near(st.time, 0));
    CHECK(st.frame == 4);
    anim.update(s, 1.0f); // non-looping: holds the last frame and reports finished
    st = anim.sprite_status(s, hero);
    CHECK(st.frame == 6);
    CHECK(st.finished);
    CHECK(near(sp->uv.x, 0.5f));
    CHECK(near(sp->uv.y, 0.5f));

    // Changing the clip field directly also restarts it.
    s.world().get<SpriteAnimatorComponent>(hero)->clip = "walk";
    anim.update(s, 0.0f);
    CHECK(anim.sprite_status(s, hero).frame == 0);

    // No clip: a static atlas cell.
    s.world().get<SpriteAnimatorComponent>(hero)->clip.clear();
    s.world().get<SpriteAnimatorComponent>(hero)->frame = 7;
    anim.update(s, 0.5f);
    CHECK(near(sp->uv.x, 0.75f));
    CHECK(near(sp->uv.y, 0.5f));
    CHECK(anim.sprite_status(s, hero).frame == 7);

    // Paused animators do not advance.
    auto* c = s.world().get<SpriteAnimatorComponent>(hero);
    c->clip = "walk";
    anim.update(s, 0);
    c->playing = false;
    anim.update(s, 0.6f);
    CHECK(anim.sprite_status(s, hero).frame == 0);

    // Missing sheets leave the Sprite alone and report invalid.
    const Entity ghost = s.create("Ghost");
    s.world().add<SpriteComponent>(ghost);
    s.world().add<SpriteAnimatorComponent>(ghost).sheet = Uuid::generate();
    anim.update(s, 0.1f);
    CHECK_FALSE(anim.sprite_status(s, ghost).valid);
    CHECK(s.world().get<SpriteComponent>(ghost)->texture_asset.is_nil());
    CHECK_FALSE(anim.play_sprite(s, s.create("Plain"), "walk"));
}

TEST_CASE("sprite animator: provider, cache clearing and scene round trip") {
    const Uuid tex = Uuid::generate(), sheet_id = Uuid::generate();
    int asks = 0;
    AnimationSystem anim(AnimationSettings{.sheets = [&](const Uuid& id) -> std::shared_ptr<const SpriteSheet> {
        ++asks;
        return id == sheet_id ? walk_sheet(tex) : nullptr;
    }});
    CHECK(anim.sheet(sheet_id) != nullptr);
    CHECK(anim.sheet(sheet_id) != nullptr);
    CHECK(asks == 1);
    anim.clear_sheets();
    CHECK(anim.sheet(sheet_id) != nullptr);
    CHECK(asks == 2);

    register_components();
    Scene s;
    const Entity e = s.create("Coin");
    auto& sp = s.world().add<SpriteComponent>(e);
    sp.uv = {0.25f, 0.5f, 0.5f, 1.0f};
    auto& a = s.world().add<SpriteAnimatorComponent>(e);
    a.sheet = sheet_id;
    a.clip = "walk";
    a.speed = 2.0f;
    auto doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const Entity e2 = t.find_by_name("Coin");
    REQUIRE_FALSE(e2.is_null());
    const auto* sp2 = t.world().get<SpriteComponent>(e2);
    REQUIRE(sp2);
    CHECK(near(sp2->uv.x, 0.25f));
    CHECK(near(sp2->uv.w, 1.0f));
    const auto* a2 = t.world().get<SpriteAnimatorComponent>(e2);
    REQUIRE(a2);
    CHECK(a2->sheet == sheet_id);
    CHECK(a2->clip == "walk");
    CHECK(near(a2->speed, 2.0f));
}
