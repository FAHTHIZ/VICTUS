// SPDX-License-Identifier: Apache-2.0
// Object-space triplanar mapping (ADR-0246): the projection follows the object, so moving and
// turning an object together with the camera (and the sun) reproduces the same image, while
// world-space triplanar slides the texture over it. Covers the base colour and the normal map.
#include "gpu_fixture.hpp"
#include "victus/render/image.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 128, H = 96;

RenderSettings plain_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = s.fxaa = s.ssao = s.bloom = s.auto_exposure = false;
    s.render_scale = 1.0f;
    s.msaa_samples = 1;
    return s;
}

Environment dark_env() {
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    env.ambient_intensity = 0.0f; // the hemisphere ambient is fixed to the world's up axis
    return env;
}

// A rigid motion applied to the object, the camera and the sun alike.
struct Pose {
    math::Mat4 m;
    math::Vec3 point(math::Vec3 p) const { return m.transform_point(p); }
    math::Vec3 dir(math::Vec3 v) const { return m.transform_vector(v); }
};

std::vector<std::uint8_t> draw(SceneRenderer& r, const GpuMesh& cube, const Material& mat, const Pose& pose, bool lit) {
    auto& dev = gpu().ctx->device();
    const Camera3D cam = Camera3D::look_at(pose.point({1.6f, 1.3f, 2.6f}), pose.point({0, 0, 0}), pose.dir({0, 1, 0}));
    DirectionalLight sun;
    sun.direction = pose.dir({-0.5f, -0.7f, -0.4f});
    sun.intensity = lit ? 3.0f : 0.0f;
    sun.shadows = false;
    r.begin(cam, sun, dark_env());
    r.submit(cube, mat, pose.m * math::Mat4::rotation(math::Quat::axis_angle({0, 1, 0}, 0.4f)));
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    return px;
}

double mean_abs_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) sum += std::abs(channel(pixel(a, W, x, y), c) - channel(pixel(b, W, x, y), c));
    return sum / (W * H * 3);
}
} // namespace

TEST_CASE("gpu: object-space triplanar textures and normal maps move with the object (ADR-0246)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto cube = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.5f}), "triplanar_cube").value();
    auto checker = ctx.create_texture(make_checker(256, 8, 0xff2020e0u, 0xff20d020u)).value();
    TextureOptions linear;
    linear.srgb = false;
    // Two tilted normals in a checker: the lighting shows the normal map's frame.
    auto bumps = ctx.create_texture(make_checker(256, 8, 0xfff7bf80u, 0xfff780bfu), linear).value();
    auto r = SceneRenderer::create(ctx).value();
    r->set_settings(plain_settings());

    const Pose rest{math::Mat4::identity()};
    const Pose moved{math::Mat4::translation({2.3f, 0.37f, -1.1f}) * math::Mat4::rotation(math::Quat::axis_angle({0.3f, 1, 0.2f}, 1.1f))};
    Material m;
    m.triplanar = true;
    m.uv_scale = {0.7f, 0.7f}; // repeats per metre; not a whole number of repeats per move
    m.base_color_texture = checker;

    double unlit_diff[2]{}, lit_diff[2]{};
    for (int object = 0; object < 2; ++object) {
        m.triplanar_object = object != 0;
        m.unlit = true;
        m.normal_texture = {};
        const auto a = draw(*r, cube, m, rest, false);
        const auto b = draw(*r, cube, m, moved, false);
        unlit_diff[object] = mean_abs_diff(a, b);
        if (object) test::save_output("triplanar_object_moved", W, H, b);
        m.unlit = false;
        m.normal_texture = bumps;
        const auto la = draw(*r, cube, m, rest, true);
        const auto lb = draw(*r, cube, m, moved, true);
        lit_diff[object] = mean_abs_diff(la, lb);
        if (object) {
            test::save_output("triplanar_object_lit", W, H, lb);
            Material flat = m;
            flat.normal_texture = {};
            const double bump_effect = mean_abs_diff(la, draw(*r, cube, flat, rest, true));
            MESSAGE("normal map changes the lit image by " << bump_effect);
            // Whole-image means: the cube covers roughly a seventh of the 128x128 frame.
            CHECK(bump_effect > 0.8); // the normal map is really in play (measured 1.25)
        }
    }
    MESSAGE("mean |rest - moved| unlit: world " << unlit_diff[0] << ", object " << unlit_diff[1]);
    MESSAGE("mean |rest - moved| lit, normal-mapped: world " << lit_diff[0] << ", object " << lit_diff[1]);
    CHECK(unlit_diff[0] > 2.5);  // world space: the checker slides over the cube (measured 3.84)
    CHECK(unlit_diff[1] < 0.75); // object space: the same image up to rasterisation
    CHECK(lit_diff[0] > 2.0);   // measured 3.42
    CHECK(lit_diff[1] < 0.75); // object space measured 0 in both cases
    r.reset();
    ctx.device().destroy(checker);
    ctx.device().destroy(bumps);
    ctx.destroy(cube);
}
