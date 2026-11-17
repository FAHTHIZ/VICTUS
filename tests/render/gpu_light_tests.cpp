// SPDX-License-Identifier: Apache-2.0
// Clustered forward lighting on the GPU: hundreds of point lights light only their own
// neighbourhood, spot cones light inside and not outside, and scenes author spot lights.
#include "gpu_fixture.hpp"
#include "victus/assets/database.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 192, H = 128;

RenderSettings plain_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = s.ssao = s.bloom = s.fxaa = s.auto_exposure = false;
    return s;
}
Environment dark_env() {
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.ambient_intensity = 0.0f;
    env.vignette = 0.0f;
    return env;
}
DirectionalLight no_sun() {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    return sun;
}
std::vector<std::uint8_t> shoot(SceneRenderer& r) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    return px;
}
// Screen pixel of a world point.
std::pair<std::uint32_t, std::uint32_t> project(const Camera3D& cam, math::Vec3 p) {
    const math::Vec4 c = cam.projection(static_cast<float>(W) / H) * cam.view * math::Vec4(p, 1);
    return {static_cast<std::uint32_t>((c.x / c.w * 0.5f + 0.5f) * W), static_cast<std::uint32_t>((0.5f - c.y / c.w * 0.5f) * H)};
}
int lum(std::uint32_t px) { return channel(px, 0) + channel(px, 1) + channel(px, 2); }
} // namespace

TEST_CASE("gpu: hundreds of clustered point lights light their own neighbourhood") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({40, 40}), "floor");
    REQUIRE(plane.ok());
    const Camera3D cam = Camera3D::look_at({0, 14, 12}, {0, 0, -1}, {0, 1, 0});
    (*sr)->begin(cam, no_sun(), dark_env());
    Material floor;
    floor.base_color = {0.8f, 0.8f, 0.8f, 1};
    floor.roughness = 0.9f;
    (*sr)->submit(*plane, floor, math::Mat4::identity());
    // A 16 x 16 grid of small lights; one cell in the middle is left dark.
    int added = 0;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            if (x == 7 && z == 7) continue;
            PointLight l;
            l.position = {(static_cast<float>(x) - 7.5f) * 2.0f, 0.4f, (static_cast<float>(z) - 7.5f) * 2.0f};
            l.color = (x + z) % 2 ? math::Color{1, 0.2f, 0.1f, 1} : math::Color{0.1f, 0.4f, 1, 1};
            l.intensity = 3.0f;
            l.range = 1.2f;
            (*sr)->add_point_light(l);
            ++added;
        }
    const auto img = shoot(**sr);
    test::save_output("clustered_lights", W, H, img);
    const SceneStats& st = (*sr)->stats();
    MESSAGE("lights " << st.point_lights << " indices " << st.light_indices << " max/cluster " << st.max_cluster_lights);
    CHECK(added == 255);
    CHECK(st.point_lights > 150); // the rest are outside the view
    CHECK(st.max_cluster_lights < 24);
    // Directly under a light: lit in its colour; the gap left at (7, 7) and between lights: dark.
    const auto [rx, ry] = project(cam, {(8 - 7.5f) * 2.0f, 0, (7 - 7.5f) * 2.0f}); // x+z odd: red
    const auto [bx, by] = project(cam, {(8 - 7.5f) * 2.0f, 0, (8 - 7.5f) * 2.0f}); // even: blue
    const auto [gx, gy] = project(cam, {(7 - 7.5f) * 2.0f, 0, (7 - 7.5f) * 2.0f});
    const auto red = pixel(img, W, rx, ry), blue = pixel(img, W, bx, by), gap = pixel(img, W, gx, gy);
    MESSAGE("red " << lum(red) << " blue " << lum(blue) << " gap " << lum(gap));
    CHECK(channel(red, 0) > 2 * channel(red, 2));
    CHECK(channel(blue, 2) > 2 * channel(blue, 0));
    CHECK(lum(red) > 120);
    CHECK(lum(gap) < 20);
    ctx.destroy(*plane);
}

