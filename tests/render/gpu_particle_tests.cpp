// SPDX-License-Identifier: Apache-2.0
// GPU particles (Slice 13): alpha and additive billboards, depth occlusion, soft fading into
// geometry, back-to-front sorting, frustum culling, the 2D sprite path and the full
// emitter -> runtime extraction -> renderer pipeline.
#include "gpu_fixture.hpp"
#include "victus/particles/particles.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cstdlib>

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

template <typename F>
Shot shoot(SceneRenderer& r, F&& content, const char* save = nullptr, Camera3D cam = Camera3D::look_at({0, 0, 5}, {0, 0, 0})) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::Medium);
    s.bloom = false; // keep pixels local so colour checks are exact
    r.set_settings(s);
    DirectionalLight sun;
    sun.intensity = 0;
    Environment env;
    env.sky_zenith = env.sky_horizon = {0.04f, 0.04f, 0.06f, 1};
    env.ambient_intensity = 0.2f;
    env.vignette = 0;
    r.begin(cam, sun, env);
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

ParticleInstance particle(math::Vec3 p, float size, math::Color c) {
    return {{p.x, p.y, p.z, size}, {c.r, c.g, c.b, c.a}, {0, 0, 0, 0}};
}

math::Aabb bounds_of(std::span<const ParticleInstance> ps) {
    math::Aabb b;
    for (const auto& p : ps) {
        const math::Vec3 c{p.position_size.x, p.position_size.y, p.position_size.z};
        const math::Vec3 h{p.position_size.w * 0.5f};
        b.expand(c - h);
        b.expand(c + h);
    }
    return b;
}

void submit(SceneRenderer& r, std::span<const ParticleInstance> ps, ParticleBlend blend, float softness = 0.25f) {
    ParticleMaterial m;
    m.blend = blend;
    m.softness = softness;
    r.submit_particles(ps, m, bounds_of(ps));
}

} // namespace

TEST_CASE("gpu particles: alpha billboards cover, additive billboards brighten") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Shot base = shoot(r, [](SceneRenderer&) {});
    CHECK(base.stats.particles == 0);
    const ParticleInstance red[] = {particle({0, 0, 0}, 2.0f, {1, 0, 0, 1})};
    const Shot alpha = shoot(r, [&](SceneRenderer& rr) { submit(rr, red, ParticleBlend::Alpha); }, "particles_alpha");
    CHECK(alpha.stats.particle_batches == 1);
    CHECK(alpha.stats.particles == 1);
    const auto c = alpha.center();
    CHECK(channel(c, 0) > 150);
    CHECK(channel(c, 1) < 40);
    CHECK(channel(c, 2) < 40);
    // The soft disc fades to nothing at its rim: a pixel just outside the billboard is untouched.
    CHECK(alpha.at(W / 2 + 40, H / 2) == base.at(W / 2 + 40, H / 2));

    const ParticleInstance green[] = {particle({0, 0, 0}, 2.0f, {0, 0.6f, 0, 1})};
    const Shot add = shoot(r, [&](SceneRenderer& rr) { submit(rr, green, ParticleBlend::Additive); }, "particles_additive");
    CHECK(channel(add.center(), 1) > channel(base.center(), 1) + 60);
    // Additive keeps the background: red/blue are not darkened.
    CHECK(channel(add.center(), 2) + 2 >= channel(base.center(), 2));
}

