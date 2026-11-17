// SPDX-License-Identifier: Apache-2.0
// Projected decals (Slice 77, ADR-0064): multiply/alpha/additive blending onto opaque surfaces
// reconstructed from scene depth, box clipping, angle fading, the camera inside a decal box,
// frustum culling, sort order, blended geometry drawn over decals, and the Decal component
// through runtime extraction.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/scene/scene.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return test::pixel(px, W, x, y); }
    [[nodiscard]] std::uint32_t center() const { return at(W / 2, H / 2); }
};

RenderSettings flat_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::Medium);
    s.bloom = false; // keep pixels local so colour checks are exact
    s.fxaa = false;
    s.taa = false;
    s.ssao = false;
    return s;
}

Environment flat_env() {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {0.3f, 0.3f, 0.3f, 1};
    env.ambient_intensity = 1.0f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    return env;
}

template <typename F>
Shot shoot(SceneRenderer& r, F&& content, const char* save = nullptr,
           Camera3D cam = Camera3D::look_at({0, 0, 5}, {0, 0, 0})) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    r.set_settings(flat_settings());
    DirectionalLight sun;
    sun.direction = {0, 0, -1};
    sun.intensity = 1.5f;
    sun.shadows = false;
    r.begin(cam, sun, flat_env());
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

// A light grey wall whose front face lies on z = 0, facing the camera at z = 5.
struct Wall {
    GpuMesh cube;
    Material grey;
    math::Mat4 model = math::Mat4::trs({0, 0, -0.5f}, {}, {12, 12, 1});
    explicit Wall(GpuContext& ctx) : cube(ctx.create_mesh(make_cube(), "decal_wall").value()) {
        grey.base_color = {0.8f, 0.8f, 0.8f, 1};
    }
    void submit(SceneRenderer& r) const { r.submit(cube, grey, model); }
};

Decal box_decal(math::Vec3 center, math::Vec3 size, math::Color color, DecalBlend blend, math::Quat rot = {}) {
    Decal d;
    d.transform = math::Mat4::trs(center, rot, size);
    d.color = color;
    d.blend = blend;
    d.depth_fade = 0.0f;
    return d;
}

int diff(std::uint32_t a, std::uint32_t b, int c) {
    return static_cast<int>(channel(a, c)) - static_cast<int>(channel(b, c));
}

} // namespace

TEST_CASE("decal blend names round trip") {
    for (const DecalBlend b : {DecalBlend::Multiply, DecalBlend::Alpha, DecalBlend::Additive})
        CHECK(parse_decal_blend(decal_blend_name(b)) == b);
    CHECK_FALSE(parse_decal_blend("glow").has_value());
}

TEST_CASE("gpu decals: multiply, alpha and additive decals change only the surface inside the box") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Wall wall(ctx);
    const Shot base = shoot(r, [&](SceneRenderer& rr) { wall.submit(rr); });
    CHECK(base.stats.decals == 0);
    const int grey = channel(base.center(), 0);
    REQUIRE(grey > 60);
    const std::uint32_t outside_x = W / 2 + 60;

    // Multiply by (1, 0.25, 0.25): red stays, green and blue drop to a quarter in linear light
    // (about 0.53 of the sRGB-encoded value).
    const Shot mul = shoot(
        r,
        [&](SceneRenderer& rr) {
            wall.submit(rr);
            rr.add_decal(box_decal({0, 0, 0}, {2, 2, 1}, {1, 0.25f, 0.25f, 1}, DecalBlend::Multiply));
        },
        "decal_multiply");
    CHECK(mul.stats.decals == 1);
    CHECK(std::abs(diff(mul.center(), base.center(), 0)) <= 3);
    CHECK(channel(mul.center(), 1) < grey * 0.6f);
    CHECK(channel(mul.center(), 2) < grey * 0.6f);
    CHECK(mul.at(outside_x, H / 2) == base.at(outside_x, H / 2)); // outside the box: untouched

    // Half-opaque multiply: halfway between the surface and the full decal.
    const Shot half = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(box_decal({0, 0, 0}, {2, 2, 1}, {1, 0.25f, 0.25f, 0.5f}, DecalBlend::Multiply));
    });
    CHECK(channel(half.center(), 1) > channel(mul.center(), 1) + 10);
    CHECK(channel(half.center(), 1) < grey - 10);

    // Alpha paints unlit colour.
    const Shot alpha = shoot(
        r,
        [&](SceneRenderer& rr) {
            wall.submit(rr);
            rr.add_decal(box_decal({0, 0, 0}, {2, 2, 1}, {0, 0, 1, 1}, DecalBlend::Alpha));
        },
        "decal_alpha");
    CHECK(channel(alpha.center(), 2) > 240);
    CHECK(channel(alpha.center(), 0) < 10);

    // Additive brightens without darkening.
    const Shot add = shoot(
        r,
        [&](SceneRenderer& rr) {
            wall.submit(rr);
            rr.add_decal(box_decal({0, 0, 0}, {2, 2, 1}, {0, 0.3f, 0, 1}, DecalBlend::Additive));
        },
        "decal_additive");
    CHECK(diff(add.center(), base.center(), 1) > 30);
    CHECK(diff(add.center(), base.center(), 0) >= -2);

    // A box floating in front of the wall contains no surface: nothing changes (but it is drawn).
    const Shot floating = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(box_decal({0, 0, 2}, {2, 2, 1}, {0, 0, 1, 1}, DecalBlend::Alpha));
    });
    CHECK(floating.stats.decals == 1);
    CHECK(floating.center() == base.center());

    // No surface at all (background only): nothing changes.
    const Shot empty = shoot(
        r, [&](SceneRenderer& rr) { rr.add_decal(box_decal({0, 0, 0}, {2, 2, 1}, {0, 0, 1, 1}, DecalBlend::Alpha)); });
    CHECK(channel(empty.center(), 2) < 5);
}

