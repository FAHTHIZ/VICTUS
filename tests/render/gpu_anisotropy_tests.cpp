// SPDX-License-Identifier: Apache-2.0
// Anisotropic filtering setting (roadmap 6b, Workstream H follow-up): a checkerboard floor seen at a
// grazing angle keeps more contrast towards the horizon as RenderSettings::max_anisotropy rises,
// and the level applied is reported in SceneStats::anisotropy.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 192, H = 108;

Image checker(std::uint32_t size, std::uint32_t cell) {
    Image img;
    img.width = img.height = size;
    img.pixels.resize(std::size_t{size} * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t i = (std::size_t{y} * size + x) * 4;
            const std::uint8_t v = ((x / cell) + (y / cell)) % 2 ? 255 : 0;
            img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = v;
            img.pixels[i + 3] = 255;
        }
    return img;
}

// Mean absolute difference between horizontally adjacent pixels (green channel) over rows
// [y0, y1): high while the checks stay resolved, near zero once they blur to grey.
double detail(const std::vector<std::uint8_t>& px, std::uint32_t y0, std::uint32_t y1) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = 1; x < W; ++x) {
            sum += std::abs(int{px[(std::size_t{y} * W + x) * 4 + 1]} - int{px[(std::size_t{y} * W + x - 1) * 4 + 1]});
            ++n;
        }
    return n ? sum / static_cast<double>(n) : 0.0;
}

} // namespace

TEST_CASE("gpu anisotropy: grazing-angle textures stay sharper at higher levels") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    TextureOptions opt;
    opt.name = "aniso_checker";
    auto tex = ctx.create_texture_mips(build_mip_chain(checker(256, 8), true), opt);
    REQUIRE(tex.ok());
    auto floor = ctx.create_mesh(make_plane({400, 400}), "aniso_floor").value();
    Material mat;
    mat.unlit = true;
    mat.base_color_texture = tex.value();
    mat.uv_scale = {25, 25}; // 16 m tiles of 32 checks: 0.5 m checks, 6 cm texels

    auto shot = [&](std::uint32_t level, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        s.max_anisotropy = level;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 1.5f, 0}, {0, 0.6f, -20}, {0, 1, 0});
        cam.fov_y = math::radians(60.0f);
        cam.z_far = 1000.0f;
        auto target = test::make_target(W, H);
        sr.value()->begin(cam, DirectionalLight{}, {});
        sr.value()->submit(floor, mat, math::Mat4::identity());
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        test::save_output(save, W, H, px);
        dev.destroy(target);
        return std::pair{px, sr.value()->stats().anisotropy};
    };

    const auto [off, off_level] = shot(1, "anisotropy_1x");
    const auto [mid, mid_level] = shot(4, "anisotropy_4x");
    const auto [full, full_level] = shot(16, "anisotropy_16x");
    CHECK(off_level == 1);
    // Rows just below the horizon: the floor is seen at a few degrees, where trilinear filtering
    // picks a mip from the compressed axis and blurs the checks to grey.
    const std::uint32_t y0 = H * 45 / 100, y1 = H * 60 / 100;
    const double d_off = detail(off, y0, y1), d_mid = detail(mid, y0, y1), d_full = detail(full, y0, y1);
    INFO("detail near the horizon: 1x " << d_off << ", 4x " << d_mid << ", 16x " << d_full << " (levels " << off_level
                                        << "/" << mid_level << "/" << full_level << ")");
    if (dev.capabilities().anisotropy) {
        CHECK(mid_level == 4);
        CHECK(full_level == 16);
        CHECK(d_mid > d_off * 1.3);
        CHECK(d_full >= d_mid * 0.95);
    } else {
        CHECK(mid_level == 1);
        CHECK(full_level == 1);
        CHECK(off == full);
    }
    // Near rows are magnified, where anisotropy does nothing.
    CHECK(std::abs(detail(off, H * 90 / 100, H) - detail(full, H * 90 / 100, H)) < 2.0);

    // Settings round to supported levels.
    RenderSettings s;
    s.max_anisotropy = 0;
    CHECK(s.sanitized().max_anisotropy == 1);
    s.max_anisotropy = 7;
    CHECK(s.sanitized().max_anisotropy == 4);
    s.max_anisotropy = 64;
    CHECK(s.sanitized().max_anisotropy == 16);
    CHECK(RenderSettings::preset(QualityTier::Low).max_anisotropy == 2);
    CHECK(RenderSettings::preset(QualityTier::Ultra).max_anisotropy == 16);
    ctx.destroy(floor);
    dev.destroy(tex.value());
}