TEST_CASE("gpu particles: hidden behind geometry, softly faded where they touch it") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "particle_cube").value();
    Material grey;
    grey.base_color = {0.4f, 0.4f, 0.4f, 1};
    const math::Mat4 blocker = math::Mat4::translation({0, 0, 1.5f});
    const ParticleInstance green[] = {particle({0, 0, 0}, 1.5f, {0, 1, 0, 1})};
    const Shot hidden_base = shoot(r, [&](SceneRenderer& rr) { rr.submit(cube, grey, blocker); });
    const Shot hidden = shoot(r, [&](SceneRenderer& rr) {
        rr.submit(cube, grey, blocker);
        submit(rr, green, ParticleBlend::Additive);
    }, "particles_occluded");
    CHECK(hidden.stats.particles == 1);
    CHECK(hidden.center() == hidden_base.center()); // fully behind the cube

    // A wall at z = 0 and a particle 5 cm in front of it: a large softness fades it out.
    const math::Mat4 wall = math::Mat4::trs({0, 0, -0.5f}, {}, {8, 8, 1});
    const ParticleInstance near_wall[] = {particle({0, 0, 0.05f}, 1.5f, {0, 1, 0, 1})};
    const Shot wall_base = shoot(r, [&](SceneRenderer& rr) { rr.submit(cube, grey, wall); });
    const Shot hard = shoot(r, [&](SceneRenderer& rr) {
        rr.submit(cube, grey, wall);
        submit(rr, near_wall, ParticleBlend::Additive, 0.001f);
    });
    const Shot soft = shoot(r, [&](SceneRenderer& rr) {
        rr.submit(cube, grey, wall);
        submit(rr, near_wall, ParticleBlend::Additive, 2.0f);
    }, "particles_soft");
    const int g_base = channel(wall_base.center(), 1);
    const int g_hard = channel(hard.center(), 1) - g_base;
    const int g_soft = channel(soft.center(), 1) - g_base;
    CHECK(g_hard > 60);
    CHECK(g_soft >= 0);
    CHECK(g_soft < g_hard / 4);
}

TEST_CASE("gpu particles: alpha particles sort back to front; off-screen batches are culled") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    // Submitted near (red) first, far (blue) second: without sorting blue would cover red.
    const ParticleInstance pair[] = {particle({0, 0, 1}, 1.5f, {1, 0, 0, 1}), particle({0, 0, -1}, 3.0f, {0, 0, 1, 1})};
    const Shot shot = shoot(r, [&](SceneRenderer& rr) { submit(rr, pair, ParticleBlend::Alpha); }, "particles_sorted");
    CHECK(shot.stats.particles == 2);
    CHECK(channel(shot.center(), 0) > 150);
    CHECK(channel(shot.center(), 2) < 60);

    const ParticleInstance behind[] = {particle({0, 0, 20}, 1.0f, {1, 1, 1, 1})}; // behind the camera
    const Shot culled = shoot(r, [&](SceneRenderer& rr) {
        submit(rr, behind, ParticleBlend::Additive);
        submit(rr, pair, ParticleBlend::Alpha);
    });
    CHECK(culled.stats.particle_batches == 1);
    CHECK(culled.stats.particles == 2);
}

TEST_CASE("gpu particles: emitter components render through runtime extraction in 3D and 2D") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    particles::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    s.world().get<scene::Transform>(cam)->position = {0, 0, 5};
    const scene::Entity fx = s.create("Sparks");
    auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
    c.rate = 0;
    c.burst = 64;
    c.shape = particles::Shape::Sphere;
    c.radius = 0.3f;
    c.speed_min = c.speed_max = 0;
    c.size_start = c.size_end = 0.6f;
    c.color_start = c.color_end = {1, 0.6f, 0.1f, 1};
    c.intensity = 1;
    c.blend = particles::Blend::Alpha;
    particles::ParticleSystem ps;
    ps.update(s, 0.0f);
    s.update_transforms();

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::High);
    r.set_settings(rs);
    const runtime::ExtractStats es = runtime::extract_scene(s, assets, r, {}, nullptr);
    CHECK(es.camera_found);
    CHECK(es.particle_emitters == 1);
    CHECK(es.particles == 64);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    const auto px = dev.read_texture(target).value();
    test::save_output("particles_extracted", W, H, px);
    CHECK(r.stats().particles == 64);
    const auto center = test::pixel(px, W, W / 2, H / 2);
    CHECK(channel(center, 0) > 180); // orange cloud
    CHECK(channel(center, 0) > channel(center, 2) + 80);

    // 2D: the same emitter as additive sprites on its layer.
    s.world().get<particles::ParticleEmitterComponent>(fx)->blend = particles::Blend::Additive;
    auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    SpriteBatch& sb = *batch.value();
    sb.begin(math::Mat4::orthographic(-4, 4, -2.4f, 2.4f, -1, 1));
    CHECK(runtime::extract_sprites(s, assets, sb) == 64);
    auto cmd2 = dev.begin_commands();
    REQUIRE(sb.prepare(cmd2).ok());
    {
        rhi::RenderPassDesc rp;
        rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
        auto pass = cmd2.begin_render_pass(rp);
        sb.render(pass, cmd2);
        pass.end();
    }
    cmd2.submit_and_wait();
    const auto px2 = dev.read_texture(target).value();
    test::save_output("particles_2d", W, H, px2);
    CHECK(sb.stats().batches == 1);
    const auto c2 = test::pixel(px2, W, W / 2, H / 2);
    CHECK(channel(c2, 0) > 200);
    CHECK(channel(c2, 1) > 100);
    CHECK(channel(test::pixel(px2, W, 2, 2), 0) == 0);
    dev.destroy(target);
}