TEST_CASE("gpu: spot lights light inside the cone only") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({20, 20}), "floor");
    REQUIRE(plane.ok());
    const Camera3D cam = Camera3D::look_at({0, 10, 6}, {0, 0, 0}, {0, 1, 0});
    (*sr)->begin(cam, no_sun(), dark_env());
    Material floor;
    floor.base_color = {0.8f, 0.8f, 0.8f, 1};
    floor.roughness = 0.9f;
    (*sr)->submit(*plane, floor, math::Mat4::identity());
    SpotLight s;
    s.position = {0, 4, 0};
    s.direction = {0, -1, 0};
    s.intensity = 60.0f;
    s.range = 12.0f;
    s.inner_angle = 0.25f; // radius on the floor ~1.0
    s.outer_angle = 0.35f; // ~1.46
    (*sr)->add_spot_light(s);
    const auto img = shoot(**sr);
    test::save_output("spot_light", W, H, img);
    CHECK((*sr)->stats().spot_lights == 1);
    const auto [cx, cy] = project(cam, {0, 0, 0});
    const auto [ex, ey] = project(cam, {2.5f, 0, 0}); // well outside the outer cone, within range
    const auto [mx, my] = project(cam, {1.25f, 0, 0}); // between inner and outer: partial
    const int centre = lum(pixel(img, W, cx, cy)), edge = lum(pixel(img, W, ex, ey)), mid = lum(pixel(img, W, mx, my));
    MESSAGE("spot centre " << centre << " penumbra " << mid << " outside " << edge);
    CHECK(centre > 300);
    CHECK(edge < 10);
    CHECK(mid > edge);
    CHECK(mid < centre);
    ctx.destroy(*plane);
}

TEST_CASE("gpu: scenes author spot lights through LightComponent") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_spot_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "assets");
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    scene::Scene authored;
    const auto floor = authored.create("Floor");
    authored.world().add<scene::MeshRendererComponent>(floor).primitive = "plane";
    authored.world().get<scene::Transform>(floor)->scale = {10, 1, 10};
    const auto lamp = authored.create("Lamp");
    authored.world().get<scene::Transform>(lamp)->position = {0, 4, 0};
    authored.world().get<scene::Transform>(lamp)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90)); // -Z -> down
    auto& l = authored.world().add<scene::LightComponent>(lamp);
    l.type = scene::LightComponent::Type::Spot;
    l.intensity = 60.0f;
    l.range = 12.0f;
    l.inner_angle_degrees = 14.0f;
    l.outer_angle_degrees = 20.0f;
    const auto cam = authored.create("Camera");
    authored.world().get<scene::Transform>(cam)->position = {0, 10, 6};
    authored.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, std::atan2(-10.0f, 6.0f));
    authored.world().add<scene::CameraComponent>(cam);
    REQUIRE(authored.save_file(project / "assets" / "spot.vscene").ok());
    scene::Scene s;
    REQUIRE(s.load_file(project / "assets" / "spot.vscene").ok());
    const auto* loaded = s.world().get<scene::LightComponent>(s.find_by_name("Lamp"));
    REQUIRE(loaded);
    CHECK(loaded->type == scene::LightComponent::Type::Spot);
    CHECK(loaded->outer_angle_degrees == doctest::Approx(20.0f));
    s.update_transforms();
    runtime::GpuAssetCache cache(ctx, db->get());
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    runtime::SceneViewSettings view;
    view.environment = dark_env();
    (void)runtime::extract_scene(s, cache, **sr, view);
    const auto img = shoot(**sr);
    CHECK((*sr)->stats().spot_lights == 1);
    const int centre = lum(pixel(img, W, W / 2, H / 2)), corner = lum(pixel(img, W, 8, H - 8));
    MESSAGE("authored spot centre " << centre << " corner " << corner);
    CHECK(centre > 200);
    CHECK(corner < 20);
    std::filesystem::remove_all(project);
}
