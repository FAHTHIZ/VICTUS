// SPDX-License-Identifier: Apache-2.0
// Subsurface scattering (ADR-0233): a sphere lit from behind glows through in its subsurface
// colour when thin and stays dark when thick; lit from the side, the wrapped diffuse carries
// light past the terminator; black subsurface is exactly the plain image at any thickness.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {
constexpr std::uint32_t W = 128, H = 128;

struct Shot {
    std::vector<std::uint8_t> px;
    // Mean of channel `ch` (0 r, 1 g, 2 b) over the box [x0, x1) x [y0, y1).
    double mean(int ch, int x0, int x1, int y0, int y1) const {
        double sum = 0.0;
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) sum += px[(std::size_t(y) * W + std::size_t(x)) * 4 + std::size_t(ch)], ++n;
        return n ? sum / n : 0.0;
    }
    double centre(int ch) const { return mean(ch, 56, 72, 56, 72); }
    // A band on the unlit side just past the terminator of a sphere lit from +x.
    double past_terminator(int ch) const { return mean(ch, 50, 58, 52, 76); }
};
} // namespace

TEST_CASE("gpu subsurface: thin parts transmit back light and the terminator softens (ADR-0233)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 64, 32), "sss_sphere").value();

    auto shot = [&](math::Vec3 sun_dir, math::Vec3 sss, float thickness, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 0, 1.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(45.0f);
        DirectionalLight sun;
        sun.direction = sun_dir;
        sun.intensity = 4.0f;
        sun.shadows = false;
        Material m;
        m.base_color = {0.75f, 0.55f, 0.45f, 1.0f}; // skin tone
        m.roughness = 0.6f;
        m.subsurface_color = sss;
        m.subsurface_thickness = thickness;
        auto target = test::make_target(W, H);
        sr.value()->begin(cam, sun, {});
        sr.value()->submit(sphere, m, math::Mat4::identity());
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value()};
        test::save_output(save, W, H, out.px);
        dev.destroy(target);
        return out;
    };

    const math::Vec3 back_light{0.0f, -0.1f, 1.0f}; // from behind the sphere towards the camera
    const math::Vec3 side_light{-1.0f, 0.0f, 0.0f}; // from +x
    const math::Vec3 blood{1.0f, 0.25f, 0.12f};
    const Shot plain = shot(back_light, {0, 0, 0}, 0.5f, "sss_off");
    const Shot plain_thin = shot(back_light, {0, 0, 0}, 0.0f, "sss_off_thin");
    const Shot thin = shot(back_light, blood, 0.1f, "sss_back_thin");
    const Shot thick = shot(back_light, blood, 1.0f, "sss_back_thick");
    const Shot side_plain = shot(side_light, {0, 0, 0}, 1.0f, "sss_side_off");
    const Shot side_wrap = shot(side_light, blood, 1.0f, "sss_side_wrap");
    MESSAGE("back-lit centre r/b plain " << plain.centre(0) << "/" << plain.centre(2) << ", thin " << thin.centre(0) << "/"
                                         << thin.centre(2) << ", thick " << thick.centre(0) << "/" << thick.centre(2)
                                         << "; past terminator r plain " << side_plain.past_terminator(0) << ", wrapped "
                                         << side_wrap.past_terminator(0));
    CHECK(plain.px == plain_thin.px); // no subsurface colour: thickness does nothing
    // Thin: the back light shines through, tinted by the subsurface colour.
    CHECK(thin.centre(0) > plain.centre(0) + 40.0);
    CHECK(thin.centre(0) - plain.centre(0) > 2.0 * (thin.centre(2) - plain.centre(2)));
    // Thick (1): no transmission, so the face towards the camera stays as dark as the plain one.
    CHECK(thick.centre(0) < plain.centre(0) + 6.0);
    // Wrapped diffuse lights the band just past the terminator.
    CHECK(side_wrap.past_terminator(0) > side_plain.past_terminator(0) + 10.0);
}
