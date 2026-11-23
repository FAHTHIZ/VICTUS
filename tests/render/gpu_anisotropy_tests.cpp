// SPDX-License-Identifier: Apache-2.0
// Anisotropic specular (ADR-0232): a brushed-metal sphere's sun highlight stretches along the
// anisotropy direction (the sphere's tangent runs around its vertical axis, so rotation 0
// stretches it sideways and a 90 degree rotation stretches it vertically); strength 0 is exactly
// the isotropic image whatever the rotation.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {
constexpr std::uint32_t W = 128, H = 128;

struct Spread {
    double sx{0}, sy{0}; // brightness-weighted standard deviation of the highlight, in pixels
    double area{0};      // number of highlight pixels
};

// Pixels inside the sphere's silhouette (radius ~51 px at this framing).
bool on_sphere(std::size_t pixel) {
    const int x = static_cast<int>(pixel % W) - int{W / 2}, y = static_cast<int>(pixel / W) - int{H / 2};
    return x * x + y * y < 48 * 48;
}

// Highlight pixels on the sphere: rgb sum above `threshold`; their weighted spread along x and y.
Spread highlight(const std::vector<std::uint8_t>& px, int threshold) {
    double w = 0, mx = 0, my = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const int l = px[i] + px[i + 1] + px[i + 2];
        if (l < threshold || !on_sphere(i / 4)) continue;
        const double x = double(i / 4 % W), y = double(i / 4 / W);
        w += l, mx += l * x, my += l * y;
    }
    Spread s;
    if (w <= 0) return s;
    mx /= w, my /= w;
    double vx = 0, vy = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const int l = px[i] + px[i + 1] + px[i + 2];
        if (l < threshold || !on_sphere(i / 4)) continue;
        const double x = double(i / 4 % W) - mx, y = double(i / 4 / W) - my;
        vx += l * x * x, vy += l * y * y;
        s.area += 1;
    }
    s.sx = std::sqrt(vx / w), s.sy = std::sqrt(vy / w);
    return s;
}
} // namespace

TEST_CASE("gpu anisotropy: the highlight stretches along the rotated tangent (ADR-0232)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 96, 48), "aniso_sphere").value();

    auto shot = [&](float aniso, float rotation, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 0, 1.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(45.0f);
        DirectionalLight sun;
        sun.direction = {-0.25f, -0.3f, -1.0f}; // over the camera's shoulder
        sun.intensity = 3.0f;
        sun.shadows = false;
        Material m;
        m.base_color = {0.8f, 0.8f, 0.82f, 1.0f}; // steel
        m.metallic = 1.0f;
        m.roughness = 0.25f;
        m.anisotropy = aniso;
        m.anisotropy_rotation = rotation;
        auto target = test::make_target(W, H);
        sr.value()->begin(cam, sun, {});
        sr.value()->submit(sphere, m, math::Mat4::identity());
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        test::save_output(save, W, H, px);
        dev.destroy(target);
        return px;
    };

    const auto iso = shot(0.0f, 0.0f, "aniso_off");
    const auto iso_rotated = shot(0.0f, 1.2f, "aniso_off_rotated");
    const auto along = shot(0.9f, 0.0f, "aniso_tangent");
    const auto across = shot(0.9f, 1.5707964f, "aniso_rotated_90");
    // Strength 0: the rotation does nothing.
    CHECK(iso == iso_rotated);
    constexpr int kThreshold = 690; // near-white highlight pixels only (out of 765)
    const Spread a = highlight(iso, kThreshold), b = highlight(along, kThreshold), c = highlight(across, kThreshold);
    MESSAGE("spread x/y iso " << a.sx << "/" << a.sy << " (" << a.area << " px), tangent " << b.sx << "/" << b.sy << " ("
                              << b.area << " px), rotated " << c.sx << "/" << c.sy << " (" << c.area << " px)");
    REQUIRE(a.area > 0);
    REQUIRE(b.area > 0);
    REQUIRE(c.area > 0);
    // Isotropic: roughly round. Tangent direction (around the vertical axis): wide.
    // Rotated a quarter turn: tall.
    CHECK(b.sx > 1.5 * b.sy);
    CHECK(c.sy > 1.5 * c.sx);
    CHECK(b.sx > 1.3 * a.sx);
    CHECK(c.sy > 1.3 * a.sy);
    CHECK(iso != along);
}