TEST_CASE("gpu decals: angle fade, camera inside the box, culling, sort order and blended geometry") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Wall wall(ctx);
    const Shot base = shoot(r, [&](SceneRenderer& rr) { wall.submit(rr); });
    const int grey = channel(base.center(), 1);

    // Projected sideways (axis along X, parallel to the wall): faded by the angle test, applied without it.
    const math::Quat sideways = math::Quat::euler(0, math::radians(90.0f), 0);
    Decal side = box_decal({0, 0, 0}, {2, 2, 2}, {1, 0, 0, 1}, DecalBlend::Multiply, sideways);
    const Shot faded = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(side);
    });
    CHECK(faded.center() == base.center());
    side.angle_fade = 0.0f;
    const Shot unfaded = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(side);
    });
    CHECK(channel(unfaded.center(), 1) < grey / 4);

    // Depth fade: a surface at the very end of the box is faded, one in the middle is not.
    Decal deep = box_decal({0, 0, 0.45f}, {2, 2, 1}, {1, 0, 0, 1}, DecalBlend::Multiply);
    deep.depth_fade = 0.5f;
    const Shot end_faded = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(deep);
    });
    CHECK(channel(end_faded.center(), 1) > grey - 15);

    // Camera inside a huge decal box: back faces still cover the screen, applied exactly once
    // (x0.5 linear is about x0.73 encoded; applied twice it would be about x0.53).
    const Shot inside = shoot(
        r,
        [&](SceneRenderer& rr) {
            wall.submit(rr);
            rr.add_decal(box_decal({0, 0, 0}, {40, 40, 40}, {1, 0.5f, 0.5f, 1}, DecalBlend::Multiply));
        },
        "decal_inside");
    const int g = channel(inside.center(), 1);
    CHECK(g > grey * 0.66f);
    CHECK(g < grey * 0.8f);
    CHECK(channel(inside.at(5, 5), 1) < channel(base.at(5, 5), 1) * 0.85f); // whole visible wall

    // Off-screen boxes are culled.
    const Shot culled = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(box_decal({0, 0, 20}, {1, 1, 1}, {1, 0, 0, 1}, DecalBlend::Alpha));
        rr.add_decal(box_decal({100, 0, 0}, {1, 1, 1}, {1, 0, 0, 1}, DecalBlend::Alpha));
    });
    CHECK(culled.stats.decals == 0);
    CHECK(culled.center() == base.center());

    // Sort order: the higher order draws last, whatever the submission order.
    Decal red = box_decal({0, 0, 0}, {2, 2, 1}, {1, 0, 0, 1}, DecalBlend::Alpha);
    Decal blue = box_decal({0, 0, 0}, {2, 2, 1}, {0, 0, 1, 1}, DecalBlend::Alpha);
    red.sort_order = 5;
    const Shot sorted = shoot(r, [&](SceneRenderer& rr) {
        wall.submit(rr);
        rr.add_decal(red);
        rr.add_decal(blue);
    });
    CHECK(channel(sorted.center(), 0) > 240);
    CHECK(channel(sorted.center(), 2) < 10);

    // Blended geometry is drawn after the decals: an additive green quad in front still shows over it,
    // and the decal under the glass is not lost.
    auto quad = ctx.create_mesh(make_cube(), "decal_glass").value();
    Material glass;
    glass.blend = MaterialBlend::Additive;
    glass.unlit = true;
    glass.base_color = {0, 0.5f, 0, 1};
    const Shot glassy = shoot(
        r,
        [&](SceneRenderer& rr) {
            wall.submit(rr);
            rr.add_decal(blue);
            rr.submit(quad, glass, math::Mat4::trs({0, 0, 1.5f}, {}, {1, 1, 0.1f}));
        },
        "decal_under_glass");
    CHECK(glassy.stats.transparent_draws == 1);
    CHECK(channel(glassy.center(), 2) > 240);
    CHECK(channel(glassy.center(), 1) > 100);
    bool has_decal_pass = false;
    for (const auto& p : r.graph_info().executed_passes)
        has_decal_pass |= p == "decals";
    CHECK(has_decal_pass);
}

