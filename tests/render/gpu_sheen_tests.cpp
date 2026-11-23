// SPDX-License-Identifier: Apache-2.0
// Sheen (ADR-0231): a dark fabric sphere lit from behind the camera gains a soft bright rim at
// grazing angles (the Charlie lobe) while its centre barely changes; a rougher sheen spreads the
// rim inwards; black sheen is exactly the plain metallic-roughness image.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {
constexpr std::uint32_t W = 128, H = 128;

int radius2(std::size_t pixel) {
    const int x = static_cast<int>(pixel % W) - int{W / 2}, y = static_cast<int>(pixel / W) - int{H / 2};
    return x * x + y * y;
}

struct Shot {
    std::vector<std::uint8_t> px;
    // Mean of (r + g + b) over pixels whose distance from the centre is in [r0, r1).
    double mean(int r0, int r1) const {
        double sum = 0.0;
        int n = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            const int d = radius2(i / 4);
            if (d < r0 * r0 || d >= r1 * r1) continue;
            sum += px[i] + px[i + 1] + px[i + 2];
            ++n;
        }
        return n ? sum / n : 0.0;
    }
    double rim() const { return mean(40, 47); }   // inside the silhouette (radius ~51 px)
    double centre() const { return mean(0, 14); }
};
} // namespace

TEST_CASE("gpu sheen: fabric gains a grazing-angle rim without brightening its centre (ADR-0231)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 64, 32), "sheen_sphere").value();

    auto shot = [&](math::Vec3 sheen, float sheen_roughness, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 0, 1.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(45.0f);
        DirectionalLight sun;
        sun.direction = {0.0f, -0.15f, -1.0f}; // from just above and behind the camera
        sun.intensity = 3.0f;
        sun.shadows = false;
        Material m;
        m.base_color = {0.06f, 0.01f, 0.03f, 1.0f}; // dark burgundy velvet
        m.roughness = 0.9f;
        m.sheen_color = sheen;
        m.sheen_roughness = sheen_roughness;
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

    const Shot plain = shot({0, 0, 0}, 0.3f, "sheen_off");
    const Shot plain_rough = shot({0, 0, 0}, 0.9f, "sheen_off_rough");
    const Shot velvet = shot({1.0f, 0.55f, 0.7f}, 0.3f, "sheen_velvet");
    const Shot wool = shot({1.0f, 0.55f, 0.7f}, 0.9f, "sheen_wool");
    MESSAGE("rim off/velvet/wool " << plain.rim() << " " << velvet.rim() << " " << wool.rim() << ", centre "
                                   << plain.centre() << " " << velvet.centre() << " " << wool.centre());
    // No sheen: sheen roughness has no effect at all.
    CHECK(plain.px == plain_rough.px);
    // The rim lights up strongly; the face-on centre stays close to the plain base.
    CHECK(velvet.rim() > plain.rim() + 40.0);
    CHECK(velvet.rim() > velvet.centre());
    CHECK(velvet.centre() < plain.centre() + 30.0);
    // A rough sheen (wool) spreads the haze across the face: brighter centre than the tight velvet rim.
    CHECK(wool.centre() > velvet.centre());
    CHECK(wool.rim() > plain.rim() + 20.0);
    // The tint follows the sheen colour: red over blue on the rim.
    std::size_t red = 0, blue = 0;
    for (std::size_t i = 0; i < velvet.px.size(); i += 4)
        if (radius2(i / 4) >= 40 * 40 && radius2(i / 4) < 47 * 47) red += velvet.px[i], blue += velvet.px[i + 2];
    CHECK(red > blue);
}
