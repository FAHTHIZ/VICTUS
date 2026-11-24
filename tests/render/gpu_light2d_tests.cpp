// SPDX-License-Identifier: Apache-2.0
// 2D lights (Slice 78, ADR-0065): point, spot and global (ambient) lights accumulate into a light
// texture that multiplies lit sprites; unlit and additive sprites are untouched, offscreen lights
// are culled, and Light2DComponent drives it all from a scene through SceneView. Normal-mapped
// sprites and 2D shadow casters (ADR-0255) shade and block the lights.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/scene/scene.hpp"

#include <cmath>
#include <functional>

using namespace victus;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 64, H = 64;

struct Frame {
    std::vector<std::uint8_t> px;
    render::SpriteBatchStats stats;
};

// World [-1, 1]^2 maps onto the 64x64 target; `content` adds sprites/lights after begin().
Frame draw(const std::function<void(render::SpriteBatch&)>& content, const char* name) {
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto batch = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    render::SpriteBatch& sb = *batch.value();
    sb.begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    content(sb);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    REQUIRE(sb.render_lights(cmd, W, H).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    Frame f;
    f.px = dev.read_texture(target).value();
    f.stats = sb.stats();
    test::save_output(name, W, H, f.px);
    dev.destroy(target);
    return f;
}

render::Sprite full_white(bool lit) {
    render::Sprite sp;
    sp.size = {2, 2};
    sp.lit = lit;
    return sp;
}

// A 4x4 tangent-space normal map whose every texel encodes `n` (unit vector).
rhi::TextureHandle flat_normal_map(math::Vec3 n) {
    auto& dev = gpu().ctx->device();
    rhi::TextureDesc td;
    td.name = "test_normal_map";
    td.width = td.height = 4;
    auto t = dev.create_texture(td);
    REQUIRE(t.ok());
    const auto enc = [](float v) { return static_cast<std::uint8_t>(std::lround((v * 0.5f + 0.5f) * 255.0f)); };
    std::vector<std::uint8_t> px;
    for (int i = 0; i < 16; ++i) px.insert(px.end(), {enc(n.x), enc(n.y), enc(n.z), 255});
    auto cmd = dev.begin_commands();
    cmd.upload_texture(t.value(), px);
    cmd.submit_and_wait();
    return t.value();
}

int brightest(const std::vector<std::uint8_t>& px, int x, int y_a, int y_b, int c) {
    return std::max(channel(test::pixel(px, W, x, y_a), c), channel(test::pixel(px, W, x, y_b), c));
}

} // namespace

TEST_CASE("gpu: 2d light - without lighting lit sprites draw unchanged") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const Frame f = draw([](render::SpriteBatch& b) { b.draw(full_white(true)); }, "light2d_off");
    CHECK_FALSE(f.stats.lighting);
    CHECK(f.stats.lights == 0);
    CHECK(channel(test::pixel(f.px, W, 32, 32), 0) > 250);
    CHECK(channel(test::pixel(f.px, W, 5, 5), 1) > 250);
}

TEST_CASE("gpu: 2d light - ambient darkens lit sprites only") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const Frame f = draw(
        [](render::SpriteBatch& b) {
            b.set_lighting({0.25f, 0.25f, 0.25f, 1});
            render::Sprite left = full_white(true);
            left.size = {1, 2};
            left.position = {-0.5f, 0};
            b.draw(left);
            render::Sprite right = full_white(false);
            right.size = {1, 2};
            right.position = {0.5f, 0};
            b.draw(right);
        },
        "light2d_ambient");
    CHECK(f.stats.lighting);
    CHECK(f.stats.lit_sprites == 1);
    const auto lit = test::pixel(f.px, W, 16, 32);
    CHECK(channel(lit, 0) > 55);
    CHECK(channel(lit, 0) < 75);
    CHECK(channel(test::pixel(f.px, W, 48, 32), 0) > 250);
}

