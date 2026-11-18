// SPDX-License-Identifier: Apache-2.0
// Mesh LOD selection (Slice 54, ADR-0049): the renderer picks coarser levels as objects shrink on
// screen, keeps the image within the pixel error budget, batches per level and honours force_lod.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>
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

namespace {

// One frame of `content` with an explicit vertical FOV and target size (Workstream H1/H3:
// camera-responsive LOD). Returns the frame's stats only.
template <typename F>
SceneStats view_stats(SceneRenderer& r, RenderSettings s, math::Vec3 eye, float fov_deg, std::uint32_t w,
                      std::uint32_t h, F&& content) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(w, h);
    s.taa = false;
    r.set_settings(s);
    Camera3D cam = Camera3D::look_at(eye, {0, 0, 0});
    cam.fov_y = math::radians(fov_deg);
    r.begin(cam, DirectionalLight{}, {});
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, w, h, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    const SceneStats out = r.stats();
    dev.destroy(target);
    return out;
}

} // namespace

TEST_CASE("gpu lods: selection follows field of view, viewport height and render scale") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_uv_sphere(0.5f, 64, 32);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    const std::size_t full = data.triangle_count();
    auto sphere = ctx.create_mesh(data, "lod_fov_sphere").value();
    Material mat;
    auto one = [&](SceneRenderer& r) { r.submit(sphere, mat, math::Mat4::identity()); };
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.lod_error_pixels = 0.25f; // strict, so a 3-level span is reachable at 96 px
    SceneRenderer& r = *sr.value();

    // Same camera position, narrowing FOV (zoom in): never coarser, and a strong zoom is finer.
    const math::Vec3 eye{0, 0, 12};
    std::uint64_t prev = 0;
    std::uint64_t wide = 0, narrow = 0;
    for (float fov : {100.0f, 60.0f, 30.0f, 10.0f, 3.0f}) {
        const SceneStats st = view_stats(r, s, eye, fov, 160, 96, one);
        CHECK(st.visible == 1);
        CHECK(st.triangles >= prev);
        prev = st.triangles;
        if (fov == 100.0f) wide = st.triangles;
        if (fov == 3.0f) narrow = st.triangles;
    }
    CHECK(wide < narrow);
    CHECK(narrow == full);

    // Dolly vs zoom: equal projected size (distance to the bounds / tan(fov/2) held constant)
    // selects the same level, so zooming is not penalised relative to walking closer.
    const float d1 = 6.0f, fov1 = 60.0f;
    const float d2 = 18.0f;
    const float fov2 = math::degrees(2.0f * std::atan(std::tan(math::radians(fov1) * 0.5f) * d1 / d2));
    const SceneStats dolly = view_stats(r, s, {0, 0, d1 + 0.5f}, fov1, 160, 96, one);
    const SceneStats zoom = view_stats(r, s, {0, 0, d2 + 0.5f}, fov2, 160, 96, one);
    CHECK(dolly.triangles == zoom.triangles);

    // Taller targets resolve more detail at the same camera; the budget is in render pixels.
    const SceneStats small = view_stats(r, s, {0, 0, 8}, 60.0f, 160, 96, one);
    const SceneStats large = view_stats(r, s, {0, 0, 8}, 60.0f, 640, 384, one);
    CHECK(small.triangles <= large.triangles);
    CHECK(small.triangles < large.triangles);

    // Render scale (dynamic resolution) lowers the level, and restoring it recovers the detail.
    RenderSettings half = s;
    half.render_scale = 0.25f;
    const SceneStats full_scale = view_stats(r, s, {0, 0, 8}, 60.0f, 640, 384, one);
    const SceneStats quarter = view_stats(r, half, {0, 0, 8}, 60.0f, 640, 384, one);
    const SceneStats restored = view_stats(r, s, {0, 0, 8}, 60.0f, 640, 384, one);
    CHECK(quarter.triangles < full_scale.triangles);
    CHECK(restored.triangles == full_scale.triangles);

    // Rapid zoom cycles are stateless: returning to the original FOV gives the original level.
    const SceneStats before = view_stats(r, s, eye, 45.0f, 160, 96, one);
    for (int i = 0; i < 6; ++i) (void)view_stats(r, s, eye, i % 2 ? 2.0f : 120.0f, 160, 96, one);
    const SceneStats after = view_stats(r, s, eye, 45.0f, 160, 96, one);
    CHECK(before.triangles == after.triangles);
    ctx.destroy(sphere);
}

