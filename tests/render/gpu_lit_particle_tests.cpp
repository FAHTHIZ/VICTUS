// SPDX-License-Identifier: Apache-2.0
// Lit particles (ADR-0214): smoke and dust billboards take the sun (half-Lambert wrap around the
// camera-facing normal), the sun's cascaded shadow and the sky/ground ambient, while unlit
// particles keep their authored colour. Checked in the renderer and through runtime extraction for
// CPU and GPU-simulated emitters.
#include "gpu_fixture.hpp"
#include "victus/particles/particles.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;
constexpr float kPi = 3.14159265f;

Environment dark_env(float ambient) {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {1, 1, 1, 1};
    env.ambient_intensity = ambient;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    return env;
}

template <typename F>
std::vector<std::uint8_t> shoot(SceneRenderer& r, const DirectionalLight& sun, const Environment& env, F&& content,
                                const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.bloom = s.fxaa = s.taa = s.ssao = s.auto_exposure = false;
    r.set_settings(s);
    r.set_time(1.0);
    r.begin(Camera3D::look_at({0, 0, 10}, {0, 0, 0}), sun, env);
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    if (save) test::save_output(save, W, H, px);
    dev.destroy(target);
    return px;
}

// White alpha billboards at x = -2.5 and x = +2.5 (screen x ~ 59 and ~101).
void puffs(SceneRenderer& r, bool lit) {
    const ParticleInstance ps[] = {{{-2.5f, 0, 0, 3}, {1, 1, 1, 1}, {0, 0, 0, 0}}, {{2.5f, 0, 0, 3}, {1, 1, 1, 1}, {0, 0, 0, 0}}};
    ParticleMaterial m;
    m.softness = 0.01f;
    m.lit = lit;
    r.submit_particles(ps, m, math::Aabb{{-4.5f, -1.5f, -1.5f}, {4.5f, 1.5f, 1.5f}});
}

std::uint32_t red(const std::vector<std::uint8_t>& px, std::uint32_t x) { return channel(test::pixel(px, W, x, H / 2), 0); }

} // namespace

TEST_CASE("gpu lit particles: the sun lights them with wrap, its cascaded shadow darkens them, unlit stay authored") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    GpuMesh slab = ctx.create_mesh(make_cube(), "lit_roof").value();
    Material grey;
    grey.base_color = {0.2f, 0.2f, 0.2f, 1};
    DirectionalLight sun;
    sun.direction = {0, -1, 0};
    sun.intensity = kPi; // radiance pi: a facing-up Lambert surface would be 1, a side-on puff 0.5
    sun.shadows = true;
    sun.shadow_distance = 30.0f;
    const Environment env = dark_env(0.0f);
    // A roof over x < 0, above the view (y = 12, z in [-5, 5]), shadows only the left puff.
    const auto roof = [&](SceneRenderer& rr) { rr.submit(slab, grey, math::Mat4::trs({-20.5f, 12.0f, 0}, {}, {40, 0.5f, 10})); };

    const auto unlit = shoot(r, sun, env, [&](SceneRenderer& rr) {
        roof(rr);
        puffs(rr, false);
    });
    const auto lit = shoot(
        r, sun, env,
        [&](SceneRenderer& rr) {
            roof(rr);
            puffs(rr, true);
        },
        "lit_particles_shadow");
    // Unlit: authored white (the built-in soft disc is ~1% below 1 just off its centre) wherever
    // the roof's shadow falls.
    CHECK(red(unlit, 59) >= 250);
    CHECK(red(unlit, 101) >= 250);
    // Lit, in the sun: radiance pi / pi * wrap 0.5 (the facing normal is horizontal) = 0.5 linear,
    // ~188 in sRGB.
    CHECK(red(lit, 101) > 175);
    CHECK(red(lit, 101) < 200);
    // Lit, under the roof: no sun and no ambient.
    CHECK(red(lit, 59) < 20);

    // Sun from behind the camera (towards -z, at the puff's face): full wrap, white again; from
    // behind the puffs (towards +z) the wrap is ~0.015 (the puff is 2.5 m off-axis, so its facing
    // normal is not exactly along z): ~32 in sRGB, against no ambient.
    DirectionalLight front = sun;
    front.direction = {0, 0, -1};
    front.shadows = false;
    CHECK(red(shoot(r, front, env, [&](SceneRenderer& rr) { puffs(rr, true); }), 101) > 250);
    DirectionalLight back = front;
    back.direction = {0, 0, 1};
    CHECK(red(shoot(r, back, env, [&](SceneRenderer& rr) { puffs(rr, true); }), 101) < 45);
}