TEST_CASE("gpu: 2d light - point light falls off to zero at its radius") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const Frame f = draw(
        [](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.color = {1, 0, 0, 1};
            l.radius = 1.0f;
            b.add_light(l);
            b.draw(full_white(true));
        },
        "light2d_point");
    CHECK(f.stats.lights == 1);
    const auto centre = test::pixel(f.px, W, 32, 32);
    CHECK(channel(centre, 0) > 240);
    CHECK(channel(centre, 1) < 5); // red light tints the white sprite red
    const int mid = channel(test::pixel(f.px, W, 48, 32), 0); // d ~ 0.5 -> (1 - 0.25)^2 ~ 0.56
    CHECK(mid > 120);
    CHECK(mid < 165);
    CHECK(channel(test::pixel(f.px, W, 2, 2), 0) < 3); // beyond the radius
    CHECK(channel(test::pixel(f.px, W, 63, 32), 0) < 8);

    // A tighter falloff exponent darkens the mid range.
    const Frame t = draw(
        [](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.radius = 1.0f;
            l.falloff = 3.0f;
            b.add_light(l);
            b.draw(full_white(true));
        },
        "light2d_point_tight");
    CHECK(channel(test::pixel(t.px, W, 48, 32), 0) < mid - 40);
}

TEST_CASE("gpu: 2d light - spot cone, additive and unlit sprites, culling") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const Frame f = draw(
        [](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.type = render::Light2D::Type::Spot;
            l.direction = {1, 0};
            l.radius = 2.0f;
            l.inner_angle = 0.3f;
            l.outer_angle = 0.5f;
            b.add_light(l);
            render::Light2D far; // entirely offscreen: culled
            far.position = {10, 10};
            b.add_light(far);
            b.draw(full_white(true));
        },
        "light2d_spot");
    CHECK(f.stats.lights == 1);
    CHECK(channel(test::pixel(f.px, W, 48, 32), 0) > 100); // along the axis
    CHECK(channel(test::pixel(f.px, W, 16, 32), 0) < 3);   // behind
    CHECK(channel(test::pixel(f.px, W, 32, 56), 0) < 3);   // outside the cone
    CHECK(channel(test::pixel(f.px, W, 32, 8), 0) < 3);

    // Additive sprites stay emissive even when flagged lit.
    const Frame a = draw(
        [](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Sprite sp = full_white(true);
            sp.additive = true;
            sp.color = {0, 0.5f, 0, 1};
            b.draw(sp);
        },
        "light2d_additive");
    CHECK(a.stats.lit_sprites == 0);
    CHECK(channel(test::pixel(a.px, W, 10, 10), 1) > 115);
}

