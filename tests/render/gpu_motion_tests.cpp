// SPDX-License-Identifier: Apache-2.0
// Object motion vectors (Slice 57, ADR-0054): moving rigid and skinned draws with a motion id keep
// a sharp TAA history instead of smearing, unmoved or untracked draws cost nothing, and skinned
// motion (from the joint palettes) matches the same motion applied through the model matrix.
#include "../assets/skinned_model.hpp"
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 128, H = 96;

RenderSettings taa_settings(bool taa, bool motion_vectors) {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = taa;
    s.motion_vectors = motion_vectors;
    s.fxaa = s.ssao = s.bloom = s.auto_exposure = false;
    s.render_scale = 1.0f;
    return s;
}

Environment black_env() {
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    return env;
}

DirectionalLight no_sun() {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    return sun;
}

Material unlit(math::Color c) {
    Material m;
    m.unlit = true;
    m.base_color = c;
    return m;
}

struct Frame {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename F>
Frame render_frame(SceneRenderer& r, F&& content, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    r.begin(Camera3D::look_at({0, 0, 6}, {0, 0, 0}), no_sun(), black_env());
    content(r);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Frame out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

double mean_abs_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) sum += std::abs(channel(pixel(a, W, x, y), 0) - channel(pixel(b, W, x, y), 0));
    return sum / (W * H);
}

// Horizontal detail: summed differences between neighbouring pixels (smearing lowers it).
double sharpness(const std::vector<std::uint8_t>& a) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x + 1 < W; ++x) sum += std::abs(channel(pixel(a, W, x + 1, y), 0) - channel(pixel(a, W, x, y), 0));
    return sum;
}

// A comb of thin white bars sliding right by `step` metres a frame: detail finer than the TAA
// neighbourhood, so camera-only reprojection smears it and only object motion keeps it sharp.
void comb(SceneRenderer& r, const GpuMesh& bar, float offset, bool tracked) {
    const Material white = unlit({1, 1, 1, 1});
    for (int i = 0; i < 9; ++i)
        r.submit(bar, white, math::Mat4::translation({-1.4f + 0.32f * static_cast<float>(i) + offset, 0, 0}),
                 tracked ? static_cast<std::uint64_t>(100 + i) : 0u);
}

} // namespace

TEST_CASE("gpu motion vectors: a moving object keeps a sharp TAA history") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto bar = ctx.create_mesh(make_cube({0.06f, 0.8f, 0.005f}), "motion_bar").value();
    constexpr int kFrames = 14;
    constexpr float kStep = 0.11f;

    // Reference: TAA converged on the comb held still at its final position (same jitter phase).
    auto ref_r = SceneRenderer::create(ctx).value();
    ref_r->set_settings(taa_settings(true, true));
    const float final_offset = kStep * (kFrames - 1);
    Frame ref;
    for (int f = 0; f < kFrames; ++f)
        ref = render_frame(*ref_r, [&](SceneRenderer& r) { comb(r, bar, final_offset, true); },
                           f == kFrames - 1 ? "motion_reference" : nullptr);
    CHECK(ref.stats.motion_draws == 0);

    double error[2]{}, sharp[2]{};
    for (int with_motion = 0; with_motion < 2; ++with_motion) {
        auto r = SceneRenderer::create(ctx).value();
        r->set_settings(taa_settings(true, with_motion != 0));
        Frame last;
        for (int f = 0; f < kFrames; ++f) {
            const bool final_frame = f == kFrames - 1;
            last = render_frame(*r, [&](SceneRenderer& s) { comb(s, bar, kStep * static_cast<float>(f), true); },
                                final_frame ? (with_motion ? "motion_vectors_on" : "motion_vectors_off") : nullptr);
            if (with_motion && f == 0) CHECK(last.stats.motion_draws == 0); // no previous frame yet
            if (with_motion && f > 0) {
                CHECK(last.stats.motion_draws == 9);
                CHECK(last.stats.motion_batches == 1); // one mesh, one instanced draw
            }
            if (!with_motion) CHECK(last.stats.motion_draws == 0);
        }
        error[with_motion] = mean_abs_diff(last.px, ref.px);
        sharp[with_motion] = sharpness(last.px);
    }
    MESSAGE("mean |TAA - reference|: camera-only " << error[0] << ", with object motion " << error[1]);
    MESSAGE("sharpness: reference " << sharpness(ref.px) << ", camera-only " << sharp[0] << ", with object motion " << sharp[1]);
    CHECK(error[1] < error[0] * 0.65);
    CHECK(sharp[1] > sharp[0] * 1.5);          // camera-only reprojection smears the comb
    CHECK(sharp[1] > sharpness(ref.px) * 0.6); // object motion keeps most of the converged detail

    // Graph: the motion pass only exists while something moved.
    auto r = SceneRenderer::create(ctx).value();
    r->set_settings(taa_settings(true, true));
    for (int f = 0; f < 3; ++f) (void)render_frame(*r, [&](SceneRenderer& s) { comb(s, bar, 0.0f, true); });
    CHECK(r->stats().motion_draws == 0); // tracked but unmoved
    bool has_motion_pass = false;
    for (const auto& p : r->graph_info().executed_passes) has_motion_pass |= p == "motion_vectors";
    CHECK_FALSE(has_motion_pass);
    (void)render_frame(*r, [&](SceneRenderer& s) { comb(s, bar, 0.05f, true); });
    CHECK(r->stats().motion_draws == 9);
    for (const auto& p : r->graph_info().executed_passes) has_motion_pass |= p == "motion_vectors";
    CHECK(has_motion_pass);
    // Untracked draws (motion id 0) and alpha-tested draws never get object motion.
    (void)render_frame(*r, [&](SceneRenderer& s) { comb(s, bar, 0.1f, false); });
    CHECK(r->stats().motion_draws == 0);
    Material cutout = unlit({1, 1, 1, 1});
    cutout.alpha_cutoff = 0.5f;
    (void)render_frame(*r, [&](SceneRenderer& s) { s.submit(bar, cutout, math::Mat4::identity(), 7); });
    (void)render_frame(*r, [&](SceneRenderer& s) { s.submit(bar, cutout, math::Mat4::translation({0.2f, 0, 0}), 7); });
    CHECK(r->stats().motion_draws == 0);
    // A history reset drops the previous frame: nothing to compare against.
    r->reset_history();
    (void)render_frame(*r, [&](SceneRenderer& s) { comb(s, bar, 0.3f, true); });
    CHECK(r->stats().motion_draws == 0);
    ctx.destroy(bar);
}

