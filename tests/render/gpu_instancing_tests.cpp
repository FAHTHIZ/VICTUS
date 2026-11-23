// SPDX-License-Identifier: Apache-2.0
// GPU instancing: identical mesh+material draws merge into instanced batches in every pass, the
// image is unchanged versus one draw per object, and differing materials/meshes stay separate.
#include "gpu_fixture.hpp"
#include "victus/core/jobs.hpp"
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

TEST_CASE("gpu instancing: parallel per-draw visibility matches the serial pass exactly") {
    // ADR-0228: classification on the job system, lists and history built serially in order.
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    MeshData data = make_uv_sphere(0.5f, 24, 12);
    REQUIRE(generate_lods(data) >= 1);
    auto sphere = ctx.create_mesh(data, "parallel_vis_sphere").value();
    auto plane = ctx.create_mesh(make_plane({200, 200}), "parallel_vis_plane").value();
    Material opaque, glass, caster;
    opaque.base_color = {0.7f, 0.6f, 0.5f, 1};
    glass.base_color = {0.2f, 0.4f, 0.9f, 0.5f};
    glass.blend = MaterialBlend::Alpha;
    caster.shadow_only = true;
    constexpr int kSide = 48; // 2,304 draws: above the parallel threshold
    constexpr std::uint32_t W = 128, H = 72;
    jobs::JobSystem workers(2);
    const auto run = [&](jobs::JobSystem* js) {
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        SceneRenderer& r = *sr.value();
        r.set_job_system(js);
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = false;
        r.set_settings(s);
        auto target = test::make_target(W, H);
        std::vector<std::vector<std::uint8_t>> frames;
        std::vector<SceneStats> stats;
        for (int f = 0; f < 3; ++f) {
            Camera3D cam = Camera3D::look_at({0, 12.0f - 3.0f * static_cast<float>(f), 60.0f - 18.0f * static_cast<float>(f)}, {0, 0, 0});
            cam.z_far = 400;
            DirectionalLight sun;
            sun.shadow_distance = 80;
            r.begin(cam, sun, {});
            r.submit(plane, {}, math::Mat4::identity());
            for (int z = 0; z < kSide; ++z)
                for (int x = 0; x < kSide; ++x) {
                    const int i = z * kSide + x;
                    const Material& m = i % 11 == 0 ? caster : i % 7 == 0 ? glass : opaque;
                    r.submit(sphere, m,
                             math::Mat4::translation({(static_cast<float>(x) - kSide / 2.0f) * 2.0f, 0.5f,
                                                      (static_cast<float>(z) - kSide / 2.0f) * 2.0f}),
                             static_cast<std::uint64_t>(i + 1));
                }
            auto cmd = dev.begin_commands();
            REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
            frames.push_back(dev.read_texture(target).value());
            stats.push_back(r.stats());
        }
        dev.destroy(target);
        return std::pair{frames, stats};
    };
    const auto serial = run(nullptr);
    const auto parallel = run(&workers);
    for (std::size_t f = 0; f < serial.first.size(); ++f) {
        CAPTURE(f);
        const SceneStats& a = serial.second[f];
        const SceneStats& b = parallel.second[f];
        CHECK(a.visible == b.visible);
        CHECK(a.culled == b.culled);
        CHECK(a.triangles == b.triangles);
        CHECK(a.lod_draws == b.lod_draws);
        CHECK(a.lod_held == b.lod_held);
        CHECK(a.lod_fading == b.lod_fading);
        CHECK(a.shadow_casters == b.shadow_casters);
        CHECK(a.shadow_receiver_culled == b.shadow_receiver_culled);
        CHECK(a.main_batches == b.main_batches);
        CHECK(a.shadow_batches == b.shadow_batches);
        CHECK(a.transparent_triangles == b.transparent_triangles);
        CHECK(max_diff(serial.first[f], parallel.first[f]) == 0);
    }
    CHECK(serial.second[0].visible > 1000);
    CHECK(serial.second[2].lod_draws > 0);
    CHECK(serial.second[0].culled + serial.second[2].culled > 0);
    ctx.destroy(sphere);
    ctx.destroy(plane);
}