TEST_CASE("gpu: 2d light - Light2DComponent lights a scene through SceneView") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    gameplay::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam).projection = scene::CameraComponent::Projection::Orthographic;
    const scene::Entity spr = s.create("Floor");
    s.world().add<scene::SpriteComponent>(spr).size = {10, 10};
    const scene::Entity amb = s.create("Ambient");
    {
        auto& l = s.world().add<gameplay::Light2DComponent>(amb);
        l.type = gameplay::Light2DComponent::Type::Global;
        l.color = {0.2f, 0.2f, 0.2f, 1};
    }
    const scene::Entity lamp = s.create("Lamp");
    {
        auto& l = s.world().add<gameplay::Light2DComponent>(lamp);
        l.radius = 2.0f;
        l.color = {0, 0, 1, 1};
    }
    s.world().get<scene::Transform>(lamp)->position = {2.5f, 0, 0};

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto view = runtime::SceneView::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(view.ok());
    auto target = test::make_target(W, H);
    runtime::SceneViewOptions o;
    o.clear_2d = {0, 0, 0, 1};
    auto frame = [&](const char* name) {
        s.update_transforms();
        auto cmd = dev.begin_commands();
        auto st = (*view)->render(cmd, s, assets, target, W, H, o);
        REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        test::save_output(name, W, H, px);
        return std::make_pair(*st, px);
    };

    {
        const auto [st, px] = frame("light2d_scene");
        CHECK(st.mode == runtime::ViewMode::TwoD);
        CHECK(st.lighting_2d);
        CHECK(st.lights_2d == 1);
        const auto dim = test::pixel(px, W, 8, 32); // ambient only (x ~ -3.75)
        CHECK(channel(dim, 0) > 40);
        CHECK(channel(dim, 0) < 62);
        const auto lit = test::pixel(px, W, 48, 32); // under the lamp (x = 2.5)
        CHECK(channel(lit, 2) > 240);
        CHECK(channel(lit, 0) < 62);
    }

    // Unlit sprites ignore the lights.
    s.world().get<scene::SpriteComponent>(spr)->unlit = true;
    {
        const auto [st, px] = frame("light2d_scene_unlit");
        CHECK(channel(test::pixel(px, W, 8, 32), 0) > 250);
    }
    s.world().get<scene::SpriteComponent>(spr)->unlit = false;

    // Disabled lights turn 2D lighting off entirely.
    s.world().get<gameplay::Light2DComponent>(amb)->enabled = false;
    s.world().get<gameplay::Light2DComponent>(lamp)->enabled = false;
    {
        const auto [st, px] = frame("light2d_scene_disabled");
        CHECK_FALSE(st.lighting_2d);
        CHECK(channel(test::pixel(px, W, 8, 32), 0) > 250);
    }
    dev.destroy(target);

    // Serialization round trip.
    s.world().get<gameplay::Light2DComponent>(lamp)->type = gameplay::Light2DComponent::Type::Spot;
    s.world().get<gameplay::Light2DComponent>(lamp)->outer_angle_degrees = 50.0f;
    s.world().get<scene::SpriteComponent>(spr)->unlit = true;
    const json::Value doc = s.save();
    scene::Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    const auto* ll = loaded.world().get<gameplay::Light2DComponent>(loaded.find_by_name("Lamp"));
    REQUIRE(ll);
    CHECK(ll->type == gameplay::Light2DComponent::Type::Spot);
    CHECK(ll->outer_angle_degrees == doctest::Approx(50.0f));
    CHECK(ll->radius == doctest::Approx(2.0f));
    CHECK_FALSE(ll->enabled);
    const auto* ls = loaded.world().get<scene::SpriteComponent>(loaded.find_by_name("Floor"));
    REQUIRE(ls);
    CHECK(ls->unlit);
}

TEST_CASE("gpu: 2d light - normal-mapped sprites shade by N.L") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const rhi::TextureHandle right = flat_normal_map({1, 0, 0}); // every texel faces +X
    const auto scene = [&](bool mapped, bool flip) {
        return [=](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.radius = 2.0f;
            l.height = 0.25f;
            b.add_light(l);
            render::Sprite sp = full_white(true);
            if (mapped) sp.normal_map = right;
            sp.flip_x = flip;
            b.draw(sp);
        };
    };
    // Flat: left and right of the light match (pixels 15 and 48 are 16.5 px either side).
    const Frame flat = draw(scene(false, false), "light2d_normals_flat");
    CHECK(flat.stats.normal_mapped_sprites == 0);
    const int fl = channel(test::pixel(flat.px, W, 15, 32), 0), fr = channel(test::pixel(flat.px, W, 48, 32), 0);
    CHECK(std::abs(fl - fr) < 4);
    CHECK(fl > 150);
    // Normals facing +X: lit where the light is to their right (x < 0), dark beyond it.
    const Frame m = draw(scene(true, false), "light2d_normals_mapped");
    CHECK(m.stats.normal_mapped_sprites == 1);
    CHECK(channel(test::pixel(m.px, W, 16, 32), 0) > 150);
    CHECK(channel(test::pixel(m.px, W, 48, 32), 0) < 5);
    // flip_x mirrors the tangent frame along with the texels.
    const Frame f = draw(scene(true, true), "light2d_normals_flipped");
    CHECK(channel(test::pixel(f.px, W, 16, 32), 0) < 5);
    CHECK(channel(test::pixel(f.px, W, 48, 32), 0) > 150);
    gpu().ctx->device().destroy(right);
}

