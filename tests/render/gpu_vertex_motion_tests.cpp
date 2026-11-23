// SPDX-License-Identifier: Apache-2.0
// Vertex functions in the depth pre-pass and object motion vectors (ADR-0244): a surface shader
// whose vertex() animates geometry over time reprojects under TAA like a moving object, writes
// the same depth in the pre-pass as in the main pass, and costs nothing while the clock is paused.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/surface_shader.hpp"

#include <cmath>
#include <string>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 128, H = 96;
constexpr double kDt = 1.0 / 30.0;

// Comb bars slide along +x by params[0].x metres a second; the surface is white and unlit.
constexpr const char* kSlide = "void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = vec3(1.0);\n}\n"
                               "void vertex(VertexInput i, inout Vertex v) {\n"
                               "    v.world_position.x += i.time * i.params[0].x; // a discarded comment\n}\n";

RenderSettings settings(bool taa, bool motion_vectors) {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = taa;
    s.motion_vectors = motion_vectors;
    s.fxaa = s.ssao = s.bloom = s.auto_exposure = false;
    s.render_scale = 1.0f;
    s.msaa_samples = 1;
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

struct Frame {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename F>
Frame render_frame(SceneRenderer& r, double time, F&& content, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    r.set_time(time);
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

double sharpness(const std::vector<std::uint8_t>& a) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x + 1 < W; ++x) sum += std::abs(channel(pixel(a, W, x + 1, y), 0) - channel(pixel(a, W, x, y), 0));
    return sum;
}

void comb(SceneRenderer& r, const GpuMesh& bar, const Material& m) {
    for (int i = 0; i < 9; ++i) r.submit(bar, m, math::Mat4::translation({-1.4f + 0.32f * static_cast<float>(i), 0, 0}));
}

struct Shaders {
    rhi::ShaderHandle fragment;
    SurfaceVertexStage stage;
    void destroy(rhi::Device& dev) {
        if (fragment) dev.destroy(fragment);
        for (auto& s : stage.shaders)
            if (s) dev.destroy(s);
    }
};

Shaders compile(const char* code) {
    auto& dev = gpu().ctx->device();
    Shaders s;
    auto frag = runtime::create_surface_shader(dev, code, "slide.vshader");
    REQUIRE(frag.ok());
    s.fragment = *frag;
    auto vx = runtime::create_surface_vertex_stage(dev, code, "slide.vshader");
    REQUIRE(vx.ok());
    s.stage = *vx;
    return s;
}
} // namespace

TEST_CASE("surface shaders: discard detection ignores comments and longer identifiers") {
    CHECK_FALSE(surface_code_may_discard(kSlide));
    CHECK_FALSE(surface_code_may_discard("void vertex(VertexInput i, inout Vertex v) { float discarded = 1.0; }"));
    CHECK_FALSE(surface_code_may_discard("/* discard */ void surface(SurfaceInput i, inout Surface s) {}"));
    CHECK(surface_code_may_discard("void surface(SurfaceInput i, inout Surface s) { if (s.alpha < 0.5) discard; }"));
}

TEST_CASE("gpu vertex functions: animated vertices get object motion vectors under TAA") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto bar = ctx.create_mesh(make_cube({0.06f, 0.8f, 0.005f}), "vx_motion_bar").value();
    Shaders sh = compile(kSlide);
    CHECK_FALSE(sh.stage.fragment_may_discard);
    for (auto v : {SurfaceVertexVariant::Motion, SurfaceVertexVariant::MotionSkinned, SurfaceVertexVariant::MotionRetained})
        CHECK(static_cast<bool>(sh.stage.get(v)));
    Material m;
    m.unlit = true;
    m.surface_shader = sh.fragment;
    m.vertex_stage = &sh.stage;
    constexpr float kSpeed = 0.11f / static_cast<float>(kDt); // 0.11 m a frame, as gpu_motion_tests
    m.shader_params[0] = {kSpeed, 0, 0, 0};
    constexpr int kFrames = 14;
    const double final_time = kDt * (kFrames - 1);
    {
        // Reference: TAA converged with the clock held at the final frame's time.
        auto ref_r = SceneRenderer::create(ctx).value();
        ref_r->set_settings(settings(true, true));
        Frame ref;
        for (int f = 0; f < kFrames; ++f)
            ref = render_frame(*ref_r, final_time, [&](SceneRenderer& r) { comb(r, bar, m); },
                               f == kFrames - 1 ? "vertex_motion_reference" : nullptr);
        CHECK(ref.stats.vertex_motion_draws == 0); // paused clock: nothing moves

        double error[2]{}, sharp[2]{};
        for (int with_motion = 0; with_motion < 2; ++with_motion) {
            auto r = SceneRenderer::create(ctx).value();
            r->set_settings(settings(true, with_motion != 0));
            Frame last;
            for (int f = 0; f < kFrames; ++f) {
                last = render_frame(*r, kDt * f, [&](SceneRenderer& s) { comb(s, bar, m); },
                                    f == kFrames - 1 ? (with_motion ? "vertex_motion_on" : "vertex_motion_off") : nullptr);
                if (with_motion && f > 0) CHECK(last.stats.vertex_motion_draws == 9);
                if (!with_motion || f == 0) CHECK(last.stats.vertex_motion_draws == 0);
            }
            error[with_motion] = mean_abs_diff(last.px, ref.px);
            sharp[with_motion] = sharpness(last.px);
        }
        MESSAGE("mean |TAA - reference|: camera-only " << error[0] << ", with vertex motion " << error[1]);
        MESSAGE("sharpness: reference " << sharpness(ref.px) << ", camera-only " << sharp[0] << ", with vertex motion " << sharp[1]);
        CHECK(error[1] < error[0] * 0.65);
        CHECK(sharp[1] > sharp[0] * 1.5);
        CHECK(sharp[1] > sharpness(ref.px) * 0.6);
    }
    {
        // Depth pre-pass: the vertex function's depth matches the main pass exactly (invariant
        // gl_Position), so forcing the pre-pass changes no pixel.
        Frame img[2];
        for (int prepass = 0; prepass < 2; ++prepass) {
            auto r = SceneRenderer::create(ctx).value();
            RenderSettings s = settings(false, false);
            s.depth_prepass = prepass != 0;
            r->set_settings(s);
            img[prepass] = render_frame(*r, 0.5, [&](SceneRenderer& sr) { comb(sr, bar, m); });
        }
        CHECK(img[0].stats.prepass_vertex_draws == 0);
        CHECK(img[1].stats.prepass_vertex_draws == 9);
        CHECK(img[1].stats.prepass_draws >= 9);
        CHECK(mean_abs_diff(img[0].px, img[1].px) == 0.0);
    }
    {
        // A surface that may discard keeps its depth out of the pre-pass.
        Shaders cut = compile("void surface(SurfaceInput i, inout Surface s) {\n    if (i.uv.x > 2.0) discard;\n}\n"
                              "void vertex(VertexInput i, inout Vertex v) {\n    v.world_position.x += i.time;\n}\n");
        CHECK(cut.stage.fragment_may_discard);
        Material cm = m;
        cm.surface_shader = cut.fragment;
        cm.vertex_stage = &cut.stage;
        {
            auto r = SceneRenderer::create(ctx).value();
            RenderSettings s = settings(false, false);
            s.depth_prepass = true;
            r->set_settings(s);
            const Frame f = render_frame(*r, 0.0, [&](SceneRenderer& sr) { comb(sr, bar, cm); });
            CHECK(f.stats.prepass_vertex_draws == 0);
        }
        cut.destroy(dev);
    }
    sh.destroy(dev);
    ctx.destroy(bar);
}