TEST_CASE("gpu particles: velocity-stretched emitters draw streaks along their motion in 3D and 2D") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    particles::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    s.world().get<scene::Transform>(cam)->position = {0, 0, 5};
    const scene::Entity fx = s.create("Streak");
    auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
    c.rate = 0;
    c.burst = 1;
    c.shape = particles::Shape::Point;
    c.angle = 0;
    c.speed_min = c.speed_max = 4;
    c.size_start = c.size_end = 0.3f;
    c.lifetime_min = c.lifetime_max = 10;
    c.color_start = c.color_end = {1, 1, 1, 1};
    c.blend = particles::Blend::Additive;
    c.velocity_stretch = 0.5f; // 0.3 m wide, 0.3 + 4 x 0.5 = 2.3 m long
    s.world().get<scene::Transform>(fx)->rotation = math::Quat::axis_angle({0, 0, 1}, -1.5707963f); // emit along +X
    particles::ParticleSystem ps;
    ps.update(s, 0.0f);
    s.update_transforms();

    // Pixels on the centre row and centre column that the particle brightened over `base`.
    const auto extent = [](const std::vector<std::uint8_t>& px, const std::vector<std::uint8_t>& base) {
        const auto lit = [&](std::uint32_t x, std::uint32_t y) {
            // The default sky is bright (~223), so tone-mapped white adds only ~20 over it.
            return channel(test::pixel(px, W, x, y), 1) > channel(test::pixel(base, W, x, y), 1) + 8;
        };
        std::uint32_t across = 0, down = 0;
        for (std::uint32_t x = 0; x < W; ++x) across += lit(x, H / 2);
        for (std::uint32_t y = 0; y < H; ++y) down += lit(W / 2, y);
        return std::pair{across, down};
    };

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    const auto draw3d = [&] {
        (void)runtime::extract_scene(s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        return dev.read_texture(target).value();
    };
    s.world().get<particles::ParticleEmitterComponent>(fx)->size_start = 0; // invisible baseline
    s.world().get<particles::ParticleEmitterComponent>(fx)->size_end = 0;
    ps.update(s, 0.0f);
    const auto sky = draw3d();
    s.world().get<particles::ParticleEmitterComponent>(fx)->size_start = 0.3f;
    s.world().get<particles::ParticleEmitterComponent>(fx)->size_end = 0.3f;
    particles::reset(s, fx);
    ps.update(s, 0.0f);
    CHECK(ps.stats().alive == 1);
    const auto px = draw3d();
    test::save_output("particles_stretched", W, H, px);
    const auto [across, down] = extent(px, sky);
    MESSAGE("3D streak: " << across << " px across, " << down << " px down");
    CHECK(across > 3 * down); // long along X on screen, thin across
    CHECK(down > 0);

    // 2D: the same particle as a stretched sprite.
    auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    SpriteBatch& sb = *batch.value();
    sb.begin(math::Mat4::orthographic(-4, 4, -2.4f, 2.4f, -1, 1));
    CHECK(runtime::extract_sprites(s, assets, sb) == 1);
    auto cmd2 = dev.begin_commands();
    REQUIRE(sb.prepare(cmd2).ok());
    {
        rhi::RenderPassDesc rp;
        rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
        auto pass = cmd2.begin_render_pass(rp);
        sb.render(pass, cmd2);
        pass.end();
    }
    cmd2.submit_and_wait();
    const auto px2 = dev.read_texture(target).value();
    test::save_output("particles_stretched_2d", W, H, px2);
    const std::vector<std::uint8_t> black(px2.size(), 0);
    const auto [across2, down2] = extent(px2, black);
    MESSAGE("2D streak: " << across2 << " px across, " << down2 << " px down");
    CHECK(across2 > 3 * down2);
    CHECK(down2 > 0);
    dev.destroy(target);
}