TEST_CASE("gpu: 2d light - shadow casters block shadowed lights only") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const math::Vec2 box[4] = {{-0.1f, -0.1f}, {0.1f, -0.1f}, {0.1f, 0.1f}, {-0.1f, 0.1f}};
    struct Opt {
        bool shadows{true}, self_shadow{false};
        float strength{1.0f};
    };
    const auto scene = [&](Opt o) {
        return [=](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.position = {-0.75f, 0};
            l.radius = 3.0f;
            l.shadows = o.shadows;
            l.shadow_strength = o.strength;
            b.add_light(l);
            render::Light2D small; // unshadowed, away from the shadow
            small.position = {0.8f, 0.8f};
            small.radius = 0.5f;
            small.color = {0, 1, 0, 1};
            b.add_light(small);
            b.add_shadow_caster(box, o.self_shadow);
            b.draw(full_white(true));
        };
    };
    const Frame open = draw(scene({.shadows = false}), "light2d_shadow_off");
    CHECK(open.stats.shadowed_lights == 0);
    CHECK(open.stats.shadow_casters == 1);
    const int unshadowed = channel(test::pixel(open.px, W, 51, 32), 0);
    CHECK(unshadowed > 100);

    const Frame f = draw(scene({}), "light2d_shadow");
    CHECK(f.stats.lights == 2);
    CHECK(f.stats.shadowed_lights == 1);
    CHECK(channel(test::pixel(f.px, W, 51, 32), 0) < 8);   // behind the caster
    CHECK(channel(test::pixel(f.px, W, 51, 6), 0) > 60);   // outside the shadow wedge
    CHECK(channel(test::pixel(f.px, W, 51, 58), 0) > 60);
    CHECK(channel(test::pixel(f.px, W, 10, 32), 0) > 150); // between the light and the caster
    CHECK(channel(test::pixel(f.px, W, 32, 32), 0) > 100); // the caster's own area stays lit
    CHECK(brightest(f.px, 56, 8, 56, 1) > 100);            // the unshadowed light is unaffected

    const Frame self = draw(scene({.self_shadow = true}), "light2d_shadow_self");
    CHECK(channel(test::pixel(self.px, W, 32, 32), 0) < 8);

    const Frame half = draw(scene({.strength = 0.5f}), "light2d_shadow_half");
    const int h = channel(test::pixel(half.px, W, 51, 32), 0);
    CHECK(h > unshadowed / 2 - 15);
    CHECK(h < unshadowed / 2 + 15);

    // Concave casters are triangulated for the lit interior; invalid outlines are ignored.
    const math::Vec2 ell[6] = {{-0.2f, -0.2f}, {0.2f, -0.2f}, {0.2f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.2f}, {-0.2f, 0.2f}};
    const math::Vec2 line[3] = {{0, 0}, {0.5f, 0}, {1, 0}};
    const Frame c = draw(
        [&](render::SpriteBatch& b) {
            b.set_lighting({0, 0, 0, 1});
            render::Light2D l;
            l.position = {-0.75f, 0};
            l.radius = 3.0f;
            l.shadows = true;
            b.add_light(l);
            b.add_shadow_caster(ell);
            b.add_shadow_caster(line); // zero area
            b.draw(full_white(true));
        },
        "light2d_shadow_concave");
    CHECK(c.stats.shadow_casters == 1);
    CHECK(channel(test::pixel(c.px, W, 29, 32), 0) > 100); // inside the L
    CHECK(channel(test::pixel(c.px, W, 56, 32), 0) < 8);   // behind it
}

