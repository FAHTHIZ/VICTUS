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
#include <cstring>
#include <limits>

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

TEST_CASE("gpu particles: ribbon strips face the camera, follow their points and fade into geometry") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    // A horizontal trail from x = -2 to 2, 0.4 m wide, plus a strip pointing straight at the eye
    // (degenerate side vector: must fall back to the camera right axis without NaNs).
    std::vector<RibbonPoint> pts;
    for (int i = 0; i <= 8; ++i) {
        RibbonPoint p;
        p.position = {-2.0f + 0.5f * static_cast<float>(i), 0, 0};
        p.width = 0.4f;
        p.color = {1, 1, 1, 1};
        pts.push_back(p);
    }
    const std::vector<std::uint32_t> one{9};
    math::Aabb b;
    for (const auto& p : pts) {
        b.expand(p.position - math::Vec3{0.2f});
        b.expand(p.position + math::Vec3{0.2f});
    }
    ParticleMaterial m;
    m.blend = ParticleBlend::Additive;
    const Shot base = shoot(r, [](SceneRenderer&) {});
    const Shot shot = shoot(r, [&](SceneRenderer& rr) { rr.submit_ribbons(pts, one, m, b); }, "particles_ribbon");
    CHECK(shot.stats.ribbon_vertices == 8 * 6);
    CHECK(shot.stats.particles == 0);
    CHECK(shot.stats.particle_batches == 1);
    const auto lit = [&](const Shot& s, std::uint32_t x, std::uint32_t y) { return channel(s.at(x, y), 1) > channel(base.at(x, y), 1) + 20; };
    std::uint32_t across = 0, down = 0;
    for (std::uint32_t x = 0; x < W; ++x) across += lit(shot, x, H / 2);
    for (std::uint32_t y = 0; y < H; ++y) down += lit(shot, W / 2, y);
    MESSAGE("ribbon: " << across << " px across, " << down << " px down");
    CHECK(across > 60); // 4 m at 5 m distance spans most of the view
    CHECK(down > 2);
    CHECK(down < 20); // 0.4 m wide
    CHECK(across > 4 * down);

    // Seen from above (camera on +Y looking down), the same strip still faces the camera.
    const Camera3D top = Camera3D::look_at({0, 5, 0.001f}, {0, 0, 0});
    const Shot base_top = shoot(r, [](SceneRenderer&) {}, nullptr, top);
    const Shot above = shoot(r, [&](SceneRenderer& rr) { rr.submit_ribbons(pts, one, m, b); }, nullptr, top);
    std::uint32_t lit_above = 0;
    for (std::uint32_t x = 0; x < W; ++x) lit_above += channel(above.at(x, H / 2), 1) > channel(base_top.at(x, H / 2), 1) + 20;
    CHECK(lit_above > 60);

    // Strips with fewer than two points are skipped; a strip along the view axis draws nothing
    // broken (the fallback side axis keeps it finite).
    std::vector<RibbonPoint> axis(2);
    axis[0].position = {0, 0, -1};
    axis[1].position = {0, 0, 1};
    axis[0].width = axis[1].width = 0.4f;
    axis[0].color = axis[1].color = {1, 1, 1, 1};
    const std::vector<std::uint32_t> counts{1, 2};
    std::vector<RibbonPoint> mixed{pts[0]};
    mixed.insert(mixed.end(), axis.begin(), axis.end());
    math::Aabb ab;
    ab.expand({-0.3f, -0.3f, -1.2f});
    ab.expand({0.3f, 0.3f, 1.2f});
    const Shot degenerate = shoot(r, [&](SceneRenderer& rr) { rr.submit_ribbons(mixed, counts, m, ab); });
    CHECK(degenerate.stats.ribbon_vertices == 6);

    // Behind an opaque wall the ribbon is hidden (soft depth test of the particle pass).
    auto cube = ctx.create_mesh(make_cube(), "ribbon_cube").value();
    Material wall;
    wall.base_color = {0.1f, 0.1f, 0.1f, 1};
    const Shot hidden = shoot(r, [&](SceneRenderer& rr) {
        rr.submit(cube, wall, math::Mat4::trs({0, 0, 1.5f}, {}, {1.2f, 1.2f, 0.2f}));
        rr.submit_ribbons(pts, one, m, b);
    });
    const Shot wall_only = shoot(r, [&](SceneRenderer& rr) {
        rr.submit(cube, wall, math::Mat4::trs({0, 0, 1.5f}, {}, {1.2f, 1.2f, 0.2f}));
    });
    CHECK(hidden.center() == wall_only.center());
    CHECK(channel(hidden.at(W / 2 - 26, H / 2), 1) > channel(wall_only.at(W / 2 - 26, H / 2), 1) + 20); // still visible beside the wall
}

