// SPDX-License-Identifier: Apache-2.0
// TAA reactive mask (ADR-0143): particles, water and rain have no motion vectors, so the resolve
// trusts the current frame where they cover a pixel (and where they covered it last frame). A
// particle moving over a high-contrast background tracks a history-free reference more closely with
// the mask than without, frames without such effects are unchanged, and the path report says so.
#include "gpu_fixture.hpp"
#include "victus/render/render_path.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 128, H = 96;

struct Setup {
    std::unique_ptr<SceneRenderer> renderer;
    GpuMesh cell;
};

Setup make(bool reactive, float render_scale = 1.0f, bool taa = true) {
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cell = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.5f}), "reactive_cell");
    REQUIRE(cell.ok());
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = taa;
    s.reactive_mask = reactive;
    s.reactive_max = 0.9f; // the strongest setting projects use; the 0.5 default trades some of it for stability
    s.fxaa = s.ssao = s.bloom = s.auto_exposure = false;
    s.render_scale = render_scale;
    (*sr)->set_settings(s);
    return {std::move(*sr), *cell};
}

// Stripes of 3-render-pixel-wide unlit columns (white / dark grey) behind the effect: the TAA
// neighbourhood box is wide everywhere, so it cannot clip a stale particle out of the history.
void background(SceneRenderer& r, const GpuMesh& cell) {
    Material light, dark;
    light.unlit = dark.unlit = true;
    light.base_color = {0.9f, 0.9f, 0.9f, 1};
    dark.base_color = {0.1f, 0.1f, 0.1f, 1};
    // Camera at z 6, 60 degree fov: at z -1 the view is 2 * 7 * tan(30) = 8.08 m tall, 10.8 m wide.
    const float column = 10.8f / static_cast<float>(W) * 3.0f;
    for (int i = -24; i <= 24; ++i)
        r.submit(cell, (i & 1) ? light : dark, math::Mat4::trs({static_cast<float>(i) * column, 0, -1}, {}, {column, 9.0f, 0.01f}));
}

ParticleInstance magenta(float x) { return {{x, 0, 0, 1.2f}, {1, 0, 1, 1}, {0, 0, 0, 0}}; }

std::vector<std::uint8_t> frame(Setup& s, const ParticleInstance* p, SceneStats* stats = nullptr) {
    auto& dev = gpu().ctx->device();
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    s.renderer->begin(Camera3D::look_at({0, 0, 6}, {0, 0, 0}), sun, env);
    background(*s.renderer, s.cell);
    if (p) {
        ParticleMaterial m;
        m.blend = ParticleBlend::Alpha;
        m.softness = 0.0f;
        const float h = p->position_size.w * 0.5f;
        math::Aabb b;
        b.expand({p->position_size.x - h, p->position_size.y - h, p->position_size.z - h});
        b.expand({p->position_size.x + h, p->position_size.y + h, p->position_size.z + h});
        s.renderer->submit_particles({p, 1}, m, b);
    }
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(s.renderer->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    if (stats) *stats = s.renderer->stats();
    return px;
}

int diff(std::uint32_t a, std::uint32_t b) {
    int d = 0;
    for (int c = 0; c < 3; ++c) d += std::abs(int{channel(a, c)} - int{channel(b, c)});
    return d;
}

// Per-frame error against an aliased, history-free reference of the same frame (TAA off), over the
// pixels the particle covers this frame or covered last frame in that reference. Without the mask
// the resolve blends the particle with a history that has no motion for it: the disc lags, fades
// and leaves a trail where it was. With the mask the resolve takes this frame there.
double footprint_error(const std::vector<std::uint8_t>& img, const std::vector<std::uint8_t>& ref,
                       const std::vector<std::uint8_t>& ref_prev, const std::vector<std::uint8_t>& bg) {
    double sum = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const auto g = pixel(bg, W, x, y);
            if (diff(pixel(ref, W, x, y), g) < 24 && diff(pixel(ref_prev, W, x, y), g) < 24) continue;
            sum += diff(pixel(img, W, x, y), pixel(ref, W, x, y));
        }
    return sum;
}

