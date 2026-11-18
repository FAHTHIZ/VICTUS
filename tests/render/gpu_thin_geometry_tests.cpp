// SPDX-License-Identifier: Apache-2.0
// Thin opaque geometry below one pixel (roadmap 6b, Workstream H3 item 8): a 2 cm pole, slightly
// tilted like a railing post, seen at distances where it projects 2, 0.5 and 0.25 pixels wide.
// Measures per-row coverage ("energy", in pixels) and continuity (rows where it shows at all)
// without and with TAA, and checks that mesh LOD selection does not thin it further.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 120;
constexpr float kRadius = 0.01f;
// Metres per pixel at distance d for a 60 degree vertical field of view and H rows.
float pixel_size(float d) { return 2.0f * d * std::tan(math::radians(30.0f)) / static_cast<float>(H); }

struct Coverage {
    double energy{0};     // mean summed coverage per row, in pixels
    double continuity{0}; // share of rows where the pole shows (> 5% of a pixel)
};

Coverage measure(const std::vector<std::uint8_t>& px) {
    Coverage c;
    const std::uint32_t y0 = H / 2 - 30, y1 = H / 2 + 30;
    for (std::uint32_t y = y0; y < y1; ++y) {
        double row = 0;
        for (std::uint32_t x = 0; x < W; ++x) row += px[(std::size_t{y} * W + x) * 4] / 255.0;
        c.energy += row;
        c.continuity += row > 0.05 ? 1.0 : 0.0;
    }
    c.energy /= (y1 - y0);
    c.continuity /= (y1 - y0);
    return c;
}

} // namespace

TEST_CASE("gpu thin geometry: sub-pixel poles keep their coverage") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData data = make_cylinder(kRadius, 1.0f, 16);
    REQUIRE(generate_lods(data) >= 1);
    auto pole = ctx.create_mesh(data, "thin_pole").value();
    Material white;
    white.unlit = true;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.tonemap = false;
    env.auto_exposure = false;
    env.vignette = 0.0f;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;

    auto render = [&](float width_px, bool taa, int force_lod, const char* save = nullptr) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = taa;
        s.fxaa = s.ssao = s.bloom = s.auto_exposure = false;
        s.render_scale = 1.0f;
        s.force_lod = force_lod;
        sr.value()->set_settings(s);
        const float d = 2.0f * kRadius / (width_px * pixel_size(1.0f));
        const Camera3D cam = Camera3D::look_at({0, 0, d}, {0, 0, 0}, {0, 1, 0});
        // The pole spans ~80% of the frame height at every distance; tilted 8 degrees so it
        // crosses pixel centres at different rows, like a railing post off the screen axis.
        const math::Mat4 model = math::Mat4::trs({0, 0, 0}, math::Quat::axis_angle({0, 0, 1}, math::radians(8.0f)),
                                                 {1, 0.8f * d * std::tan(math::radians(30.0f)) * 2.0f, 1});
        std::vector<std::uint8_t> px;
        const int frames = taa ? 24 : 1;
        for (int f = 0; f < frames; ++f) {
            sr.value()->begin(cam, sun, env);
            sr.value()->submit(pole, white, model);
            auto target = test::make_target(W, H);
            auto cmd = dev.begin_commands();
            REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
            if (f == frames - 1) px = dev.read_texture(target).value();
            dev.destroy(target);
        }
        if (save) test::save_output(save, W, H, px);
        return std::pair{measure(px), sr.value()->stats().lod_draws};
    };

    for (const float w : {2.0f, 0.5f, 0.25f}) {
        const auto [plain, plain_lod] = render(w, false, -1, w == 0.25f ? "thin_pole_quarter_px" : nullptr);
        const auto [base, base_lod] = render(w, false, 0);
        const auto [taa, taa_lod] = render(w, true, -1, w == 0.25f ? "thin_pole_quarter_px_taa" : nullptr);
        MESSAGE("width " << w << " px: no TAA energy " << plain.energy << " continuity " << plain.continuity << " (lod draws "
                         << plain_lod << "), LOD 0 energy " << base.energy << " continuity " << base.continuity
                         << ", TAA energy " << taa.energy << " continuity " << taa.continuity);
        // Mesh LOD: the level picked for the distance keeps the silhouette's coverage.
        CHECK(plain_lod == 1);
        CHECK(base_lod == 0);
        CHECK(std::abs(plain.energy - base.energy) < 0.05 * base.energy + 0.01);
        CHECK(std::abs(plain.continuity - base.continuity) < 0.05);
        // Without TAA, coverage is right on average (point sampling, within 2x) ...
        CHECK(plain.energy > 0.8 * w);
        CHECK(plain.energy < 1.8 * w);
        if (w >= 1.0f) {
            CHECK(plain.continuity == doctest::Approx(1.0));
            CHECK(taa.continuity == doctest::Approx(1.0));
            CHECK(taa.energy == doctest::Approx(plain.energy).epsilon(0.1));
        } else {
            // ... but a sub-pixel pole breaks into dashes: only rows whose pixel centre it covers.
            CHECK(plain.continuity < 0.8);
            // TAA's jittered accumulation fills more rows, while neighbourhood clamping removes part
            // of the energy (measured ~0.7x at 0.5 px, ~0.55x at 0.25 px on lavapipe).
            CHECK(taa.continuity > plain.continuity + 0.15);
            CHECK(taa.energy > 0.4 * w);
            CHECK(taa.energy < plain.energy);
        }
    }
    ctx.destroy(pole);
}
