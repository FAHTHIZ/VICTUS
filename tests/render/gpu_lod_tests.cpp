// SPDX-License-Identifier: Apache-2.0
// Mesh LOD selection (Slice 54, ADR-0049): the renderer picks coarser levels as objects shrink on
// screen, keeps the image within the pixel error budget, batches per level and honours force_lod.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>

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
        s.shadow_lod_texels = 0.0f; // the bias alone (per-cascade LOD has its own test)
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

// H3 item 3: a main view and a secondary view (a zoomed scope or picture-in-picture camera) with
// different FOVs and render-target sizes, drawn by one renderer in the same frame. Each view picks
// its own levels from its own projection and resolution; neither inherits the other's choice, and
// each frame's SceneStats report that view's cost so callers can add them to the frame budget.
TEST_CASE("gpu lods: main and secondary views select levels independently") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_uv_sphere(0.5f, 64, 32);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    auto sphere = ctx.create_mesh(data, "view_lod_sphere").value();
    Material mat;
    mat.base_color = {0.8f, 0.5f, 0.2f, 1};
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false; // no history between views
    s.auto_exposure = false;
    sr.value()->set_settings(s);

    struct View {
        std::uint32_t w, h;
        float fov;
    };
    const View main_view{320, 180, 60.0f}, scope{96, 96, 6.0f};
    auto draw = [&](const View& v, const char* save = nullptr) {
        auto target = test::make_target(v.w, v.h);
        Camera3D cam = Camera3D::look_at({0, 1.5f, 12}, {0, 0.5f, -30});
        cam.fov_y = math::radians(v.fov);
        DirectionalLight sun;
        sun.shadow_distance = 60;
        sr.value()->begin(cam, sun, {});
        for (int i = 0; i < 9; ++i) // a row receding from 4 m to 44 m
            sr.value()->submit(sphere, mat, math::Mat4::translation({(i % 3 - 1) * 1.5f, 0.5f, 8.0f - 5.0f * i}));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, v.w, v.h, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value(), sr.value()->stats()};
        if (save) test::save_output(save, v.w, v.h, out.px);
        dev.destroy(target);
        return out;
    };

    const Shot main_alone = draw(main_view, "lod_view_main");
    const Shot scope_alone = draw(scope, "lod_view_scope");
    // The narrow, small scope magnifies the far spheres: fewer of them, at finer levels.
    INFO("main: " << main_alone.stats.visible << " visible, " << main_alone.stats.lod_draws << " coarser, "
                  << main_alone.stats.triangles << " tris; scope: " << scope_alone.stats.visible << " visible, "
                  << scope_alone.stats.lod_draws << " coarser, " << scope_alone.stats.triangles << " tris");
    CHECK(main_alone.stats.lod_draws > 0);
    CHECK(scope_alone.stats.visible < main_alone.stats.visible);
    CHECK(scope_alone.stats.triangles / std::max<std::uint64_t>(1, scope_alone.stats.visible) >
          main_alone.stats.triangles / std::max<std::uint64_t>(1, main_alone.stats.visible));

    // Interleaved in one frame, in both orders: each view matches its standalone render.
    for (int round = 0; round < 2; ++round) {
        const Shot a = draw(main_view);
        const Shot b = draw(scope);
        const Shot c = draw(main_view);
        for (const auto& [got, want] : {std::pair{&a, &main_alone}, std::pair{&b, &scope_alone}, std::pair{&c, &main_alone}}) {
            CHECK(got->stats.triangles == want->stats.triangles);
            CHECK(got->stats.lod_draws == want->stats.lod_draws);
            CHECK(got->stats.shadow_triangles == want->stats.shadow_triangles);
            CHECK(mean_diff(got->px, want->px) < 0.25);
        }
    }
    ctx.destroy(sphere);
}

