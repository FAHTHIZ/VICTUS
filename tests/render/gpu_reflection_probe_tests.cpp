// SPDX-License-Identifier: Apache-2.0
// Local reflection probes: scene captures land in the right directions, probes replace the
// sky in reflections of objects inside their box, and the runtime captures them on demand.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/reflection_capture.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
struct Marker {
    math::Vec3 direction;
    math::Color color;
};
const Marker kMarkers[] = {
    {{1.0f, 0.6f, 0.4f}, {1, 0, 0, 1}},
    {{-0.5f, -0.7f, 0.6f}, {0, 1, 0, 1}},
    {{0.3f, 0.8f, -0.6f}, {0, 0, 1, 1}},
    {{-0.9f, 0.1f, -0.3f}, {1, 1, 0, 1}},
    {{0.05f, -0.2f, -1.0f}, {1, 0, 1, 1}},
};

math::Vec4 sample(const ImageF& img, math::Vec3 d) {
    const math::Vec2 uv = equirect_uv(math::normalize(d));
    return img.at(std::min(static_cast<std::uint32_t>(uv.x * img.width), img.width - 1),
                  std::min(static_cast<std::uint32_t>(uv.y * img.height), img.height - 1));
}
} // namespace

TEST_CASE("faces_to_equirect maps each face to its axis") {
    std::array<ImageF, 6> faces;
    for (int f = 0; f < 6; ++f) {
        faces[f].width = faces[f].height = 8;
        faces[f].pixels.assign(8 * 8 * 4, static_cast<float>(f + 1));
    }
    const ImageF eq = faces_to_equirect(faces, 64);
    const math::Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) CHECK(sample(eq, axes[f]).x == doctest::Approx(static_cast<float>(f + 1)));
}

TEST_CASE("gpu: capture_equirect puts scene objects in the right directions") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto cube = ctx.create_mesh(make_cube({0.45f, 0.45f, 0.45f}), "marker");
    REQUIRE(cube.ok());
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {0.02f, 0.02f, 0.02f, 1};
    DirectionalLight sun;
    sun.intensity = 0.0f;
    auto img = capture_equirect(ctx, **sr, {0, 0, 0}, 64, [&](SceneRenderer& r, const Camera3D& cam) {
        r.begin(cam, sun, env);
        for (const Marker& m : kMarkers) {
            Material mat;
            mat.unlit = true;
            mat.base_color = m.color;
            r.submit(*cube, mat, math::Mat4::translation(math::normalize(m.direction) * 4.0f));
        }
    });
    REQUIRE(img.ok());
    CHECK(img->width == 256);
    CHECK(img->height == 128);
    for (const Marker& m : kMarkers) {
        const math::Vec4 c = sample(*img, m.direction);
        MESSAGE("marker " << m.color.r << m.color.g << m.color.b << " -> " << c.x << "," << c.y << "," << c.z);
        CHECK(c.x == doctest::Approx(m.color.r).epsilon(0.05));
        CHECK(c.y == doctest::Approx(m.color.g).epsilon(0.05));
        CHECK(c.z == doctest::Approx(m.color.b).epsilon(0.05));
    }
    CHECK(sample(*img, {0, 1, 0.01f}).x < 0.1f); // sky between markers
    CHECK_FALSE((*sr)->capture_mode());
    ctx.destroy(*cube);
}

