// SPDX-License-Identifier: Apache-2.0
// Mesh particles (ADR-0215): a mesh emitter's particles draw as lit, opaque, shadow-casting mesh
// instances through runtime extraction (built-in primitive and default material tinted by
// color_start), with the cast-shadow option honoured.
#include "gpu_fixture.hpp"
#include "victus/particles/particles.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
    render::SceneStats render_stats;
};

// Screen pixel of a world point for `cam` (y down).
std::pair<std::uint32_t, std::uint32_t> project(const render::Camera3D& cam, math::Vec3 p) {
    const math::Vec4 c = cam.projection(static_cast<float>(W) / static_cast<float>(H)) * cam.view * math::Vec4(p, 1.0f);
    const float x = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(W), y = (0.5f - c.y / c.w * 0.5f) * static_cast<float>(H);
    return {static_cast<std::uint32_t>(std::clamp(x, 0.0f, W - 1.0f)), static_cast<std::uint32_t>(std::clamp(y, 0.0f, H - 1.0f))};
}

} // namespace

TEST_CASE("gpu mesh particles: emitter meshes draw lit and opaque and cast sun shadows through runtime extraction") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    particles::register_components();
    scene::Scene s;
    const scene::Entity floor = s.create("Floor");
    auto& mr = s.world().add<scene::MeshRendererComponent>(floor);
    mr.primitive = "plane";
    mr.base_color = {0.6f, 0.6f, 0.6f, 1};
    s.world().get<scene::Transform>(floor)->scale = {12, 1, 12};
    const scene::Entity sun = s.create("Sun");
    s.world().add<scene::LightComponent>(sun).intensity = 3.0f;
    s.world().get<scene::Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, -math::kPi * 0.5f); // straight down
    const scene::Entity fx = s.create("Debris");
    s.world().get<scene::Transform>(fx)->position = {0, 1, 0};
    auto& c = s.world().add<particles::ParticleEmitterComponent>(fx);
    c.rate = 0;
    c.burst = 1;
    c.lifetime_min = c.lifetime_max = 10;
    c.shape = particles::Shape::Point;
    c.speed_min = c.speed_max = 0;
    c.size_start = c.size_end = 0.8f;
    c.color_start = {0.9f, 0.1f, 0.05f, 1};
    c.seed = 11;
    c.mesh_primitive = "cube";
    particles::ParticleSystem ps;
    ps.update(s, 0.0f);
    s.update_transforms();

    const render::Camera3D cam = render::Camera3D::look_at({0, 3, 5}, {0, 0.5f, 0});
    runtime::GpuAssetCache assets(ctx, nullptr);
    auto sr = render::SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    render::SceneRenderer& r = *sr.value();
    render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::High);
    rs.bloom = rs.taa = rs.fxaa = rs.auto_exposure = rs.ssao = false;
    r.set_settings(rs);
    const auto shoot = [&](const char* save) {
        runtime::SceneViewSettings sv;
        sv.environment.draw_sky = false;
        sv.environment.clear_color = {0, 0, 0, 1};
        Frame f;
        f.stats = runtime::extract_scene(s, assets, r, sv, &cam);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        f.px = dev.read_texture(target).value();
        f.render_stats = r.stats();
        dev.destroy(target);
        if (save) test::save_output(save, W, H, f.px);
        return f;
    };
    const Frame on = shoot("mesh_particles");
    CHECK(on.stats.particle_meshes == 1);
    CHECK(on.stats.particles == 1);
    CHECK(on.stats.particle_emitters == 1);
    CHECK(on.render_stats.particles == 0); // no billboards
    // The cube: red, lit (sun from above, ambient on the sides).
    const auto [cx, cy] = project(cam, {0, 1, 0});
    const std::uint32_t cube = test::pixel(on.px, W, cx, cy);
    CHECK(channel(cube, 0) > 60);
    CHECK(channel(cube, 0) > channel(cube, 1) + 40);
    // Its shadow on the floor straight below, against lit floor beside it.
    const auto [sx, sy] = project(cam, {0, 0, 0.0f});
    const auto [lx, ly] = project(cam, {1.6f, 0, 0.0f});
    const int shadow = channel(test::pixel(on.px, W, sx, sy), 1), lit = channel(test::pixel(on.px, W, lx, ly), 1);
    MESSAGE("floor green: under the cube " << shadow << ", beside it " << lit);
    CHECK(lit > shadow + 30);

    // mesh_cast_shadows off: the floor under the cube is lit like beside it.
    s.world().get<particles::ParticleEmitterComponent>(fx)->mesh_cast_shadows = false;
    const Frame off = shoot("mesh_particles_no_shadow");
    const int unshadowed = channel(test::pixel(off.px, W, sx, sy), 1);
    CHECK(std::abs(unshadowed - lit) < 12);
}
