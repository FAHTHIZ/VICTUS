// SPDX-License-Identifier: Apache-2.0
// Distance fog on particles and precipitation (ADR-0116): alpha billboards take on the fog colour
// with distance exactly like lit surfaces, additive billboards are dimmed by the same amount, the
// fog follows the range along the pixel's ray (not just the view depth), and rain is tinted.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

struct Shot {
    std::vector<std::uint8_t> px;
    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return test::pixel(px, W, x, y); }
    [[nodiscard]] double mean(int c) const {
        double s = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) s += channel(at(x, y), c);
        return s / (W * H);
    }
};

Environment fog_env(FogMode mode) {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {1, 1, 1, 1};
    env.ambient_intensity = 1.0f;
    env.vignette = 0;
    env.tonemap = false;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.fog_mode = mode;
    env.fog_color = {1, 0, 0, 1}; // red fog against white particles
    env.fog_start = 0.0f;
    env.fog_end = 20.0f;
    env.fog_density = 0.1f;
    return env;
}

template <typename F>
Shot shoot(SceneRenderer& r, const Environment& env, F&& content, Camera3D cam = Camera3D::look_at({0, 0, 0}, {0, 0, -1}),
           const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.bloom = s.fxaa = s.taa = s.ssao = s.auto_exposure = false;
    r.set_settings(s);
    r.set_time(1.0);
    DirectionalLight sun;
    sun.intensity = 0;
    sun.shadows = false;
    r.begin(cam, sun, env);
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

// One white billboard `dist` metres down the view axis, sized to fill the middle of the view.
auto billboard(float dist, ParticleBlend blend, math::Vec3 side = {}) {
    return [=](SceneRenderer& r) {
        ParticleInstance p{{side.x, side.y, -dist + side.z, dist * 0.6f}, {1, 1, 1, 1}, {0, 0, 0, 0}};
        ParticleMaterial m;
        m.blend = blend;
        m.softness = 0.01f;
        const float h = dist * 0.3f;
        r.submit_particles(std::span(&p, 1), m, math::Aabb{{side.x - h, side.y - h, -dist + side.z - h}, {side.x + h, side.y + h, -dist + side.z + h}});
    };
}

} // namespace

TEST_CASE("gpu effect fog: alpha particles fade to the fog colour, additive ones dim") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    const Environment clear = fog_env(FogMode::None), fog = fog_env(FogMode::Linear);
    const auto mid = [](const Shot& s) { return s.at(W / 2, H / 2); };

    // Alpha particle 15 m away: 75% of the way into linear fog (0..20 m).
    const Shot a0 = shoot(r, clear, billboard(15, ParticleBlend::Alpha));
    const Shot a1 = shoot(r, fog, billboard(15, ParticleBlend::Alpha), Camera3D::look_at({0, 0, 0}, {0, 0, -1}), "effect_fog_alpha");
    CHECK(channel(mid(a0), 0) > 200);
    CHECK(std::abs(channel(mid(a0), 0) - channel(mid(a0), 1)) <= 2); // white without fog
    CHECK(channel(mid(a1), 0) > 200);                                // fog is red, particle stays bright in red
    CHECK(channel(mid(a1), 1) * 10 < channel(mid(a0), 1) * 6);       // green falls to 25% linear (54% in sRGB)
    // Nearer is clearer.
    const Shot near = shoot(r, fog, billboard(2, ParticleBlend::Alpha));
    CHECK(channel(mid(near), 1) > channel(mid(a1), 1) + 60);

    // Additive white: all channels dim by the fog amount; red does not rise above the clear value.
    const Shot d0 = shoot(r, clear, billboard(15, ParticleBlend::Additive));
    const Shot d1 = shoot(r, fog, billboard(15, ParticleBlend::Additive));
    CHECK(channel(mid(d1), 1) * 10 < channel(mid(d0), 1) * 6);
    CHECK(channel(mid(d1), 0) * 10 < channel(mid(d0), 0) * 6);

    // Exponential fog works too.
    const Shot e = shoot(r, fog_env(FogMode::Exponential), billboard(15, ParticleBlend::Alpha));
    CHECK(channel(mid(e), 1) < channel(mid(a0), 1) - 40);

    // The fog uses the range along the ray: the same view depth off-axis is farther, so foggier.
    Environment mild = fog;
    mild.fog_end = 30.0f;
    Camera3D wide = Camera3D::look_at({0, 0, 0}, {0, 0, -1});
    wide.fov_y = 1.6f;
    const Shot centre = shoot(r, mild, billboard(10, ParticleBlend::Alpha, {0, 0, 0}), wide);
    const Shot edge = shoot(r, mild, billboard(10, ParticleBlend::Alpha, {9, 0, 0}), wide);
    int centre_g = 0, edge_g = 0;
    for (std::uint32_t x = 0; x < W; ++x) {
        centre_g = std::max(centre_g, int{channel(centre.at(x, H / 2), 1)});
        edge_g = std::max(edge_g, int{channel(edge.at(x, H / 2), 1)});
    }
    CHECK(edge_g + 3 < centre_g);
}

TEST_CASE("gpu effect fog: rain is tinted by distance fog") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    Environment clear = fog_env(FogMode::None);
    clear.precipitation.type = PrecipitationType::Snow;
    clear.precipitation.radius = 10.0f;
    Environment fog = clear;
    fog.fog_mode = FogMode::Linear;
    fog.fog_end = 8.0f;
    const auto nothing = [](SceneRenderer&) {};
    const Shot s0 = shoot(r, clear, nothing);
    const Shot s1 = shoot(r, fog, nothing, Camera3D::look_at({0, 0, 0}, {0, 0, -1}), "effect_fog_snow");
    CHECK(std::abs(s0.mean(0) - s0.mean(1)) < 0.5);
    CHECK(s1.mean(0) > s1.mean(1) * 1.5);
    CHECK(s1.mean(1) < s0.mean(1));
}