TEST_CASE("gpu: 2d light - ShadowCaster2D and sprite normal maps through SceneView") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    gameplay::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam).projection = scene::CameraComponent::Projection::Orthographic;
    const scene::Entity floor = s.create("Floor");
    s.world().add<scene::SpriteComponent>(floor).size = {10, 10};
    const scene::Entity amb = s.create("Ambient");
    {
        auto& l = s.world().add<gameplay::Light2DComponent>(amb);
        l.type = gameplay::Light2DComponent::Type::Global;
        l.color = {0.2f, 0.2f, 0.2f, 1};
    }
    const scene::Entity lamp = s.create("Lamp");
    {
        auto& l = s.world().add<gameplay::Light2DComponent>(lamp);
        l.radius = 3.0f;
        l.color = {0, 0, 1, 1};
        l.shadows = true;
        l.height = 2.0f;
    }
    s.world().get<scene::Transform>(lamp)->position = {2.5f, 0, 0};
    const scene::Entity wall = s.create("Wall");
    {
        auto& sp = s.world().add<scene::SpriteComponent>(wall);
        sp.layer = 1;
        s.world().add<gameplay::ShadowCaster2DComponent>(wall); // empty outline: the sprite rectangle
    }
    s.world().get<scene::Transform>(wall)->position = {1.5f, 0, 0};

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto view = runtime::SceneView::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(view.ok());
    auto target = test::make_target(W, H);
    runtime::SceneViewOptions o;
    o.clear_2d = {0, 0, 0, 1};
    auto frame = [&](const char* name) {
        s.update_transforms();
        auto cmd = dev.begin_commands();
        auto st = (*view)->render(cmd, s, assets, target, W, H, o);
        REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        test::save_output(name, W, H, px);
        return std::make_pair(*st, px);
    };
    int shadowed = 0;
    {
        const auto [st, px] = frame("light2d_scene_shadow");
        CHECK(st.lights_2d == 1);
        CHECK(st.shadowed_lights_2d == 1);
        shadowed = channel(test::pixel(px, W, 35, 32), 2); // x ~ 0.5: behind the wall from the lamp
        CHECK(shadowed < 70);                                // ambient only
        CHECK(channel(test::pixel(px, W, 60, 32), 2) > 110); // x ~ 4.4: open floor, ambient + lamp
    }
    s.world().get<gameplay::ShadowCaster2DComponent>(wall)->enabled = false;
    {
        const auto [st, px] = frame("light2d_scene_shadow_off");
        CHECK(st.shadowed_lights_2d == 0); // no casters: drawn as an ordinary light
        CHECK(channel(test::pixel(px, W, 35, 32), 2) > shadowed + 40);
    }
    dev.destroy(target);

    // Serialization round trip of the new fields.
    auto* caster = s.world().get<gameplay::ShadowCaster2DComponent>(wall);
    caster->enabled = true;
    caster->self_shadow = true;
    caster->outline = {{0, 0}, {1, 0}, {0, 1}};
    const Uuid nmap = Uuid::generate();
    s.world().get<scene::SpriteComponent>(wall)->normal_map_asset = nmap;
    const json::Value doc = s.save();
    scene::Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    const scene::Entity lw = loaded.find_by_name("Wall");
    const auto* lc = loaded.world().get<gameplay::ShadowCaster2DComponent>(lw);
    REQUIRE(lc);
    CHECK(lc->self_shadow);
    REQUIRE(lc->outline.size() == 3);
    CHECK(lc->outline[2].y == doctest::Approx(1.0f));
    CHECK(loaded.world().get<scene::SpriteComponent>(lw)->normal_map_asset == nmap);
    CHECK(loaded.world().get<scene::SpriteComponent>(loaded.find_by_name("Floor"))->normal_map_asset.is_nil());
    const auto* ll = loaded.world().get<gameplay::Light2DComponent>(loaded.find_by_name("Lamp"));
    REQUIRE(ll);
    CHECK(ll->shadows);
    CHECK(ll->height == doctest::Approx(2.0f));
    CHECK(ll->shadow_strength == doctest::Approx(1.0f));
}

TEST_CASE("gpu: sprite batch - radix-sorted order keeps layers, then submission order") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    // 600 overlapping full-target sprites over layers -1, 0 and 1: the visible one is the last
    // submitted sprite of layer 1 (the sort keys carry the submission index, ADR-0255).
    constexpr int kCount = 600;
    const auto layer_of = [](int i) { return (i * 7) % 3 - 1; };
    int expected = -1;
    for (int i = 0; i < kCount; ++i)
        if (layer_of(i) == 1) expected = i;
    REQUIRE(expected >= 0);
    const Frame f = draw(
        [&](render::SpriteBatch& b) {
            for (int i = 0; i < kCount; ++i) {
                render::Sprite sp = full_white(false);
                sp.layer = layer_of(i);
                sp.color = {static_cast<float>(i & 255) / 255.0f, static_cast<float>(i >> 8) / 4.0f, 0, 1};
                b.draw(sp);
            }
        },
        "sprite_radix_order");
    CHECK(f.stats.batches == 1); // same texture and state: one sorted draw
    const auto px = test::pixel(f.px, W, 32, 32);
    CHECK(std::abs(channel(px, 0) - (expected & 255)) <= 1);
    CHECK(std::abs(channel(px, 1) - static_cast<int>(std::lround((expected >> 8) / 4.0 * 255.0))) <= 1);
}
