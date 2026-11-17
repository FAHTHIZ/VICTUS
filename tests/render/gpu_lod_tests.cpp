// SPDX-License-Identifier: Apache-2.0
// Mesh LOD selection (Slice 54, ADR-0049): the renderer picks coarser levels as objects shrink on
// screen, keeps the image within the pixel error budget, batches per level and honours force_lod.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cstdlib>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename F>
Shot shoot(SceneRenderer& r, RenderSettings s, math::Vec3 eye, F&& content, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    constexpr std::uint32_t W = 160, H = 96;
    auto target = test::make_target(W, H);
    s.taa = false;
    r.set_settings(s);
    Camera3D cam = Camera3D::look_at(eye, {0, 0, 0});
    DirectionalLight sun;
    sun.shadow_distance = 40;
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

double mean_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    double sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
    return sum / static_cast<double>(a.size());
}

} // namespace

TEST_CASE("gpu lods: coarser levels at a distance, unchanged image, per-level batches") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_uv_sphere(0.5f, 64, 32);
    const std::size_t full = data.triangle_count();
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    const std::size_t coarsest = data.lods.back().indices.size() / 3;
    auto sphere = ctx.create_mesh(data, "lod_sphere").value();
    REQUIRE(sphere.lod_count == 4);
    CHECK(sphere.lods[0].index_count == full * 3);
    CHECK(sphere.lods[3].index_count == coarsest * 3);
    CHECK(sphere.lods[1].first_index == full * 3);
    CHECK(sphere.lods[3].error >= sphere.lods[1].error);

    Material mat;
    mat.base_color = {0.2f, 0.6f, 0.9f, 1};
    auto one = [&](SceneRenderer& r) { r.submit(sphere, mat, math::Mat4::identity()); };
    const RenderSettings high = RenderSettings::preset(QualityTier::High);

    // Close up with a strict budget: the full mesh. (The simplifier is accurate enough that the
    // 1 px default already allows LOD 1-2 here at 160x96.)
    RenderSettings strict = high;
    strict.lod_error_pixels = 0.25f;
    const math::Vec3 near_eye{0, 0.3f, 1.2f};
    const Shot near_shot = shoot(*sr.value(), strict, near_eye, one, "lod_near");
    CHECK(near_shot.stats.visible == 1);
    CHECK(near_shot.stats.lod_draws == 0);
    CHECK(near_shot.stats.triangles == full);

    // Far away (a few pixels wide): the coarsest level, and the image matches the full mesh.
    const math::Vec3 far_eye{0, 4, 40};
    const Shot far_shot = shoot(*sr.value(), high, far_eye, one, "lod_far");
    CHECK(far_shot.stats.lod_draws == 1);
    CHECK(far_shot.stats.triangles == coarsest);
    RenderSettings off = high;
    off.lods = false;
    const Shot far_full = shoot(*sr.value(), off, far_eye, one);
    CHECK(far_full.stats.lod_draws == 0);
    CHECK(far_full.stats.triangles == full);
    CHECK(mean_diff(far_shot.px, far_full.px) < 0.5);

    // force_lod draws the requested level (clamped to the mesh's last level) even up close.
    RenderSettings forced = high;
    forced.force_lod = 1;
    const Shot f1 = shoot(*sr.value(), forced, near_eye, one);
    CHECK(f1.stats.triangles == data.lods[0].indices.size() / 3);
    forced.force_lod = 7;
    const Shot f7 = shoot(*sr.value(), forced, near_eye, one);
    CHECK(f7.stats.triangles == coarsest);

    // A larger pixel budget switches earlier; a stricter one keeps more detail.
    const math::Vec3 mid_eye{0, 0.3f, 2};
    RenderSettings loose = high;
    loose.lod_error_pixels = 16.0f;
    const Shot l = shoot(*sr.value(), loose, mid_eye, one);
    const Shot s = shoot(*sr.value(), strict, mid_eye, one);
    CHECK(l.stats.triangles < s.stats.triangles);

    // Instancing groups by level: a near and a far copy are two batches, two far copies one.
    RenderSettings inst = strict;
    inst.instancing = true;
    auto pair = [&](float z0, float z1) {
        return [&, z0, z1](SceneRenderer& r) {
            r.submit(sphere, mat, math::Mat4::translation({-0.6f, 0, z0}));
            r.submit(sphere, mat, math::Mat4::translation({0.6f, 0, z1}));
        };
    };
    const Shot mixed = shoot(*sr.value(), inst, {0, 0.5f, 3}, pair(1.8f, -30.0f));
    CHECK(mixed.stats.visible == 2);
    CHECK(mixed.stats.lod_draws == 1);
    CHECK(mixed.stats.main_batches == 2);
    const Shot same = shoot(*sr.value(), inst, {0, 0.5f, 3}, pair(-30.0f, -30.0f));
    CHECK(same.stats.lod_draws == 2);
    CHECK(same.stats.main_batches == 1);
    CHECK(same.stats.triangles == 2 * coarsest);

    // Meshes without LODs are unaffected.
    auto cube = ctx.create_mesh(make_cube(), "lod_cube").value();
    CHECK(cube.lod_count == 1);
    const Shot c = shoot(*sr.value(), high, far_eye, [&](SceneRenderer& r) { r.submit(cube, mat, math::Mat4::identity()); });
    CHECK(c.stats.lod_draws == 0);
    CHECK(c.stats.triangles == 12);
    ctx.destroy(sphere);
    ctx.destroy(cube);
}