TEST_CASE("gpu lods: casters outside the main view still shadow it (H3 off-screen contribution)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto ground = ctx.create_mesh(make_plane({20, 20}), "h3_ground").value();
    MeshData box_data = make_uv_sphere(1.0f, 48, 24);
    REQUIRE(generate_lods(box_data) == 3);
    auto blocker = ctx.create_mesh(box_data, "h3_blocker").value();
    Material mat;
    mat.base_color = {0.8f, 0.8f, 0.8f, 1};
    mat.cast_shadows = true;
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.ssao = false;
    s.bloom = false;
    s.auto_exposure = false;
    sr.value()->set_settings(s);

    constexpr std::uint32_t W = 160, H = 96;
    // The camera looks at the origin with a narrow FOV; the blocker floats far to its right,
    // outside the frustum, and the low sun throws its shadow onto the middle of the view.
    auto frame = [&](bool with_blocker, SceneStats* st) {
        auto target = test::make_target(W, H);
        Camera3D cam = Camera3D::look_at({0, 2, 6}, {0, 0, 0});
        cam.fov_y = math::radians(30.0f);
        DirectionalLight sun;
        sun.direction = {-2.0f, -1.0f, 0.0f};
        sun.shadow_distance = 30;
        sr.value()->begin(cam, sun, {});
        sr.value()->submit(ground, mat, math::Mat4::identity());
        if (with_blocker) sr.value()->submit(blocker, mat, math::Mat4::translation({6, 3, 0}));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        if (st) *st = sr.value()->stats();
        dev.destroy(target);
        return px;
    };
    SceneStats st;
    const auto lit = frame(false, nullptr);
    const auto shadowed = frame(true, &st);
    test::save_output("h3_offscreen_caster", W, H, shadowed);
    CHECK(st.visible == 1); // only the ground is in the main view
    CHECK(st.shadow_casters == 2);
    // Mean luminance of the centre 32x16 pixels drops with the off-screen caster present.
    auto centre = [&](const std::vector<std::uint8_t>& px) {
        double sum = 0;
        for (std::uint32_t y = H / 2 - 8; y < H / 2 + 8; ++y)
            for (std::uint32_t x = W / 2 - 16; x < W / 2 + 16; ++x) sum += px[(y * W + x) * 4 + 1];
        return sum / (32.0 * 16.0);
    };
    CHECK(centre(shadowed) < centre(lit) * 0.8);
    ctx.destroy(ground);
    ctx.destroy(blocker);
}

TEST_CASE("gpu lods: shadow_lod_bias draws coarser casters without changing the main view") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto ground = ctx.create_mesh(make_plane({20, 20}), "bias_ground").value();
    MeshData data = make_uv_sphere(0.6f, 64, 32);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    auto sphere = ctx.create_mesh(data, "bias_sphere").value();
    Material mat;
    mat.cast_shadows = true;
    constexpr std::uint32_t W = 160, H = 96;
    auto frame = [&](RenderSettings s, SceneStats& st) {
        s.taa = false;
        s.ssao = false;
        s.lod_error_pixels = 0.25f;
        sr.value()->set_settings(s);
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = {-1.0f, -1.0f, -0.3f};
        sr.value()->begin(Camera3D::look_at({0, 1.5f, 3}, {0, 0.3f, 0}), sun, {});
        sr.value()->submit(ground, mat, math::Mat4::identity());
        for (float x : {-1.0f, 1.0f}) sr.value()->submit(sphere, mat, math::Mat4::translation({x, 0.6f, 0}));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        st = sr.value()->stats();
        dev.destroy(target);
        return px;
    };
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    CHECK(s.shadow_lod_bias == 0);
    CHECK(RenderSettings::preset(QualityTier::Low).shadow_lod_bias == 1);
    s.shadow_lod_bias = 9;
    CHECK(s.sanitized().shadow_lod_bias == 3);

    SceneStats base{}, biased{}, forced{};
    s.shadow_lod_bias = 0;
    const auto a = frame(s, base);
    s.shadow_lod_bias = 2;
    const auto b = frame(s, biased);
    test::save_output("shadow_lod_bias2", W, H, b);
    CHECK(base.triangles == biased.triangles);       // main view unchanged
    CHECK(base.shadow_triangles > 0);
    CHECK(biased.shadow_triangles < base.shadow_triangles);
    CHECK(biased.shadow_batches == base.shadow_batches); // the two spheres still instance together
    CHECK(mean_diff(a, b) < 1.5);
    // force_lod (debugging) draws the forced level in every pass and ignores the bias.
    s.force_lod = 0;
    (void)frame(s, forced);
    CHECK(forced.lod_draws == 0);
    CHECK(forced.shadow_triangles > base.shadow_triangles);
    ctx.destroy(ground);
    ctx.destroy(sphere);
}
