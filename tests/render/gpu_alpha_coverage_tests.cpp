// SPDX-License-Identifier: Apache-2.0
// Alpha-tested surfaces during zoom (roadmap 6b, Workstream H3 item 8): a cutout "foliage" texture
// (random 2x2 speckles, ~30% opaque) on a plane seen from above at increasing distances. With
// box-filtered alpha mips the coarser levels average below the cutoff and the surface vanishes;
// with coverage-preserving mips (render::preserve_alpha_coverage) it keeps about half or more of
// the close-up's coverage at every distance.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

Image speckles(std::uint32_t size) {
    Image img;
    img.width = img.height = size;
    img.pixels.resize(std::size_t{size} * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint32_t h = (x / 2) * 73856093u ^ (y / 2) * 19349663u;
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            const std::size_t i = (std::size_t{y} * size + x) * 4;
            img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = 255;
            img.pixels[i + 3] = h % 100 < 30 ? 255 : 0;
        }
    return img;
}

} // namespace

TEST_CASE("gpu alpha coverage: cutout surfaces keep their coverage when zoomed out") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const Image base = speckles(256);
    const float source = alpha_coverage(base, 0.5f);
    REQUIRE(source > 0.25f);
    REQUIRE(source < 0.35f);
    auto plain_chain = build_mip_chain(base, true);
    auto kept_chain = plain_chain;
    preserve_alpha_coverage(kept_chain, 0.5f);
    TextureOptions opt;
    opt.name = "speckles_plain";
    auto plain = ctx.create_texture_mips(plain_chain, opt);
    opt.name = "speckles_kept";
    auto kept = ctx.create_texture_mips(kept_chain, opt);
    REQUIRE(plain.ok());
    REQUIRE(kept.ok());
    auto plane = ctx.create_mesh(make_plane({80, 80}), "cutout_plane").value();
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.bloom = false;
    s.ssao = false;
    s.auto_exposure = false;
    sr.value()->set_settings(s);
    Material mat;
    mat.unlit = true;
    mat.double_sided = true;
    mat.alpha_cutoff = 0.5f;
    mat.uv_scale = {16, 16}; // 4096 texels across 80 m: ~51 texels per metre

    auto shot = [&](float height, const Material* m, const char* save = nullptr) {
        auto target = test::make_target(W, H);
        Camera3D cam = Camera3D::look_at({0, height, 0}, {0, 0, 0}, {0, 0, -1});
        cam.fov_y = math::radians(60.0f);
        cam.z_near = 0.01f;
        cam.z_far = 500.0f;
        sr.value()->begin(cam, DirectionalLight{}, {});
        if (m) sr.value()->submit(plane, *m, math::Mat4::identity());
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        if (save) test::save_output(save, W, H, px);
        dev.destroy(target);
        return px;
    };
    // Fraction of the frame where the white cutout surface replaces the empty-scene background.
    auto coverage = [&](const std::vector<std::uint8_t>& px, const std::vector<std::uint8_t>& empty) {
        std::size_t hit = 0;
        for (std::size_t i = 0; i < std::size_t{W} * H; ++i) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d += std::abs(int{px[i * 4 + c]} - int{empty[i * 4 + c]});
            hit += d > 96 ? 1u : 0u;
        }
        return static_cast<double>(hit) / (double{W} * H);
    };

    Material m_plain = mat, m_kept = mat;
    m_plain.base_color_texture = plain.value();
    m_kept.base_color_texture = kept.value();
    struct Row {
        float height;
        double plain, kept;
    };
    std::vector<Row> rows;
    for (const float h : {0.5f, 6.0f, 15.0f, 40.0f}) {
        const auto empty = shot(h, nullptr);
        const bool far = h == 40.0f;
        rows.push_back({h, coverage(shot(h, &m_plain, far ? "alpha_coverage_plain_far" : nullptr), empty),
                        coverage(shot(h, &m_kept, far ? "alpha_coverage_kept_far" : nullptr), empty)});
    }
    const double close = rows[0].kept;
    for (const Row& r : rows) {
        INFO("height " << r.height << " m: plain " << r.plain << ", preserved " << r.kept << " (source " << source << ")");
        CHECK(r.kept >= r.plain - 0.005); // never worse than box-filtered alpha
        CHECK(r.kept < close * 1.6);
        // Where coarse mips are sampled (>= 15 m: mip 4 and below) the surface keeps at least ~half
        // its close-up coverage. The remaining loss, and the dip at 6 m that both chains share, is
        // bilinear erosion of single-texel features under the alpha test (Wyman and McGuire 2017),
        // which needs hashed alpha or alpha-to-coverage rather than different mips.
        if (r.height >= 15.0f) CHECK(r.kept > close * 0.45);
    }
    // At the close-up both chains sample mip 0 and agree; zoomed out, plain mips lose the surface.
    CHECK(std::abs(rows[0].plain - rows[0].kept) < 0.02);
    CHECK(close > 0.2);
    CHECK(rows.back().plain < close * 0.25);
    dev.destroy(plain.value());
    dev.destroy(kept.value());
    ctx.destroy(plane);
}