TEST_CASE("gpu lods: hysteresis keeps a level at a threshold without exceeding the budget") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_uv_sphere(0.5f, 64, 32);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    auto sphere = ctx.create_mesh(data, "lod_hysteresis_sphere").value();
    Material mat;
    auto tracked = [&](SceneRenderer& r) { r.submit(sphere, mat, math::Mat4::identity(), 7); };
    RenderSettings stateless = RenderSettings::preset(QualityTier::High);
    stateless.lod_error_pixels = 0.25f;
    stateless.lod_hysteresis = 0.0f;
    stateless.lod_fade_frames = 0; // counts triangles of one level per frame
    RenderSettings held = stateless;
    held.lod_hysteresis = 0.25f;
    SceneRenderer& r = *sr.value();
    const auto at = [&](const RenderSettings& s, float z) { return view_stats(r, s, {0, 0, z}, 60.0f, 160, 96, tracked); };

    // Find the distance where the stateless rule switches LOD 0 -> LOD 1 (0.5 mm resolution).
    const std::uint64_t lod0 = at(stateless, 1.0f).triangles;
    float lo = 1.0f, hi = 60.0f;
    REQUIRE(at(stateless, hi).triangles < lod0);
    while (hi - lo > 5e-4f) {
        const float mid = 0.5f * (lo + hi);
        (at(stateless, mid).triangles == lod0 ? lo : hi) = mid;
    }
    const float threshold = 0.5f * (lo + hi);
    INFO("threshold distance " << threshold);

    // An object breathing 1% around the threshold: stateless selection switches every frame,
    // hysteresis switches at most once.
    const std::uint64_t ref_near = at(stateless, threshold * 0.99f).triangles;
    const std::uint64_t ref_far = at(stateless, threshold * 1.01f).triangles;
    REQUIRE(ref_near == lod0);
    REQUIRE(ref_far < lod0);
    const auto switches = [&](const RenderSettings& s) {
        r.reset_history();
        int n = 0;
        std::uint64_t prev = 0;
        for (int i = 0; i < 16; ++i) {
            const SceneStats st = at(s, threshold * (i % 2 ? 1.01f : 0.99f));
            if (i > 0 && st.triangles != prev) ++n;
            prev = st.triangles;
            CHECK(st.triangles >= (i % 2 ? ref_far : ref_near)); // never coarser: the budget holds
        }
        return n;
    };
    CHECK(switches(stateless) == 15);
    CHECK(switches(held) <= 1);

    // Receding: the level is held until the distance passes the shrunk budget, then coarsens.
    r.reset_history();
    (void)at(held, threshold * 0.9f);
    const SceneStats hold = at(held, threshold * 1.05f);
    CHECK(hold.triangles == lod0);
    CHECK(hold.lod_held == 1);
    const SceneStats far = at(held, threshold * 2.0f);
    CHECK(far.triangles < lod0);
    // Approaching past the threshold refines at once.
    CHECK(at(held, threshold * 0.95f).triangles == lod0);

    // reset_history() and a different output size (another view) fall back to the stateless rule.
    (void)at(held, threshold * 0.9f);
    r.reset_history();
    CHECK(at(held, threshold * 1.05f).triangles < lod0);
    (void)at(held, threshold * 0.9f);
    const SceneStats other_view = view_stats(r, held, {0, 0, threshold * 1.05f}, 60.0f, 160, 97, tracked);
    CHECK(other_view.lod_held == 0);

    // Draws without a motion id stay stateless.
    auto untracked = [&](SceneRenderer& rr) { rr.submit(sphere, mat, math::Mat4::identity()); };
    (void)view_stats(r, held, {0, 0, threshold * 0.9f}, 60.0f, 160, 96, untracked);
    CHECK(view_stats(r, held, {0, 0, threshold * 1.05f}, 60.0f, 160, 96, untracked).triangles < lod0);
    ctx.destroy(sphere);
}