TEST_CASE("gpu: a reflection probe replaces the sky in reflections inside its box") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto wall = ctx.create_mesh(make_cube({4.0f, 4.0f, 0.2f}), "wall");
    auto ball = ctx.create_mesh(make_uv_sphere(1.0f, 48, 24), "ball");
    REQUIRE(wall.ok());
    REQUIRE(ball.ok());
    Environment env;
    env.vignette = 0.0f;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    Material red;
    red.unlit = true;
    red.base_color = {1, 0, 0, 1};
    Material mirror;
    mirror.metallic = 1.0f;
    mirror.roughness = 0.05f;
    const auto wall_model = math::Mat4::translation({0, 0, 7}); // behind the camera
    auto scene_fill = [&](SceneRenderer& r, const Camera3D& cam, bool with_ball) {
        r.begin(cam, sun, env);
        r.submit(*wall, red, wall_model);
        if (with_ball) r.submit(*ball, mirror, math::Mat4::identity());
    };
    auto img = capture_equirect(ctx, **sr, {0, 0, 0}, 64,
                                [&](SceneRenderer& r, const Camera3D& cam) { scene_fill(r, cam, false); });
    REQUIRE(img.ok());
    auto map = EnvironmentMap::create(ctx, *img, EnvironmentBakeSettings::low());
    REQUIRE(map.ok());

    constexpr std::uint32_t W = 96, H = 96;
    auto shoot = [&](bool probe) {
        scene_fill(**sr, Camera3D::look_at({0, 0, 4}, {0, 0, 0}), true);
        if (probe) {
            ReflectionProbe p;
            p.map = map->get();
            p.box = {{-8, -8, -8}, {8, 8, 8}};
            p.position = {0, 0, 0};
            (*sr)->add_reflection_probe(p);
        }
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const auto sky = shoot(false);
    CHECK((*sr)->stats().probe_draws == 0);
    const auto local = shoot(true);
    CHECK((*sr)->stats().probe_draws == 1);
    CHECK((*sr)->stats().reflection_probes == 1);
    test::save_output("reflection_probe", W, H, local);
    const auto a = pixel(sky, W, W / 2, H / 2), b = pixel(local, W, W / 2, H / 2);
    MESSAGE("mirror centre: sky " << int(channel(a, 0)) << "," << int(channel(a, 1)) << "," << int(channel(a, 2))
                                  << " probe " << int(channel(b, 0)) << "," << int(channel(b, 1)) << ","
                                  << int(channel(b, 2)));
    CHECK(channel(b, 0) > channel(b, 2) + 80); // the red wall behind the camera
    CHECK(channel(a, 2) > channel(a, 0));      // the blue-grey sky
    ctx.destroy(*wall);
    ctx.destroy(*ball);
}

TEST_CASE("gpu: ReflectionProbe components are captured once and recaptured on change") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().get<scene::Transform>(cam)->position = {0, 0, 4};
    s.world().add<scene::CameraComponent>(cam);
    const scene::Entity wall = s.create("Wall");
    s.world().get<scene::Transform>(wall)->position = {0, 0, 7};
    s.world().get<scene::Transform>(wall)->scale = {8, 8, 0.4f};
    auto& wm = s.world().add<scene::MeshRendererComponent>(wall);
    wm.primitive = "cube";
    wm.base_color = {0, 0, 0, 1};
    auto& wo = s.world().add<gameplay::MaterialOverridesComponent>(wall);
    wo.override_emissive = true;
    wo.emissive = {1, 0, 0, 1};
    wo.emissive_strength = 2.0f;
    const scene::Entity ball = s.create("Ball");
    s.world().get<scene::Transform>(ball)->scale = {2, 2, 2};
    auto& bm = s.world().add<scene::MeshRendererComponent>(ball);
    bm.primitive = "sphere";
    bm.metallic = 1.0f;
    bm.roughness = 0.05f;
    const scene::Entity probe = s.create("Probe");
    auto& pc = s.world().add<gameplay::ReflectionProbeComponent>(probe);
    pc.size = {16, 16, 16};
    pc.resolution = 32;
    s.update_transforms();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto stats = runtime::extract_scene(s, cache, **sr);
    CHECK(cache.probe_capture_count() == 1);
    CHECK(stats.reflection_probes == 1);
    (void)runtime::extract_scene(s, cache, **sr);
    CHECK(cache.probe_capture_count() == 1); // unchanged probe is reused
    s.world().get<gameplay::ReflectionProbeComponent>(probe)->revision = 1;
    (void)runtime::extract_scene(s, cache, **sr);
    CHECK(cache.probe_capture_count() == 2);
    s.world().get<scene::Transform>(probe)->position = {0, 0.5f, 0};
    s.update_transforms();
    (void)runtime::extract_scene(s, cache, **sr);
    CHECK(cache.probe_capture_count() == 3);
    constexpr std::uint32_t W = 64, H = 64;
    auto target = test::make_target(W, H);
    auto cmd = ctx.device().begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = ctx.device().read_texture(target).value();
    ctx.device().destroy(target);
    const auto c = pixel(px, W, W / 2, H / 2);
    CHECK(channel(c, 0) > channel(c, 2) + 60);
    s.destroy(probe);
    (void)runtime::extract_scene(s, cache, **sr);
    CHECK(cache.probe_captures().empty());
}
