// SPDX-License-Identifier: Apache-2.0
// GPU instancing: identical mesh+material draws merge into instanced batches in every pass, the
// image is unchanged versus one draw per object, and differing materials/meshes stay separate.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cstdlib>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename F>
Shot shoot(SceneRenderer& r, bool instancing, F&& content, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    constexpr std::uint32_t W = 160, H = 96;
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.ssao = true; // exercises the depth pre-pass with instances too
    s.instancing = instancing;
    s.taa = false; // compares exact frames; jitter would differ per frame
    r.set_settings(s);
    Camera3D cam = Camera3D::look_at({0, 9, 14}, {0, 0, 0});
    DirectionalLight sun;
    sun.shadow_distance = 30;
    r.begin(cam, sun, {});
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

int max_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

} // namespace

TEST_CASE("gpu instancing: identical objects become one batch per pass with an unchanged image") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube(), "inst_cube").value();
    auto plane = ctx.create_mesh(make_plane({40, 40}), "inst_plane").value();
    Material red;
    red.base_color = {0.8f, 0.1f, 0.1f, 1};
    auto grid = [&](SceneRenderer& r) {
        r.submit(plane, {}, math::Mat4::identity());
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x)
                r.submit(cube, red,
                         math::Mat4::translation({(static_cast<float>(x) - 3.5f) * 1.6f, 0.5f, (static_cast<float>(z) - 3.5f) * 1.6f}) *
                             math::Mat4::rotation(math::Quat::axis_angle({0, 1, 0}, 0.2f * static_cast<float>(x + z))));
    };
    const Shot on = shoot(*sr.value(), true, grid, "instancing_on");
    const Shot off = shoot(*sr.value(), false, grid, "instancing_off");
    CHECK(on.stats.visible == off.stats.visible);
    CHECK(on.stats.visible >= 60);
    CHECK(on.stats.main_batches == 2);                     // plane + all cubes
    CHECK(off.stats.main_batches == on.stats.visible);     // one draw per object
    CHECK(on.stats.prepass_batches == 2);
    CHECK(on.stats.prepass_draws == on.stats.visible);
    CHECK(on.stats.shadow_batches <= 2 * on.stats.shadow_cascades);
    CHECK(off.stats.shadow_batches == off.stats.shadow_draws);
    CHECK(on.stats.instances_uploaded == on.stats.visible + on.stats.shadow_draws);
    // Instancing changes how draws are issued, not what is drawn.
    CHECK(max_diff(on.px, off.px) <= 1);
    // The cubes are actually there: a large share of the frame is red-dominant.
    int red_px = 0;
    for (std::uint32_t y = 0; y < 96; ++y)
        for (std::uint32_t x = 0; x < 160; ++x) {
            const auto c = test::pixel(on.px, 160, x, y);
            red_px += channel(c, 0) > channel(c, 1) + 40 ? 1 : 0;
        }
    CHECK(red_px > 160 * 96 / 10);
    ctx.destroy(cube);
    ctx.destroy(plane);
}

TEST_CASE("gpu instancing: submit_instances matches one submit per copy") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube(), "inst_cube_span").value();
    Material green;
    green.base_color = {0.1f, 0.8f, 0.2f, 1};
    std::vector<math::Mat4> models;
    for (int z = 0; z < 10; ++z)
        for (int x = 0; x < 10; ++x)
            models.push_back(math::Mat4::translation({(static_cast<float>(x) - 4.5f) * 1.1f, 0.3f, (static_cast<float>(z) - 4.5f) * 1.1f}) *
                             math::Mat4::scale({0.5f, 0.5f, 0.5f}));
    const Shot each = shoot(*sr.value(), true, [&](SceneRenderer& r) {
        for (const auto& m : models) r.submit(cube, green, m);
    });
    const Shot span = shoot(*sr.value(), true, [&](SceneRenderer& r) { r.submit_instances(cube, green, models); });
    CHECK(span.stats.submitted == 100);
    CHECK(span.stats.visible == each.stats.visible);
    CHECK(span.stats.main_batches == each.stats.main_batches);
    CHECK(span.stats.main_batches == 1);
    CHECK(span.stats.shadow_batches == each.stats.shadow_batches);
    CHECK(max_diff(span.px, each.px) == 0);
    // An empty span or invalid mesh submits nothing.
    const Shot none = shoot(*sr.value(), true, [&](SceneRenderer& r) { r.submit_instances(cube, green, {}); });
    CHECK(none.stats.submitted == 0);
    ctx.destroy(cube);
}

TEST_CASE("gpu instancing: different materials or meshes never share a batch") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube(), "inst_cube2").value();
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 16, 12), "inst_sphere").value();
    Material a, b;
    a.base_color = {0.9f, 0.2f, 0.2f, 1};
    b.base_color = {0.2f, 0.2f, 0.9f, 1};
    Material a_rough = a;
    a_rough.roughness = 0.9f;
    Material a_coat = a; // differs only in the clear coat layer (interned and batched apart)
    a_coat.clearcoat = 0.8f;
    const Shot s = shoot(*sr.value(), true, [&](SceneRenderer& r) {
        for (int i = 0; i < 4; ++i) {
            const float x = static_cast<float>(i) * 1.5f - 6.0f;
            r.submit(cube, a, math::Mat4::translation({x, 0.5f, -2}));       // batch 1
            r.submit(cube, b, math::Mat4::translation({x, 0.5f, 0}));        // batch 2
            r.submit(sphere, a, math::Mat4::translation({x, 0.5f, 2}));      // batch 3
            r.submit(cube, a_rough, math::Mat4::translation({x + 6, 0.5f, 0})); // batch 4
            r.submit(cube, a_coat, math::Mat4::translation({x + 6, 0.5f, 2}));  // batch 5
        }
    });
    CHECK(s.stats.visible == 20);
    CHECK(s.stats.main_batches == 5);
    // Shadows ignore materials: cubes (any material) and spheres, per cascade.
    CHECK(s.stats.shadow_batches <= 2 * s.stats.shadow_cascades);
    ctx.destroy(cube);
    ctx.destroy(sphere);
}

TEST_CASE("gpu instancing: instance buffer grows across frames and stays correct") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube(), "inst_cube3").value();
    for (int n : {1, 300, 1200, 5}) {
        const Shot s = shoot(*sr.value(), true, [&](SceneRenderer& r) {
            for (int i = 0; i < n; ++i) {
                const float x = static_cast<float>(i % 40) * 0.3f - 6.0f;
                const float z = static_cast<float>(i / 40) * 0.3f - 4.0f;
                r.submit(cube, {}, math::Mat4::translation({x, 0.1f, z}) * math::Mat4::scale({0.2f, 0.2f, 0.2f}));
            }
        });
        CHECK(s.stats.main_batches == 1);
        CHECK(s.stats.visible == static_cast<std::uint32_t>(n));
    }
    ctx.destroy(cube);
}