TEST_CASE("gpu decals: Decal components render through runtime extraction") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    gameplay::register_components();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const auto cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    s.world().get<scene::Transform>(cam)->position = {0, 0, 5};
    const auto wall = s.create("Wall");
    s.world().get<scene::Transform>(wall)->position = {0, 0, -0.5f};
    s.world().get<scene::Transform>(wall)->scale = {12, 12, 1};
    s.world().add<scene::MeshRendererComponent>(wall).primitive = "cube";
    const auto light = s.create("Sun");
    s.world().add<scene::LightComponent>(light).shadows = false;
    const auto decal = s.create("Stain");
    {
        auto& dc = s.world().add<gameplay::DecalComponent>(decal);
        dc.size = {1, 1, 0.5f};
        dc.color = {1, 0.1f, 0.1f, 1};
    }
    // Component storage may move as other components are added: look it up each time.
    auto comp = [&]() -> gameplay::DecalComponent& { return *s.world().get<gameplay::DecalComponent>(decal); };
    s.world().get<scene::Transform>(decal)->scale = {2, 2, 2}; // 2 x 2 m box

    auto frame = [&](const char* save = nullptr) {
        s.update_transforms();
        runtime::SceneViewSettings settings;
        settings.environment = flat_env();
        const runtime::ExtractStats st = runtime::extract_scene(s, cache, r, settings);
        r.set_settings(flat_settings());
        auto& dev = ctx.device();
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value(), r.stats()};
        if (save) test::save_output(save, W, H, out.px);
        dev.destroy(target);
        return std::pair{st, out};
    };
    const auto [st, shot] = frame("decal_component");
    CHECK(st.decals == 1);
    CHECK(shot.stats.decals == 1);
    const std::uint32_t c = shot.center(), out = shot.at(W / 2 + 60, H / 2);
    CHECK(channel(c, 1) < channel(out, 1) / 2);                          // stained inside the 2 m box ...
    CHECK(channel(shot.at(W / 2 + 12, H / 2), 1) < channel(out, 1) / 2); // 1 m is about 17 px here
    CHECK(std::abs(static_cast<int>(channel(c, 0)) - static_cast<int>(channel(out, 0))) <= 3);

    comp().visible = false;
    const auto [st2, hidden] = frame();
    CHECK(st2.decals == 0);
    CHECK(hidden.stats.decals == 0);
    CHECK(hidden.center() == hidden.at(W / 2 + 1, H / 2 + 1));

    // Serialization round trip through the registered schema.
    comp().visible = true;
    comp().blend = gameplay::DecalComponent::Blend::Additive;
    comp().sort_order = 3;
    const json::Value doc = s.save();
    scene::Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    const auto e = loaded.find_by_name("Stain");
    REQUIRE_FALSE(e.is_null());
    const auto* ld = loaded.world().get<gameplay::DecalComponent>(e);
    REQUIRE(ld != nullptr);
    CHECK(ld->blend == gameplay::DecalComponent::Blend::Additive);
    CHECK(ld->sort_order == 3);
    CHECK(ld->size.z == doctest::Approx(0.5f));
}