TEST_CASE("gpu particles: emitter trails render through runtime extraction in 3D and 2D") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    particles::register_components();
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().add<scene::CameraComponent>(cam);
    s.world().get<scene::Transform>(cam)->position = {0, 0, 5};
    const scene::Entity fx = s.create("Comet");
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
    c.trail_lifetime = 1.0f;
    c.trail_width_end = 1.0f;
    c.trail_alpha_end = 1.0f;
    c.trail_min_distance = 0.1f;
    c.trail_max_points = 64;
    c.trail_billboards = false;
    s.world().get<scene::Transform>(fx)->position = {-2, 0, 0};
    s.world().get<scene::Transform>(fx)->rotation = math::Quat::axis_angle({0, 0, 1}, -1.5707963f); // emit along +X
    particles::ParticleSystem ps;
    ps.update(s, 0.0f);
    for (int i = 0; i < 20; ++i) ps.update(s, 0.05f); // the head reaches x = 2; the trail covers the last 4 m
    s.update_transforms();

    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    const auto draw3d = [&] {
        const runtime::ExtractStats st = runtime::extract_scene(s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        return std::pair{dev.read_texture(target).value(), st};
    };
    const auto extent = [](const std::vector<std::uint8_t>& px, const std::vector<std::uint8_t>& base) {
        const auto lit = [&](std::uint32_t x, std::uint32_t y) { return channel(test::pixel(px, W, x, y), 1) > channel(test::pixel(base, W, x, y), 1) + 8; };
        std::uint32_t across = 0, down = 0;
        for (std::uint32_t x = 0; x < W; ++x) across += lit(x, H / 2);
        for (std::uint32_t y = 0; y < H; ++y) down += lit(W / 2, y);
        return std::pair{across, down};
    };
    s.world().get<particles::ParticleEmitterComponent>(fx)->trail_width = 0; // invisible baseline
    ps.update(s, 0.0f);
    const auto sky = draw3d().first;
    s.world().get<particles::ParticleEmitterComponent>(fx)->trail_width = 1;
    ps.update(s, 0.0f);
    const auto [px, st] = draw3d();
    CHECK(st.particle_trails == 1);
    CHECK(st.particles == 0); // trails only
    CHECK(r.stats().ribbon_vertices > 0);
    test::save_output("particles_trail", W, H, px);
    const auto [across, down] = extent(px, sky);
    MESSAGE("3D trail: " << across << " px across, " << down << " px down");
    CHECK(across > 50);
    CHECK(down > 0);
    CHECK(across > 4 * down);

    // 2D: one sprite per trail segment.
    auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    SpriteBatch& sb = *batch.value();
    sb.begin(math::Mat4::orthographic(-4, 4, -2.4f, 2.4f, -1, 1));
    CHECK(runtime::extract_sprites(s, assets, sb) > 10);
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
    test::save_output("particles_trail_2d", W, H, px2);
    const std::vector<std::uint8_t> black(px2.size(), 0);
    const auto [across2, down2] = extent(px2, black);
    MESSAGE("2D trail: " << across2 << " px across, " << down2 << " px down");
    CHECK(across2 > 60);
    CHECK(down2 > 0);
    CHECK(across2 > 4 * down2);
    dev.destroy(target);
}

// ---- GPU-simulated particles (ADR-0097) ----

namespace {

struct GpuFx {
    scene::Scene s;
    scene::Entity fx, cam;
    particles::ParticleSystem ps;
    GpuFx() {
        particles::register_components();
        cam = s.create("Camera");
        s.world().add<scene::CameraComponent>(cam);
        s.world().get<scene::Transform>(cam)->position = {0, 0, 6};
        fx = s.create("GpuFx");
        auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
        c.gpu = true;
        c.blend = particles::Blend::Additive;
        c.color_start = c.color_end = {1, 1, 1, 1};
        c.size_start = c.size_end = 0.2f;
    }
    particles::ParticleEmitterComponent& emitter() { return *s.world().get<particles::ParticleEmitterComponent>(fx); }
    particles::GpuEmitterView view() {
        std::vector<particles::EmitterView> views;
        particles::gather(s, views);
        for (const auto& v : views)
            if (v.entity == fx && v.gpu) return *v.gpu;
        FAIL("no GPU emitter view");
        return {};
    }
};

std::vector<particles::GpuParticle> read_state(SceneRenderer& r, std::uint64_t key) {
    auto got = r.read_gpu_particles(key);
    REQUIRE(got.ok());
    std::vector<particles::GpuParticle> out(got.value().size());
    std::memcpy(static_cast<void*>(out.data()), got.value().data(), out.size() * sizeof(particles::GpuParticle));
    return out;
}

} // namespace