TEST_CASE("gpu motion vectors: skinned motion matches the same motion through the model matrix") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    const assets::CookedModel model = test::make_skinned_column();
    auto mesh = ctx.create_mesh(model.meshes[0].data, "motion_skin_column").value();
    REQUIRE(mesh.skinned());
    const math::Aabb rest{{-0.3f, -0.3f, -0.3f}, {0.3f, 2.3f, 0.3f}};
    const Material red = unlit({0.9f, 0.1f, 0.05f, 1});
    constexpr int kFrames = 10;
    const auto offset = [](int f) { return math::Vec3{-1.0f + 0.13f * static_cast<float>(f), -1.0f, 0}; };

    // Rigid: the model matrix moves, joints stay at rest.
    auto rigid = SceneRenderer::create(ctx).value();
    rigid->set_settings(taa_settings(true, true));
    Frame rigid_last;
    for (int f = 0; f < kFrames; ++f) {
        const math::Mat4 m = math::Mat4::translation(offset(f));
        const math::Mat4 joints[2] = {math::Mat4::identity(), math::Mat4::identity()};
        rigid_last = render_frame(*rigid, [&](SceneRenderer& r) { r.submit_skinned(mesh, red, m, joints, rest.transformed(m), 5); },
                                  f == kFrames - 1 ? "motion_skin_rigid" : nullptr);
        if (f > 0) CHECK(rigid_last.stats.motion_draws == 1);
    }
    // Skinned: the model stays put and both joints carry the same motion.
    auto skinned = SceneRenderer::create(ctx).value();
    skinned->set_settings(taa_settings(true, true));
    Frame skin_last;
    for (int f = 0; f < kFrames; ++f) {
        const math::Mat4 t = math::Mat4::translation(offset(f));
        const math::Mat4 joints[2] = {t, t};
        skin_last = render_frame(*skinned, [&](SceneRenderer& r) {
            r.submit_skinned(mesh, red, math::Mat4::identity(), joints, rest.transformed(t), 5);
        }, f == kFrames - 1 ? "motion_skin_joints" : nullptr);
        if (f > 0) {
            CHECK(skin_last.stats.motion_draws == 1);
            CHECK(skin_last.stats.joints_uploaded == 4); // current + previous palette
        }
    }
    const double diff = mean_abs_diff(rigid_last.px, skin_last.px);
    MESSAGE("mean |rigid - skinned| " << diff);
    CHECK(diff < 1.0);
    // An unchanged pose and transform: no motion draw, no extra palette.
    const math::Mat4 t = math::Mat4::translation(offset(kFrames - 1));
    const math::Mat4 joints[2] = {t, t};
    const Frame still = render_frame(*skinned, [&](SceneRenderer& r) {
        r.submit_skinned(mesh, red, math::Mat4::identity(), joints, rest.transformed(t), 5);
    });
    CHECK(still.stats.motion_draws == 0);
    CHECK(still.stats.joints_uploaded == 2);
    ctx.destroy(mesh);
}

TEST_CASE("render settings: motion vectors default on") {
    CHECK(RenderSettings::preset(QualityTier::High).motion_vectors);
    CHECK(RenderSettings::preset(QualityTier::Ultra).motion_vectors);
    CHECK(RenderSettings{}.motion_vectors);
}


TEST_CASE("gpu motion vectors: scene entities are tracked by entity id") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    auto r = SceneRenderer::create(ctx).value();
    r->set_settings(taa_settings(true, true));
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().get<scene::Transform>(cam)->position = {0, 0, 6};
    s.world().add<scene::CameraComponent>(cam);
    const scene::Entity still = s.create("Still");
    s.world().add<scene::MeshRendererComponent>(still).primitive = "cube";
    s.world().get<scene::Transform>(still)->position = {-1.5f, 0, 0};
    const scene::Entity mover = s.create("Mover");
    s.world().add<scene::MeshRendererComponent>(mover).primitive = "cube";
    auto frame = [&](float x) {
        s.world().get<scene::Transform>(mover)->position = {x, 0, 0};
        s.update_transforms();
        (void)runtime::extract_scene(s, cache, *r);
        auto target = test::make_target(W, H);
        auto cmd = ctx.device().begin_commands();
        REQUIRE(r->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        ctx.device().destroy(target);
        return r->stats();
    };
    CHECK(frame(0.0f).motion_draws == 0);
    CHECK(frame(0.0f).motion_draws == 0);
    CHECK(frame(0.3f).motion_draws == 1); // only the entity that moved
    CHECK(frame(0.3f).motion_draws == 0);
}
