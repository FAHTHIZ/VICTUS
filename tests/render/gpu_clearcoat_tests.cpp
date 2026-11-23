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

// ADR-0248: rectangle area lights shade the clear coat and sheen layers too. Before, a coated or
// sheen material under an area light rendered exactly like the plain one.
TEST_CASE("gpu clear coat and sheen: rectangle area lights shade both layers (ADR-0248)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto sphere = ctx.create_mesh(make_uv_sphere(0.5f, 64, 32), "area_coat_sphere").value();

    auto shot = [&](const Material& m, const char* save) {
        RenderSettings s = RenderSettings::preset(QualityTier::High);
        s.taa = s.fxaa = s.bloom = s.ssao = s.auto_exposure = false;
        s.render_scale = 1.0f;
        sr.value()->set_settings(s);
        Camera3D cam = Camera3D::look_at({0, 0, 1.6f}, {0, 0, 0}, {0, 1, 0});
        cam.fov_y = math::radians(45.0f);
        DirectionalLight sun;
        sun.intensity = 0.0f;
        sun.shadows = false;
        Environment env;
        env.draw_sky = false;
        env.clear_color = {0, 0, 0, 1};
        env.ambient_intensity = 0.0f;
        env.vignette = 0.0f;
        AreaLight panel;
        panel.position = {0.7f, 0.9f, 1.4f}; // behind the camera, up and to the right
        panel.direction = math::normalize(math::Vec3{0, 0, 0} - panel.position);
        // 1 m across, so the glossy coat's mirror image of it covers a dozen pixels on the sphere.
        panel.width = panel.height = 1.0f;
        panel.intensity = 6.0f;
        panel.range = 10.0f;
        auto target = test::make_target(W, H);
        sr.value()->begin(cam, sun, env);
        sr.value()->submit(sphere, m, math::Mat4::identity());
        sr.value()->add_area_light(panel);
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Shot out{dev.read_texture(target).value()};
        test::save_output(save, W, H, out.px);
        dev.destroy(target);
        return out;
    };
    const auto sphere_sum = [](const Shot& s) {
        long sum = 0;
        for (std::size_t i = 0; i < s.px.size(); i += 4)
            if (on_sphere(i / 4)) sum += s.px[i] + s.px[i + 1] + s.px[i + 2];
        return sum;
    };

    Material paint;
    paint.base_color = {0.05f, 0.12f, 0.35f, 1.0f};
    paint.roughness = 0.75f;
    const Shot plain = shot(paint, "area_clearcoat_off");
    Material coated_m = paint;
    coated_m.clearcoat = 1.0f;
    coated_m.clearcoat_roughness = 0.03f;
    const Shot coated = shot(coated_m, "area_clearcoat_on");
    coated_m.clearcoat_roughness = 0.45f;
    const Shot satin = shot(coated_m, "area_clearcoat_satin");
    MESSAGE("area light peak off/on/satin " << plain.peak() << " " << coated.peak() << " " << satin.peak()
                                            << ", pixels above base peak + 30: " << coated.count_above(plain.peak() + 30)
                                            << " " << satin.count_above(plain.peak() + 30));
    CHECK(coated.peak() > plain.peak() + 60);
    CHECK(coated.count_above(plain.peak() + 30) > 0);
    CHECK(plain.count_above(plain.peak() + 30) == 0);
    CHECK(satin.peak() < coated.peak());
    CHECK(satin.peak() > plain.peak() + 20);
    // Unlike the sun, the 1 m panel's mirror image is itself large (measured 906 pixels above the
    // base peak + 30 for the glossy coat): the rougher coat spreads it dimmer, not wider.
    CHECK(coated.count_above(plain.peak() + 30) > 200);
    CHECK(satin.count_above(plain.peak() + 30) > 0);
    const std::size_t i = (std::size_t{H / 2} * W + W / 2 - 22) * 4 + 2;
    CHECK(int{coated.px[i]} <= int{plain.px[i]} + 2);

    Material cloth = paint;
    cloth.base_color = {0.25f, 0.05f, 0.08f, 1.0f}; // dark red velvet
    cloth.roughness = 0.9f;
    const Shot bare = shot(cloth, "area_sheen_off");
    cloth.sheen_color = {1.0f, 0.8f, 0.8f};
    cloth.sheen_roughness = 0.3f;
    const Shot velvet = shot(cloth, "area_sheen_on");
    const long bare_sum = sphere_sum(bare), velvet_sum = sphere_sum(velvet);
    MESSAGE("area light sphere sum: no sheen " << bare_sum << ", sheen " << velvet_sum);
    CHECK(velvet_sum > bare_sum + bare_sum / 10); // the sheen lobe adds light
    ctx.destroy(sphere);
}