TEST_CASE("gpu particles: compute simulation matches the CPU reference kernel and draws") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    GpuFx fx;
    auto& c = fx.emitter();
    c.rate = 300;
    c.max_particles = 256;
    c.lifetime_min = 0.3f;
    c.lifetime_max = 1.2f;
    c.speed_min = 1;
    c.speed_max = 3;
    c.angle = 40;
    c.gravity = {0, -2, 0};
    c.drag = 0.3f;
    c.turbulence = 2;
    c.turbulence_frequency = 0.6f;
    c.turbulence_speed = 1.5f;
    c.spin_min = -90;
    c.spin_max = 90;
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    std::vector<particles::GpuParticle> ref(256);
    std::uint64_t seen = 0;
    double last = 0;
    fx.ps.update(fx.s, 0.0f);
    std::vector<std::uint8_t> px;
    for (int f = 0; f < 40; ++f) {
        fx.ps.update(fx.s, 1.0f / 60.0f);
        fx.s.update_transforms();
        const particles::GpuEmitterView v = fx.view();
        // Reference: advance the live slots, then write this frame's spawns pre-aged to the clock.
        for (auto& p : ref)
            if (p.age < p.life) particles::gpu_integrate(c, p, static_cast<float>(v.time - last), last);
        for (const auto& rec : v.records) {
            if (rec.index < seen || rec.time + rec.particle.life <= v.time) continue;
            particles::GpuParticle p = rec.particle;
            particles::gpu_integrate(c, p, static_cast<float>(v.time - rec.time), rec.time);
            ref[rec.particle.meta[0]] = p;
        }
        seen = v.spawned;
        last = v.time;
        const runtime::ExtractStats st = runtime::extract_scene(fx.s, assets, r, {}, nullptr);
        CHECK(st.particle_emitters == 1);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        if (f == 39) px = dev.read_texture(target).value();
    }
    const particles::GpuEmitterView v = fx.view();
    const auto state = read_state(r, v.key);
    REQUIRE(state.size() == 256);
    std::uint32_t gpu_alive = 0, compared = 0;
    float worst = 0;
    for (std::size_t i = 0; i < state.size(); ++i) {
        const bool live = state[i].age < state[i].life;
        gpu_alive += live;
        if (!live || !(ref[i].age < ref[i].life)) continue;
        worst = std::max(worst, math::length(state[i].position - ref[i].position));
        CHECK(std::abs(state[i].rotation - ref[i].rotation) < 1e-3f);
        ++compared;
    }
    MESSAGE("GPU particles: " << gpu_alive << " alive (CPU says " << v.alive << "), worst position error " << worst << " m");
    CHECK(std::abs(static_cast<int>(gpu_alive) - static_cast<int>(v.alive)) <= 2);
    CHECK(compared + 2 >= v.alive);
    CHECK(v.alive > 100);
    CHECK(worst < 5e-3f);
    CHECK(r.stats().gpu_particle_emitters == 1);
    CHECK(r.stats().gpu_particles == v.alive);
    CHECK(r.stats().particle_batches == 1);
    test::save_output("particles_gpu", W, H, px);
    // Baseline: the same camera and sky without the emitter.
    std::vector<std::uint8_t> sky;
    {
        scene::Scene empty;
        const scene::Entity cam = empty.create("Camera");
        empty.world().add<scene::CameraComponent>(cam);
        empty.world().get<scene::Transform>(cam)->position = {0, 0, 6};
        empty.update_transforms();
        auto sr2 = SceneRenderer::create(ctx);
        REQUIRE(sr2.ok());
        sr2.value()->set_settings(rs);
        (void)runtime::extract_scene(empty, assets, *sr2.value(), {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(sr2.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        sky = dev.read_texture(target).value();
    }
    std::uint32_t lit = 0, far_changed = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const bool brighter = channel(test::pixel(px, W, x, y), 2) > channel(test::pixel(sky, W, x, y), 2) + 10;
            lit += brighter;
            far_changed += brighter && (x < W / 6 || x > W * 5 / 6); // nothing reaches the frame edges
        }
    MESSAGE("GPU particles brighten " << lit << " pixels");
    CHECK(lit > 50);
    CHECK(far_changed == 0);
    // The 2D sprite path skips GPU emitters.
    auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    batch.value()->begin(math::Mat4::orthographic(-4, 4, -2.4f, 2.4f, -1, 1));
    CHECK(runtime::extract_sprites(fx.s, assets, *batch.value()) == 0);
    dev.destroy(target);
}

TEST_CASE("gpu particles: depth-buffer collision bounces particles off the floor; reset clears the GPU state") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto cube = ctx.create_mesh(make_cube(), "gpu_particle_floor").value();
    Material grey;
    grey.base_color = {0.3f, 0.3f, 0.3f, 1};
    const math::Mat4 floor = math::Mat4::trs({0, -1.1f, 0}, {}, {8, 0.4f, 8}); // top at y = -0.9
    const auto run = [&](bool collide, std::vector<particles::GpuParticle>& out, std::uint32_t& alive) {
        GpuFx fx;
        auto& c = fx.emitter();
        c.rate = 0;
        c.burst = 200;
        c.max_particles = 200;
        c.lifetime_min = c.lifetime_max = 10;
        c.speed_min = 0.3f;
        c.speed_max = 0.8f;
        c.angle = 30;
        c.gravity = {0, -9.8f, 0};
        c.collision = collide;
        c.bounce = 0.3f;
        c.friction = 0.3f;
        fx.s.world().get<scene::Transform>(fx.fx)->position = {0, 1, 0};
        auto* cam = fx.s.world().get<scene::Transform>(fx.cam);
        cam->position = {0, 1.5f, 5};
        cam->rotation = math::Quat::axis_angle({1, 0, 0}, -0.35f);
        runtime::GpuAssetCache assets(ctx, nullptr);
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        SceneRenderer& r = *sr.value();
        auto target = test::make_target(W, H);
        fx.ps.update(fx.s, 0.0f);
        const auto frame = [&](float dt) {
            fx.ps.update(fx.s, dt);
            fx.s.update_transforms();
            (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
            r.submit(cube, grey, floor);
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
        };
        for (int f = 0; f < 90; ++f) frame(1.0f / 60.0f);
        test::save_output(collide ? "particles_gpu_collision" : "particles_gpu_no_collision", W, H, dev.read_texture(target).value());
        const std::uint64_t key = fx.view().key;
        out = read_state(r, key);
        alive = fx.view().alive;
        // reset(): the next frame starts from cleared (dead) slots.
        CHECK(particles::reset(fx.s, fx.fx));
        fx.emitter().burst = 0;
        frame(1.0f / 60.0f);
        const auto cleared = read_state(r, key);
        std::uint32_t live = 0;
        for (const auto& p : cleared) live += p.age < p.life;
        CHECK(live == 0);
        CHECK(fx.view().alive == 0);
        dev.destroy(target);
    };
    std::vector<particles::GpuParticle> with, without;
    std::uint32_t alive_with = 0, alive_without = 0;
    run(true, with, alive_with);
    run(false, without, alive_without);
    CHECK(alive_with == 200);
    CHECK(alive_without == 200);
    float lowest_with = 1e9f, lowest_without = 1e9f;
    std::uint32_t resting = 0;
    for (const auto& p : with) {
        lowest_with = std::min(lowest_with, p.position.y);
        resting += p.position.y < -0.7f && std::abs(p.velocity.y) < 1.5f;
    }
    for (const auto& p : without) lowest_without = std::min(lowest_without, p.position.y);
    MESSAGE("collision: lowest y " << lowest_with << " (without: " << lowest_without << "), " << resting << " resting on the floor");
    CHECK(lowest_without < -5.0f);   // falls through the floor without collision
    CHECK(lowest_with > -0.9f - 0.1f); // never more than a frame's fall below the surface
    CHECK(resting > 150);
}

TEST_CASE("gpu particles: alpha-blended GPU emitters are depth sorted on the GPU, additive ones are not") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    GpuFx fx;
    auto& c = fx.emitter();
    c.blend = particles::Blend::Alpha;
    c.color_start = c.color_end = {0.8f, 0.6f, 0.4f, 0.5f};
    c.rate = 600;
    c.max_particles = 700; // pads to 1024 pairs: local sort, global steps and merges all run
    c.lifetime_min = c.lifetime_max = 3.0f;
    c.speed_min = 1;
    c.speed_max = 3;
    c.angle = 60;
    c.gravity = {0, 0, 0};
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    const auto frame = [&] {
        fx.ps.update(fx.s, 1.0f / 60.0f);
        fx.s.update_transforms();
        (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    };
    fx.ps.update(fx.s, 0.0f);
    for (int f = 0; f < 45; ++f)
        frame();
    const particles::GpuEmitterView v = fx.view();
    REQUIRE(v.alive > 300);
    const auto check_order = [&](math::Vec3 eye) {
        auto got = r.read_gpu_particle_draw(v.key);
        REQUIRE(got.ok());
        const auto& draw = got.value();
        REQUIRE(draw.size() == 700);
        std::uint32_t visible = 0, out_of_order = 0, after_dead = 0;
        double worst_violation = 0; // relative; ulp-level is CPU/GPU rounding, larger is a sort error
        float prev = std::numeric_limits<float>::infinity();
        bool dead_seen = false;
        for (const ParticleInstance& p : draw) {
            const bool live = p.position_size.w > 0 && p.color.w > 0;
            if (!live) {
                dead_seen = true;
                continue;
            }
            after_dead += dead_seen; // live particles all come before the dead slots
            ++visible;
            const math::Vec3 d = math::Vec3{p.position_size.x, p.position_size.y, p.position_size.z} - eye;
            const float dist = math::dot(d, d);
            // The GPU computes dot(d, d) with its own rounding (FMA contraction), so near-ties may
            // differ from this CPU recomputation by an ulp or two; a real sort error is far larger.
            out_of_order += dist > prev * (1.0f + 1e-5f);
            if (dist > prev) worst_violation = std::max(worst_violation, double(dist) / double(prev) - 1.0);
            prev = dist;
        }
        MESSAGE("sorted " << visible << " live particles (CPU says " << v.alive << "), worst order violation "
                          << worst_violation);
        CHECK(std::abs(static_cast<int>(visible) - static_cast<int>(v.alive)) <= 2);
        CHECK(out_of_order == 0);
        CHECK(after_dead == 0);
    };
    check_order({0, 0, 6});
    CHECK(r.stats().gpu_particle_sorted_emitters == 1);
    CHECK(r.stats().gpu_particle_sort_passes == 8); // keys, local, k=512 (1 step + merge), k=1024 (2 + merge), gather
    std::vector<std::uint8_t> px = dev.read_texture(target).value();
    test::save_output("particles_gpu_sorted", W, H, px);

    // A different eye re-sorts every frame, also while the simulation does not advance.
    fx.s.world().get<scene::Transform>(fx.cam)->position = {1.5f, 0.5f, 7};
    fx.s.update_transforms();
    (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
    {
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    check_order({1.5f, 0.5f, 7});

    // Additive blending is order independent: drawn in slot order without the sort passes.
    fx.emitter().blend = particles::Blend::Additive;
    frame();
    CHECK(r.stats().gpu_particle_sorted_emitters == 0);
    CHECK(r.stats().gpu_particle_sort_passes == 0);
    CHECK(r.stats().gpu_particles > 0);
    dev.destroy(target);
}

TEST_CASE("gpu particles: effect budgets keep the nearest particle batches, decals and water surfaces") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const auto cloud = [](float z) {
        std::vector<ParticleInstance> ps;
        for (int i = 0; i < 100; ++i) ps.push_back(particle({static_cast<float>(i % 10) * 0.1f - 0.5f, static_cast<float>(i / 10) * 0.1f - 0.5f, z}, 0.1f, {1, 1, 1, 0.5f}));
        return ps;
    };
    const auto near_c = cloud(2), mid_c = cloud(0), far_c = cloud(-3);
    const auto frame = [&](RenderSettings s) {
        r.set_settings(s);
        DirectionalLight sun;
        sun.intensity = 0;
        r.begin(Camera3D::look_at({0, 0, 5}, {0, 0, 0}), sun, Environment{});
        for (const auto* c : {&far_c, &near_c, &mid_c}) submit(r, *c, ParticleBlend::Alpha);
        for (int i = 0; i < 6; ++i) {
            Decal d;
            d.transform = math::Mat4::trs({0, 0, static_cast<float>(-i)}, {}, {1, 1, 1});
            r.add_decal(d);
        }
        for (int i = 0; i < 3; ++i) {
            WaterSurface w;
            w.center = {0, -1, static_cast<float>(-4 * i)};
            w.half_extent = {1, 1};
            w.resolution = 8;
            r.add_water(w);
        }
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        dev.destroy(target);
    };
    RenderSettings s = RenderSettings::preset(QualityTier::Medium);
    frame(s);
    CHECK(r.stats().particles == 300);
    CHECK(r.stats().decals == 6);
    CHECK(r.stats().water_surfaces == 3);
    for (const char* n : {"particles", "decals", "water"}) CHECK_MESSAGE(r.path_report().find(n)->state == PathState::Active, n);

    s.max_particles = 150; // the nearest batch fits, the next one would not: whole batches only
    s.max_decals = 2;
    s.max_water_surfaces = 1;
    frame(s);
    CHECK(r.stats().particle_batches == 1);
    CHECK(r.stats().particles == 100);
    CHECK(r.stats().decals == 2);
    CHECK(r.stats().water_surfaces == 1);
    const RenderPathReport rep = r.path_report();
    for (const char* n : {"particles", "decals", "water"}) {
        CHECK_MESSAGE(rep.find(n)->state == PathState::Reduced, n);
        CHECK_MESSAGE(rep.find(n)->reason == PathReason::Setting, n);
    }
    CHECK(rep.find("particles")->detail == "100 particles in 1 batch; nearest 100 of 300 CPU particles in view (max_particles 150)");

    s.max_particles = 40; // the nearest batch alone is over: it is cut
    frame(s);
    CHECK(r.stats().particle_batches == 1);
    CHECK(r.stats().particles == 40);

    // The Low preset: everything here fits its budgets, so nothing is reported as cut.
    frame(RenderSettings::preset(QualityTier::Low));
    CHECK(r.stats().particles == 300);
    CHECK(r.path_report().find("particles")->state == PathState::Active);
}

TEST_CASE("gpu particles: GPU trails record ring histories on the GPU and draw camera-facing ribbons") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    GpuFx fx;
    auto& c = fx.emitter();
    c.rate = 0;
    c.max_particles = 64;
    c.shape = particles::Shape::Point;
    c.angle = 25;
    c.lifetime_min = c.lifetime_max = 3.0f;
    c.speed_min = c.speed_max = 2.0f;
    c.gravity = {0, 0, 0};
    c.color_start = c.color_end = {0.6f, 0.8f, 1.0f, 1.0f};
    c.size_start = c.size_end = 0.2f;
    c.trail_lifetime = 0.5f;
    c.trail_min_distance = 0.05f;
    c.trail_max_points = 16;
    c.trail_width = 1.0f;
    c.trail_width_end = 0.0f;
    c.trail_alpha_end = 0.0f;
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    const auto frame = [&] {
        fx.ps.update(fx.s, 1.0f / 60.0f);
        fx.s.update_transforms();
        (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    };
    fx.ps.update(fx.s, 0.0f);
    frame(); // nothing alive yet: the baseline image
    const std::vector<std::uint8_t> empty = dev.read_texture(target).value();
    REQUIRE(particles::emit(fx.s, fx.fx, 6));
    for (int f = 0; f < 40; ++f) {
        if (f == 15) REQUIRE(particles::emit(fx.s, fx.fx, 6)); // younger particles: shorter trails
        frame();
    }
    const particles::GpuEmitterView v = fx.view();
    REQUIRE(v.alive == 12);
    REQUIRE(r.gpu_particle_trail_points(v.key) == 16);
    CHECK(r.stats().gpu_particle_trail_emitters == 1);
    CHECK(r.stats().gpu_particle_trails_reduced == 0);
    CHECK(r.stats().gpu_particle_trail_vertices == 64 * 17 * 2);
    CHECK(r.stats().particle_batches == 2); // ribbons + heads
    {
        const PathFeature* gp = r.path_report().find("gpu_particles");
        REQUIRE(gp != nullptr);
        CHECK(gp->state == PathState::Active);
        CHECK(gp->detail.find("1 with trails") != std::string::npos);
    }
    const auto state = read_state(r, v.key);
    auto got = r.read_gpu_particle_trails(v.key);
    REQUIRE(got.ok());
    const std::vector<ParticleInstance>& rib = got.value();
    REQUIRE(rib.size() == 64u * 17u * 2u);
    const auto center = [](const ParticleInstance& p) { return math::Vec3{p.position_size.x, p.position_size.y, p.position_size.z}; };
    std::uint32_t live = 0, strips = 0, old_strips = 0, young_strips = 0, bad_dead = 0;
    float worst_line = 0, worst_head = 0, worst_spacing = 1;
    for (std::uint32_t slot = 0; slot < 64; ++slot) {
        const particles::GpuParticle& p = state[slot];
        const ParticleInstance* pairs = rib.data() + static_cast<std::size_t>(slot) * 17 * 2;
        std::uint32_t n = 0;
        // Untextured trails carry v = 0.5 (the soft disc's middle row); unused pairs carry 0.
        while (n < 17 && pairs[n * 2 + 1].params.w == 0.5f) ++n;
        for (std::uint32_t k = n; k < 17; ++k) // unused pairs collapse to zero width
            bad_dead += pairs[k * 2].position_size.w != 0 || pairs[k * 2 + 1].position_size.w != 0;
        if (!(p.age < p.life)) {
            CHECK(n == 0);
            continue;
        }
        ++live;
        CHECK(p.meta[3] == 16);
        CHECK(p.meta[1] <= 16);
        REQUIRE(n >= 2);
        ++strips;
        const math::Vec3 tail = center(pairs[0]);
        if (p.age > c.trail_lifetime + 0.05f) {
            // Older than the trail lifetime: the ring is full or expiring, about 0.5 s x 2 m/s long.
            const float len = math::length(p.position - tail);
            old_strips += pairs[0].color.w <= 0.15f && len > 0.85f && len < 1.05f && p.meta[1] >= 14;
        } else {
            // Younger: the trail still starts at the birth point, as old as the particle.
            young_strips += math::length(tail) < 1e-3f && std::abs((1.0f - pairs[0].color.w) - p.age / c.trail_lifetime) < 0.02f;
        }
        const math::Vec3 dir = math::normalize(p.velocity);
        float prev_v = 2, prev_w = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            const ParticleInstance& a = pairs[k * 2];
            const ParticleInstance& b = pairs[k * 2 + 1];
            CHECK(math::length(center(a) - center(b)) < 1e-6f);
            CHECK(a.position_size.w == doctest::Approx(-b.position_size.w));
            // Every point lies on the particle's straight path from the emitter.
            const math::Vec3 q = center(a);
            worst_line = std::max(worst_line, math::length(q - dir * math::dot(q, dir)));
            const float vv = 1.0f - a.color.w; // alpha fades to 0 at the end of the trail lifetime
            CHECK(vv >= -1e-6f);
            CHECK(vv <= prev_v + 1e-6f); // age along the trail falls from the tail to the head
            CHECK(b.position_size.w >= prev_w - 1e-6f); // and the ribbon widens toward the head
            CHECK(b.position_size.w == doctest::Approx(0.1f * (1.0f - vv)).epsilon(1e-3));
            CHECK(std::abs(math::dot(math::Vec3{a.params.x, a.params.y, a.params.z}, dir)) > 0.999f);
            if (k + 2 < n) worst_spacing = std::min(worst_spacing, math::length(center(pairs[(k + 1) * 2]) - q));
            prev_v = vv;
            prev_w = b.position_size.w;
        }
        const ParticleInstance& head = pairs[(n - 1) * 2 + 1];
        worst_head = std::max(worst_head, math::length(center(head) - p.position));
        CHECK(head.color.w == doctest::Approx(1.0f));
        CHECK(head.position_size.w == doctest::Approx(0.1f).epsilon(1e-3)); // half of size x trail_width
    }
    MESSAGE("GPU trails: " << strips << " strips, worst off-path " << worst_line << " m, head " << worst_head << " m, min spacing " << worst_spacing);
    CHECK(live == 12);
    CHECK(strips == 12);
    CHECK(old_strips == 6);   // the first six keep 0.5 s of trail
    CHECK(young_strips == 6); // the later ones are still growing from their birth point
    CHECK(bad_dead == 0);
    CHECK(worst_line < 1e-3f);
    CHECK(worst_head < 1e-4f);
    CHECK(worst_spacing >= 0.05f - 1e-4f);
    std::vector<std::uint8_t> with_heads = dev.read_texture(target).value();
    test::save_output("particles_gpu_trails", W, H, with_heads);

    // Trails only: the heads are not drawn but the ribbons are, and they light pixels behind the heads.
    fx.emitter().trail_billboards = false;
    frame();
    CHECK(r.stats().particle_batches == 1);
    CHECK(r.stats().gpu_particles == 12);
    const std::vector<std::uint8_t> trails_only = dev.read_texture(target).value();
    test::save_output("particles_gpu_trails_only", W, H, trails_only);
    std::uint32_t lit = 0, lit_below = 0, lit_edges = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const bool on = channel(test::pixel(trails_only, W, x, y), 0) > channel(test::pixel(empty, W, x, y), 0) + 12;
            lit += on;
            lit_below += on && y > H / 2 + 4; // trails rise from the emitter at the centre
            lit_edges += on && (x < W / 4 || x > W * 3 / 4);
        }
    MESSAGE("trails only light " << lit << " pixels");
    CHECK(lit > 40);
    CHECK(lit_below == 0);
    CHECK(lit_edges == 0);

    // A new point count restarts every history (like the CPU path) without dropping particles.
    fx.emitter().trail_max_points = 4;
    frame();
    CHECK(r.gpu_particle_trail_points(v.key) == 4);
    {
        const auto st = read_state(r, v.key);
        std::uint32_t restarted = 0;
        for (const auto& p : st)
            if (p.age < p.life) restarted += p.meta[3] == 4 && p.meta[1] <= 1;
        CHECK(restarted == 12);
    }

    // Large emitters get fewer points per particle (kMaxGpuTrailSegments), then none.
    CHECK(SceneRenderer::gpu_trail_points(64, 16) == 16);
    CHECK(SceneRenderer::gpu_trail_points(32768, 16) == 8);
    CHECK(SceneRenderer::gpu_trail_points(1u << 18, 16) == 0);
    CHECK(SceneRenderer::gpu_trail_points(64, 0) == 0);
    CHECK(SceneRenderer::gpu_trail_points(64, 200) == 64);
    fx.emitter().trail_max_points = 16;
    fx.emitter().max_particles = 32768;
    frame();
    const particles::GpuEmitterView big = fx.view();
    CHECK(r.gpu_particle_trail_points(big.key) == 8);
    CHECK(r.stats().gpu_particle_trails_reduced == 1);
    {
        const PathFeature* gp = r.path_report().find("gpu_particles");
        REQUIRE(gp != nullptr);
        CHECK(gp->state == PathState::Reduced);
        CHECK(gp->reason == PathReason::Budget);
    }
    dev.destroy(target);
}

