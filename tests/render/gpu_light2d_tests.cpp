// SPDX-License-Identifier: Apache-2.0
// 2D lights (Slice 78, ADR-0065): point, spot and global (ambient) lights accumulate into a light
// texture that multiplies lit sprites; unlit and additive sprites are untouched, offscreen lights
// are culled, and Light2DComponent drives it all from a scene through SceneView.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/scene/scene.hpp"

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