TEST_CASE("gpu shadows: receiver culling skips casters whose shadow no cascade pixel can see, image unchanged") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto ground = ctx.create_mesh(make_plane({200, 200}), "recv_ground").value();
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 32, 16), "recv_sphere").value();
    auto pillar = ctx.create_mesh(make_cube({0.25f, 3.0f, 0.25f}), "recv_pillar").value();
    Material mat;
    mat.cast_shadows = true;
    constexpr std::uint32_t W = 256, H = 144;
    // A street of spheres receding from the camera (they land in every cascade's box), plus an
    // off-screen pillar up-sun whose long shadow crosses the view: it must still be drawn.
    auto frame = [&](RenderSettings s, SceneStats& st, math::Vec3 sun_dir) {
        s.taa = false;
        s.ssao = false;
        s.lods = false;
        sr.value()->set_settings(s);
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = sun_dir;
        sun.shadow_distance = 60;
        sr.value()->begin(Camera3D::look_at({0, 1.6f, 4}, {0, 0.8f, -6}), sun, {});
        sr.value()->submit(ground, mat, math::Mat4::identity());
        for (int i = 0; i < 24; ++i)
            for (float x : {-2.0f, 2.0f})
                sr.value()->submit(sphere, mat, math::Mat4::translation({x, 0.5f, 2.0f - 2.5f * float(i)}));
        sr.value()->submit(pillar, mat, math::Mat4::translation({-14.0f, 3.0f, -8.0f}));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        st = sr.value()->stats();
        dev.destroy(target);
        return px;
    };
    CHECK(RenderSettings{}.shadow_receiver_culling);
    for (const QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High, QualityTier::Ultra}) {
        const math::Vec3 suns[] = {{1.0f, -0.6f, 0.2f}, {0.3f, -1.0f, -0.4f}, {0.2f, -0.35f, 1.0f}};
        for (const math::Vec3 sun : suns) {
            RenderSettings s = RenderSettings::preset(tier);
            SceneStats on{}, off{};
            s.shadow_receiver_culling = false;
            const auto a = frame(s, off, sun);
            s.shadow_receiver_culling = true;
            const auto b = frame(s, on, sun);
            INFO("tier " << int(tier) << " sun " << sun.x << "," << sun.y << "," << sun.z << ": shadow triangles "
                         << on.shadow_triangles << " vs " << off.shadow_triangles << ", culled pairs "
                         << on.shadow_receiver_culled);
            MESSAGE("receiver culling tier " << int(tier) << ": " << on.shadow_triangles << " / "
                                             << off.shadow_triangles << " shadow triangles, "
                                             << on.shadow_receiver_culled << " pairs skipped");
            CHECK(off.shadow_receiver_culled == 0);
            CHECK(on.triangles == off.triangles); // main view untouched
            CHECK(on.shadow_triangles <= off.shadow_triangles);
            if (off.shadow_cascades > 1) {
                CHECK(on.shadow_receiver_culled > 0);
                CHECK(on.shadow_triangles < off.shadow_triangles);
            }
            int worst = 0;
            for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
            CHECK(worst == 0); // lossless: no pixel that samples a cascade loses a caster
            if (tier == QualityTier::High && sun.x > 0.9f) test::save_output("shadow_receiver_culling", W, H, b);
        }
    }
    ctx.destroy(ground);
    ctx.destroy(sphere);
    ctx.destroy(pillar);
}

