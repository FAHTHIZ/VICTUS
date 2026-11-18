// SPDX-License-Identifier: Apache-2.0
// Texture mip selection (roadmap 6b, Workstream H2/H3 item 6): sampled level follows the on-screen
// texel footprint, independently of mesh LOD, and a close-up after a distant view samples mip 0
// again (no residency state). Each level of the chain is a distinct solid colour, so the colour on
// screen identifies the level the GPU sampled.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 160, H = 96;

std::vector<Image> coloured_chain(std::uint32_t size) {
    static constexpr std::uint8_t kColours[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 0},
                                                   {0, 0, 255}, {0, 255, 0}, {0, 0, 255}, {0, 255, 0}};
    std::vector<Image> chain;
    for (std::uint32_t m = 0; size >> m; ++m) {
        Image img;
        img.width = img.height = std::max(1u, size >> m);
        img.pixels.resize(std::size_t{img.width} * img.height * 4);
        for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
            img.pixels[i] = kColours[m % 8][0];
            img.pixels[i + 1] = kColours[m % 8][1];
            img.pixels[i + 2] = kColours[m % 8][2];
            img.pixels[i + 3] = 255;
        }
        chain.push_back(std::move(img));
    }
    return chain;
}

struct Rgb {
    double r, g, b;
};

// Mean colour of the centre 16x16 pixels.
Rgb centre(const std::vector<std::uint8_t>& px) {
    Rgb c{0, 0, 0};
    for (std::uint32_t y = H / 2 - 8; y < H / 2 + 8; ++y)
        for (std::uint32_t x = W / 2 - 8; x < W / 2 + 8; ++x) {
            const std::size_t i = (std::size_t{y} * W + x) * 4;
            c.r += px[i];
            c.g += px[i + 1];
            c.b += px[i + 2];
        }
    return {c.r / 256.0, c.g / 256.0, c.b / 256.0};
}

} // namespace

TEST_CASE("gpu texture mips: level follows the footprint and recovers on close-up") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const auto chain = coloured_chain(64);
    REQUIRE(chain.size() == 7);
    TextureOptions opt;
    opt.srgb = false;
    opt.name = "mip_probe";
    auto tex = ctx.create_texture_mips(chain, opt);
    REQUIRE(tex.ok());
    auto plane = ctx.create_mesh(make_plane({40, 40}), "mip_plane").value();
    Material mat;
    mat.unlit = true; // exact texture colours
    mat.base_color_texture = tex.value();
    mat.uv_scale = {64, 64}; // 0.0098 m texels
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.bloom = false;
    s.ssao = false;
    s.auto_exposure = false;
    sr.value()->set_settings(s);

    auto shot = [&](float height, float fov_deg, const Material& m, const char* save = nullptr) {
        auto target = test::make_target(W, H);
        Camera3D cam = Camera3D::look_at({0, height, 0}, {0, 0, 0}, {0, 0, -1});
        cam.fov_y = math::radians(fov_deg);
        cam.z_near = 0.01f;
        sr.value()->begin(cam, DirectionalLight{}, {});
        sr.value()->submit(plane, m, math::Mat4::identity());
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        if (save) test::save_output(save, W, H, px);
        dev.destroy(target);
        return centre(px);
    };
    auto red = [](Rgb c) { return c.r > 150 && c.g < 60 && c.b < 60; };

    // Close: magnified texels sample mip 0 (red).
    const Rgb close = shot(0.3f, 60.0f, mat, "mip_close");
    CHECK(red(close));
    // Far: about 8 texels per pixel, so the GPU samples level 3 or so (no red left).
    const Rgb far = shot(8.0f, 60.0f, mat, "mip_far");
    CHECK(far.r < 20);
    CHECK(far.g + far.b > 150);
    // Back to the close-up: mip 0 again, identical to the first close-up.
    const Rgb again = shot(0.3f, 60.0f, mat);
    CHECK(red(again));
    CHECK(std::abs(again.r - close.r) < 1.0);
    // Zooming (narrow FOV) from the far position also returns to mip 0: the footprint, not the
    // distance, picks the level.
    const Rgb zoomed = shot(8.0f, 1.5f, mat, "mip_zoomed");
    CHECK(red(zoomed));
    // Nearest filtering keeps the footprint-based level choice for minification.
    Material point = mat;
    point.nearest_filter = true;
    const Rgb far_point = shot(8.0f, 60.0f, point);
    CHECK(far_point.r < 20);
    dev.destroy(tex.value());
    ctx.destroy(plane);
}
