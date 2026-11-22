// SPDX-License-Identifier: Apache-2.0
// Clear coat (ADR-0203): a rough base under a glossy coat gains a tight, bright highlight from the
// sun while the diffuse body dims slightly (the light the coat reflects no longer reaches it);
// with clearcoat = 0 the image is exactly the plain metallic-roughness result.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {
constexpr std::uint32_t W = 128, H = 128;

// Only pixels well inside the sphere's silhouette (radius ~51 px at this camera) count, so the
// bright sky behind it never does.
bool on_sphere(std::size_t pixel) {
    const int x = static_cast<int>(pixel % W) - int{W / 2}, y = static_cast<int>(pixel / W) - int{H / 2};
    return x * x + y * y < 44 * 44;
}

struct Shot {
    std::vector<std::uint8_t> px;
    int peak() const {
        int m = 0;
        for (std::size_t i = 0; i < px.size(); i += 4)
            if (on_sphere(i / 4)) m = std::max(m, int{px[i + 1]});
        return m;
    }
    // Sphere pixels at least `level` bright in green: the size of the highlight.
    int count_above(int level) const {
        int n = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) n += on_sphere(i / 4) && px[i + 1] >= level;
        return n;
    }
};
} // namespace

TEST_CASE("gpu clear coat: a glossy layer adds a sharp highlight over a rough base (ADR-0203)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 64, 32), "coat_sphere").value();

    auto shot = [&](float coat, float coat_roughness, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 0, 1.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(45.0f);
        DirectionalLight sun;
        sun.direction = {-0.35f, -0.45f, -1.0f}; // from behind the camera, up and to the right
        sun.intensity = 2.0f;
        sun.shadows = false;
        Material m;
        m.base_color = {0.05f, 0.12f, 0.35f, 1.0f}; // dark blue paint
        m.roughness = 0.75f;
        m.clearcoat = coat;
        m.clearcoat_roughness = coat_roughness;
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

    const Shot plain = shot(0.0f, 0.03f, "clearcoat_off");
    const Shot coated = shot(1.0f, 0.03f, "clearcoat_on");
    const Shot satin = shot(1.0f, 0.45f, "clearcoat_satin");
    MESSAGE("peak off/on/satin " << plain.peak() << " " << coated.peak() << " " << satin.peak()
                                 << ", pixels above base peak + 30: " << coated.count_above(plain.peak() + 30) << " "
                                 << satin.count_above(plain.peak() + 30));
    // The coat's sharp reflection of the sun is far brighter than the rough base's broad lobe.
    CHECK(coated.peak() > plain.peak() + 60);
    CHECK(coated.count_above(plain.peak() + 30) > 0);
    CHECK(plain.count_above(plain.peak() + 30) == 0);
    // A rougher coat spreads the highlight: dimmer peak than the glossy coat, still above the base.
    CHECK(satin.peak() < coated.peak());
    CHECK(satin.peak() > plain.peak() + 20);
    CHECK(satin.count_above(plain.peak() + 30) > coated.count_above(plain.peak() + 30));
    // Away from the highlight the coat darkens the body slightly (energy taken by the coat's
    // Fresnel) and never brightens it: compare the sphere's centre-left, off the reflection.
    const std::size_t i = (std::size_t{H / 2} * W + W / 2 - 22) * 4 + 2;
    CHECK(int{coated.px[i]} <= int{plain.px[i]} + 2);
}