// ADR-0132: each directional cascade picks the coarsest level whose error spans at most
// shadow_lod_texels of its own texels. A street of detailed spheres recedes through every
// cascade: the main view is unchanged, the wide cascades draw far fewer shadow triangles and the
// shadows look the same.
TEST_CASE("gpu shadows: per-cascade LOD cuts shadow triangles without changing the image") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto ground = ctx.create_mesh(make_plane({200, 200}), "clod_ground").value();
    MeshData data = make_uv_sphere(0.5f, 96, 48);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    auto sphere = ctx.create_mesh(data, "clod_sphere").value();
    Material mat;
    mat.cast_shadows = true;
    constexpr std::uint32_t W = 384, H = 216;
    auto frame = [&](RenderSettings s, SceneStats& st) {
        s.taa = false;
        s.ssao = false;
        s.lod_error_pixels = 0.25f; // fine main-view levels, as at phone and PC resolutions
        s.lod_hysteresis = 0.0f;
        sr.value()->set_settings(s);
        auto target = test::make_target(W, H);
        DirectionalLight sun;
        sun.direction = {0.25f, -0.4f, -1.0f}; // low sun: near shadows run into the wide cascades
        sun.shadow_distance = 60;
        sr.value()->begin(Camera3D::look_at({0, 1.6f, 4}, {0, 0.8f, -6}), sun, {});
        sr.value()->submit(ground, mat, math::Mat4::identity());
        for (int i = 0; i < 20; ++i)
            for (float x : {-1.5f, 1.5f})
                sr.value()->submit(sphere, mat, math::Mat4::translation({x, 0.5f, 2.0f - 2.5f * float(i)}));
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        st = sr.value()->stats();
        dev.destroy(target);
        return px;
    };
    CHECK(RenderSettings{}.shadow_lod_texels == 1.0f);
    {
        RenderSettings s;
        s.shadow_lod_texels = 20.0f;
        CHECK(s.sanitized().shadow_lod_texels == 8.0f);
        s.shadow_lod_texels = -1.0f;
        CHECK(s.sanitized().shadow_lod_texels == 0.0f);
        s.shadow_lod_texels = std::numeric_limits<float>::quiet_NaN();
        CHECK(s.sanitized().shadow_lod_texels == 1.0f);
    }
    for (const QualityTier tier : {QualityTier::Low, QualityTier::Medium, QualityTier::High, QualityTier::Ultra}) {
        RenderSettings s = RenderSettings::preset(tier);
        SceneStats on{}, off{};
        s.shadow_lod_texels = 0.0f;
        const auto a = frame(s, off);
        s.shadow_lod_texels = 1.0f;
        const auto b = frame(s, on);
        int worst = 0, over8 = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const int d = std::abs(int(a[i]) - int(b[i]));
            worst = std::max(worst, d);
            over8 += d > 8 ? 1 : 0;
        }
        const double mean = mean_diff(a, b);
        MESSAGE("per-cascade shadow LOD tier " << int(tier) << ": " << on.shadow_triangles << " / "
                                               << off.shadow_triangles << " shadow triangles, main "
                                               << on.triangles << ", mean diff " << mean << ", worst " << worst
                                               << ", channels > 8: " << over8);
        CHECK(on.triangles == off.triangles); // main view untouched
        CHECK(on.shadow_batches <= off.shadow_batches + 4);
        CHECK(on.shadow_triangles <= off.shadow_triangles);
        if (off.shadow_cascades > 1) CHECK(on.shadow_triangles < off.shadow_triangles);
        CHECK(mean < 0.05);
        // < 0.02 % of the 331 776 channels, all on shadow edges (lavapipe 17 at most, Metal 41 on High).
        CHECK(over8 <= 64);
        if (tier == QualityTier::High) test::save_output("shadow_cascade_lod", W, H, b);
    }
    // force_lod (debugging) still draws the forced level in every cascade.
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.force_lod = 0;
    SceneStats forced{}, full{};
    (void)frame(s, forced);
    s.force_lod = -1;
    s.lods = false;
    (void)frame(s, full);
    CHECK(forced.shadow_triangles == full.shadow_triangles);
    ctx.destroy(ground);
    ctx.destroy(sphere);
}