// Runs a particle from x = -3 to x = +1.2 (0.12 m, about 1.7 pixels per frame) and returns the last
// frame and the footprint error summed over the last 24 frames.
std::pair<std::vector<std::uint8_t>, double> sweep(Setup& s, Setup& ref) {
    for (int i = 0; i < 12; ++i) (void)frame(s, nullptr); // converged background history
    const auto bg = frame(ref, nullptr);
    std::vector<std::uint8_t> last, ref_prev = bg;
    double err = 0;
    float x = -3.0f;
    for (int i = 0; i < 36; ++i, x += 0.12f) {
        const ParticleInstance p = magenta(x);
        last = frame(s, &p);
        auto r = frame(ref, &p);
        if (i >= 12) err += footprint_error(last, r, ref_prev, bg);
        ref_prev = std::move(r);
    }
    return {last, err};
}
} // namespace

TEST_CASE("render settings: the TAA reactive mask is on by default and its weight is clamped") {
    RenderSettings s;
    CHECK(s.reactive_mask);
    CHECK(s.reactive_max == doctest::Approx(0.5f));
    s.reactive_max = 3.0f;
    CHECK(s.sanitized().reactive_max == doctest::Approx(1.0f));
    s.reactive_max = std::nanf("");
    CHECK(s.sanitized().reactive_max == doctest::Approx(0.5f));
}

TEST_CASE("gpu: the TAA reactive mask reduces particle ghosting over a detailed background (ADR-0143)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Setup off = make(false), on = make(true), ref = make(false, 1.0f, false);
    const auto [img_off, trail_off] = sweep(off, ref);
    const auto [img_on, trail_on] = sweep(on, ref);
    test::save_output("reactive_off", W, H, img_off);
    test::save_output("reactive_on", W, H, img_on);
    MESSAGE("particle footprint error without the mask " << trail_off << ", with it " << trail_on);
    CHECK(trail_off > 1.0e5); // the scenario does ghost without the mask
    CHECK(trail_on < trail_off * 0.92); // and the mask measurably reduces it
    SceneStats st{};
    const ParticleInstance p = magenta(0.0f);
    (void)frame(on, &p, &st);
    CHECK(st.reactive_mask);
    (void)frame(off, &p, &st);
    CHECK_FALSE(st.reactive_mask);
    gpu().ctx->destroy(off.cell);
    gpu().ctx->destroy(on.cell);
    gpu().ctx->destroy(ref.cell);
}

TEST_CASE("gpu: the reactive mask also steers temporal upscaling (ADR-0143)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Setup off = make(false, 0.5f), on = make(true, 0.5f), ref = make(false, 1.0f, false);
    const auto [img_off, trail_off] = sweep(off, ref);
    const auto [img_on, trail_on] = sweep(on, ref);
    test::save_output("reactive_upscale_off", W, H, img_off);
    test::save_output("reactive_upscale_on", W, H, img_on);
    MESSAGE("upscaled particle footprint error without the mask " << trail_off << ", with it " << trail_on);
    CHECK(on.renderer->stats().temporal_upscale);
    CHECK(trail_on < trail_off * 0.8);
    gpu().ctx->destroy(off.cell);
    gpu().ctx->destroy(on.cell);
    gpu().ctx->destroy(ref.cell);
}

TEST_CASE("gpu: frames without particles, water or rain are unchanged by the reactive mask") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Setup off = make(false), on = make(true);
    std::vector<std::uint8_t> a, b;
    SceneStats st{};
    for (int i = 0; i < 6; ++i) {
        a = frame(off, nullptr);
        b = frame(on, nullptr, &st);
    }
    CHECK_FALSE(st.reactive_mask);
    CHECK(a == b);
    // The render path report explains why the mask is off.
    const RenderPathReport& r = on.renderer->path_report();
    bool found = false;
    for (const auto& f : r.features)
        if (f.name == "reactive_mask") {
            found = true;
            CHECK(f.state == PathState::Off);
            CHECK(f.reason == PathReason::NotNeeded);
        }
    CHECK(found);
    gpu().ctx->destroy(off.cell);
    gpu().ctx->destroy(on.cell);
}