TEST_CASE("gpu lit particles: sky and ground ambient light them without a sun") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    DirectionalLight sun;
    sun.intensity = 0;
    sun.shadows = false;
    // Uniform white sky, horizon and ground at 0.25: pbr.frag's blurred ambient is 0.25 in every
    // direction, so the puff is 0.25 linear (~137 in sRGB).
    const auto px = shoot(r, sun, dark_env(0.25f), [&](SceneRenderer& rr) { puffs(rr, true); }, "lit_particles_ambient");
    CHECK(red(px, 101) > 128);
    CHECK(red(px, 101) < 146);
    // A blue sky over a black ground: the puff is lit, bluish, and dimmer than with a white
    // ground as well.
    Environment blue = dark_env(1.0f);
    blue.sky_zenith = blue.sky_horizon = {0.0f, 0.1f, 0.4f, 1};
    blue.ground = {0, 0, 0, 1};
    const auto b = shoot(r, sun, blue, [&](SceneRenderer& rr) { puffs(rr, true); });
    const std::uint32_t c = test::pixel(b, W, 101, H / 2);
    CHECK(channel(c, 2) > channel(c, 0) + 40);
    CHECK(channel(c, 2) > channel(c, 1));
}

TEST_CASE("gpu lit particles: the emitter's lit flag reaches CPU and GPU emitters through runtime extraction") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    particles::register_components();
    for (const bool on_gpu : {false, true}) {
        CAPTURE(on_gpu);
        scene::Scene s;
        const scene::Entity cam = s.create("Camera");
        s.world().add<scene::CameraComponent>(cam);
        s.world().get<scene::Transform>(cam)->position = {0, 0, 5};
        const scene::Entity fx = s.create("Smoke");
        auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
        c.rate = 0;
        c.burst = 32;
        c.shape = particles::Shape::Sphere;
        c.radius = 0.2f;
        c.speed_min = c.speed_max = 0;
        c.size_start = c.size_end = 0.8f;
        c.color_start = c.color_end = {1, 1, 1, 1};
        c.intensity = 1;
        c.seed = 7;
        c.gpu = on_gpu;
        runtime::GpuAssetCache assets(ctx, nullptr);
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        SceneRenderer& r = *sr.value();
        RenderSettings rs = RenderSettings::preset(QualityTier::High);
        rs.bloom = rs.taa = rs.fxaa = rs.auto_exposure = false;
        r.set_settings(rs);
        const auto render_center = [&](bool lit) {
            s.world().get<particles::ParticleEmitterComponent>(fx)->lit = lit;
            particles::ParticleSystem ps;
            ps.update(s, 0.0f);
            s.update_transforms();
            REQUIRE(runtime::extract_scene(s, assets, r, {}, nullptr).particle_emitters == 1);
            auto target = test::make_target(W, H);
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
            const auto px = dev.read_texture(target).value();
            dev.destroy(target);
            return test::pixel(px, W, W / 2, H / 2);
        };
        const std::uint32_t unlit = render_center(false), lit = render_center(true);
        // The default scene has no sun and a dim ambient: lit smoke is darker than authored white.
        CHECK(channel(unlit, 0) > 200);
        CHECK(channel(lit, 0) + 40 < channel(unlit, 0));
    }
}