TEST_CASE("gpu lods: cross-fade dissolves a level change over lod_fade_frames without holes, also for group clusters") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_uv_sphere(0.5f, 64, 32);
    REQUIRE(generate_lods(data) == 3);
    optimize_mesh(data);
    auto sphere = ctx.create_mesh(data, "lod_fade_sphere").value();
    Material mat;
    auto tracked = [&](SceneRenderer& r) { r.submit(sphere, mat, math::Mat4::identity(), 7); };
    RenderSettings snap = RenderSettings::preset(QualityTier::High);
    snap.lod_error_pixels = 2.0f; // switch while the sphere is still large on screen
    snap.lod_hysteresis = 0.0f;
    snap.lod_fade_frames = 0;
    RenderSettings fade = snap;
    fade.lod_fade_frames = 4;
    SceneRenderer& r = *sr.value();
    const auto at = [&](const RenderSettings& s, float z) { return view_stats(r, s, {0, 0, z}, 60.0f, 160, 96, tracked); };

    // Start close enough that a coarser level is still left to switch to: simplification errors
    // differ slightly between platforms, so at distance 1 lavapipe draws LOD2 and Metal already
    // the coarsest level. Halve the gap to the surface (radius 0.5) until the far view is coarser.
    float lo = 1.0f, hi = 60.0f;
    const std::uint64_t coarsest = at(snap, hi).triangles;
    while (lo > 0.505f && at(snap, lo).triangles <= coarsest) lo = 0.5f + 0.5f * (lo - 0.5f);
    const std::uint64_t lod0 = at(snap, lo).triangles;
    INFO("search starts at z " << lo << " with " << lod0 << " tris, far view " << coarsest);
    REQUIRE(coarsest < lod0);
    while (hi - lo > 1e-3f) {
        const float mid = 0.5f * (lo + hi);
        (at(snap, mid).triangles == lod0 ? lo : hi) = mid;
    }
    const float near_z = lo * 0.97f, far_z = hi * 1.03f;
    r.reset_history();
    const Shot solid_far = shoot(r, snap, {0, 0, far_z}, tracked, "lod_fade_solid");
    const std::uint64_t lod1 = solid_far.stats.triangles;
    REQUIRE(lod1 < lod0);
    INFO("threshold " << lo << ", LOD0 " << lod0 << " tris, LOD1 " << lod1);
    // The finer level forced at the same distance: a dithered pixel may show either level, so
    // only pixels matching neither are holes or artefacts.
    RenderSettings fine = snap;
    for (fine.force_lod = 0; fine.force_lod < 4 && at(fine, far_z).triangles != lod0; ++fine.force_lod) {
    }
    r.reset_history();
    const Shot solid_fine = shoot(r, fine, {0, 0, far_z}, tracked, "lod_fade_solid_fine");
    INFO("fine view " << solid_fine.stats.triangles << " tris");
    REQUIRE(solid_fine.stats.triangles == lod0);

    // Without cross-fade the level snaps in one frame.
    r.reset_history();
    (void)shoot(r, snap, {0, 0, near_z}, tracked);
    CHECK(shoot(r, snap, {0, 0, far_z}, tracked).stats.lod_fading == 0);

    r.reset_history();
    const Shot first = shoot(r, fade, {0, 0, near_z}, tracked);
    CHECK(first.stats.lod_fading == 0); // no history: nothing to fade from
    CHECK(first.stats.triangles == lod0);
    for (int frame = 1; frame <= 4; ++frame) {
        const Shot s = shoot(r, fade, {0, 0, far_z}, tracked, frame == 2 ? "lod_fade_mid" : nullptr);
        INFO("fade frame " << frame);
        CHECK(s.stats.lod_fading == 1);
        CHECK(s.stats.triangles == lod0 + lod1); // both levels drawn
        CHECK(s.stats.prepass_draws == 0);       // dithered draws take their depth from the main pass
        // The two levels cover complementary pixels: every pixel matches one of the solid levels
        // (no holes showing the background). Near the switch distance the sphere fills much of
        // the view, so the silhouettes of the two levels differ by more pixels than a margin.
        std::size_t far_off = 0;
        for (std::size_t i = 0; i < s.px.size(); i += 4) {
            int d_coarse = 0, d_fine = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                d_coarse += std::abs(int(s.px[i + c]) - int(solid_far.px[i + c]));
                d_fine += std::abs(int(s.px[i + c]) - int(solid_fine.px[i + c]));
            }
            if (std::min(d_coarse, d_fine) > 90) ++far_off;
        }
        MESSAGE("cross-fade frame " << frame << ": " << far_off << " pixels match neither level");
        CHECK(far_off <= 40);
    }
    const Shot done = shoot(r, fade, {0, 0, far_z}, tracked);
    CHECK(done.stats.lod_fading == 0);
    CHECK(done.stats.triangles == lod1);
    CHECK(mean_diff(done.px, solid_far.px) < 0.05);

    // Untracked draws (no motion id) never fade.
    auto untracked = [&](SceneRenderer& rr) { rr.submit(sphere, mat, math::Mat4::identity()); };
    r.reset_history();
    (void)shoot(r, fade, {0, 0, near_z}, untracked);
    CHECK(shoot(r, fade, {0, 0, far_z}, untracked).stats.lod_fading == 0);

    // ADR-0251: a retained-group cluster is tracked by its group and first instance, so it fades
    // like a tracked draw (one copy at the origin: the same threshold as above).
    const math::Mat4 one[] = {math::Mat4::identity()};
    SceneRenderer::InstanceGroupDesc desc;
    desc.mesh = &sphere;
    desc.material = mat;
    desc.models = one;
    auto group = r.create_instance_group(desc);
    REQUIRE(group.ok());
    auto grouped = [&](SceneRenderer& rr) { (void)rr.submit_instance_group(group.value()); };
    r.reset_history();
    const Shot g_first = shoot(r, fade, {0, 0, near_z}, grouped);
    CHECK(g_first.stats.group_clusters_visible == 1);
    CHECK(g_first.stats.lod_fading == 0);
    CHECK(g_first.stats.triangles == lod0);
    for (int frame = 1; frame <= 4; ++frame) {
        const Shot s = shoot(r, fade, {0, 0, far_z}, grouped, frame == 2 ? "lod_fade_group_mid" : nullptr);
        INFO("group fade frame " << frame);
        CHECK(s.stats.lod_fading == 1);
        CHECK(s.stats.triangles == lod0 + lod1);
        std::size_t far_off = 0;
        for (std::size_t i = 0; i < s.px.size(); i += 4) {
            int d_coarse = 0, d_fine = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                d_coarse += std::abs(int(s.px[i + c]) - int(solid_far.px[i + c]));
                d_fine += std::abs(int(s.px[i + c]) - int(solid_fine.px[i + c]));
            }
            if (std::min(d_coarse, d_fine) > 90) ++far_off;
        }
        MESSAGE("group cross-fade frame " << frame << ": " << far_off << " pixels match neither level");
        CHECK(far_off <= 40);
    }
    const Shot g_done = shoot(r, fade, {0, 0, far_z}, grouped);
    CHECK(g_done.stats.lod_fading == 0);
    CHECK(g_done.stats.triangles == lod1);
    CHECK(mean_diff(g_done.px, solid_far.px) < 0.05);
    // Snapping still works for groups with fades off, and a rebuilt group (new id) starts fresh.
    r.reset_history();
    (void)shoot(r, snap, {0, 0, near_z}, grouped);
    CHECK(shoot(r, snap, {0, 0, far_z}, grouped).stats.lod_fading == 0);
    (void)shoot(r, fade, {0, 0, near_z}, grouped);
    r.destroy_instance_group(group.value());
    auto rebuilt = r.create_instance_group(desc);
    REQUIRE(rebuilt.ok());
    CHECK(shoot(r, fade, {0, 0, far_z}, [&](SceneRenderer& rr) { (void)rr.submit_instance_group(rebuilt.value()); }).stats.lod_fading == 0);
    r.destroy_instance_group(rebuilt.value());
    ctx.destroy(sphere);
}