TEST_CASE("gpu particles: over-life colour gradient and size curve match the CPU reference (ADR-0209)") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    GpuFx fx;
    auto& c = fx.emitter();
    c.rate = 200;
    c.max_particles = 128;
    c.lifetime_min = 0.4f;
    c.lifetime_max = 1.0f;
    c.speed_min = 0.5f;
    c.speed_max = 1.5f;
    c.size_start = 0.3f;
    c.size_variation = 0.3f;
    c.intensity = 1.5f;
    c.color_over_life = {{0.0f, {1, 1, 1, 1}}, {0.3f, {1, 0.6f, 0.1f, 1}}, {0.7f, {0.6f, 0.1f, 0.0f, 0.5f}}, {1.0f, {0.1f, 0, 0, 0}}};
    c.size_over_life = {{0.0f, 0.2f}, {0.15f, 1.6f}, {1.0f, 0.6f}};
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    fx.ps.update(fx.s, 0.0f);
    for (int f = 0; f < 30; ++f) {
        fx.ps.update(fx.s, 1.0f / 60.0f);
        fx.s.update_transforms();
        (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    const particles::GpuEmitterView v = fx.view();
    const auto state = read_state(r, v.key);
    auto drawn = r.read_gpu_particle_draw(v.key); // additive: unsorted, instance i = slot i
    REQUIRE(drawn.ok());
    REQUIRE(drawn.value().size() == state.size());
    std::uint32_t compared = 0;
    float worst_color = 0, worst_size = 0;
    for (std::size_t i = 0; i < state.size(); ++i) {
        if (!(state[i].age < state[i].life)) continue;
        // Fresh lookup: the system's runtime state moves the entity, so `c` no longer refers to it.
        const particles::Billboard want = particles::gpu_billboard(fx.emitter(), state[i], math::Mat4::identity(), math::Mat4::identity());
        const ParticleInstance& got = drawn.value()[i];
        worst_size = std::max(worst_size, std::abs(got.position_size.w - want.size));
        worst_color = std::max({worst_color, std::abs(got.color.x - want.color.r), std::abs(got.color.y - want.color.g),
                                std::abs(got.color.z - want.color.b), std::abs(got.color.w - want.color.a)});
        ++compared;
    }
    MESSAGE("over-life curves: " << compared << " particles compared, worst colour error " << worst_color << ", worst size error "
                                 << worst_size << " m");
    CHECK(compared > 50);
    CHECK(worst_color < 1e-4f);
    CHECK(worst_size < 1e-5f);
    dev.destroy(target);
}

TEST_CASE("gpu particles: speed and spin curves over life match the CPU reference kernel (ADR-0211)") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    GpuFx fx;
    {
        auto& c = fx.emitter();
        c.rate = 240;
        c.max_particles = 192;
        c.lifetime_min = 0.4f;
        c.lifetime_max = 1.0f;
        c.speed_min = 1;
        c.speed_max = 3;
        c.angle = 35;
        c.gravity = {0, -1, 0};
        c.drag = 0.2f;
        c.spin_min = 90;
        c.spin_max = 360;
        c.velocity_stretch = 0.05f;
        c.blend = particles::Blend::Additive;
        c.speed_over_life = {{0.0f, 2.5f}, {0.2f, 0.4f}, {0.6f, 0.1f}, {1.0f, 1.0f}};
        c.spin_over_life = {{0.0f, 3.0f}, {0.5f, 0.0f}};
    }
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    RenderSettings rs = RenderSettings::preset(QualityTier::Medium);
    rs.bloom = false;
    r.set_settings(rs);
    auto target = test::make_target(W, H);
    std::vector<particles::GpuParticle> ref(192);
    std::uint64_t seen = 0;
    double last = 0;
    fx.ps.update(fx.s, 0.0f);
    for (int f = 0; f < 36; ++f) {
        fx.ps.update(fx.s, 1.0f / 60.0f);
        fx.s.update_transforms();
        const particles::GpuEmitterView v = fx.view();
        const auto& c = fx.emitter();
        for (auto& p : ref)
            if (p.age < p.life) particles::gpu_integrate(c, p, static_cast<float>(v.time - last), last);
        for (const auto& rec : v.records) {
            if (rec.index < seen || rec.time + rec.particle.life <= v.time) continue;
            particles::GpuParticle p = rec.particle;
            particles::gpu_integrate(c, p, static_cast<float>(v.time - rec.time), rec.time);
            ref[rec.particle.meta[0]] = p;
        }
        seen = v.spawned;
        last = v.time;
        (void)runtime::extract_scene(fx.s, assets, r, {}, nullptr);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    const particles::GpuEmitterView v = fx.view();
    const auto state = read_state(r, v.key);
    REQUIRE(state.size() == 192);
    auto drawn = r.read_gpu_particle_draw(v.key);
    REQUIRE(drawn.ok());
    REQUIRE(drawn.value().size() == state.size());
    std::uint32_t compared = 0;
    float worst = 0, worst_rot = 0, worst_aspect = 0;
    // Without the curves the same particles would have travelled farther: check the curve acts.
    particles::ParticleEmitterComponent plain = fx.emitter();
    plain.speed_over_life.clear();
    float slowed = 0;
    const math::Mat4 view = math::Mat4::translation({0, 0, -6}); // the camera at (0, 0, 6) looking down -Z
    for (std::size_t i = 0; i < state.size(); ++i) {
        if (!(state[i].age < state[i].life) || !(ref[i].age < ref[i].life)) continue;
        worst = std::max(worst, math::length(state[i].position - ref[i].position));
        worst_rot = std::max(worst_rot, std::abs(state[i].rotation - ref[i].rotation));
        const particles::Billboard want = particles::gpu_billboard(fx.emitter(), state[i], math::Mat4::identity(), view);
        worst_aspect = std::max(worst_aspect, std::abs(drawn.value()[i].params.w - want.aspect));
        const particles::Billboard unscaled = particles::gpu_billboard(plain, state[i], math::Mat4::identity(), view);
        slowed += unscaled.aspect - want.aspect;
        ++compared;
    }
    MESSAGE("speed/spin curves: " << compared << " particles, worst position error " << worst << " m, rotation " << worst_rot
                                  << " rad, stretch " << worst_aspect << "; stretch reduced by " << slowed);
    CHECK(compared > 80);
    CHECK(worst < 5e-3f);
    CHECK(worst_rot < 2e-3f);
    CHECK(worst_aspect < 1e-3f);
    CHECK(slowed > 0);
    dev.destroy(target);
}
