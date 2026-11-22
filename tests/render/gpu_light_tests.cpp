// SPDX-License-Identifier: Apache-2.0
// Clustered forward lighting on the GPU: hundreds of point lights light only their own
// neighbourhood, spot cones light inside and not outside, and scenes author spot lights.
#include "gpu_fixture.hpp"
#include "victus/assets/database.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <algorithm>
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
    CHECK((*sr)->stats().local_shadow_lights == 1); // LightComponent::shadows (default on) casts
    const int centre = lum(pixel(img, W, W / 2, H / 2)), corner = lum(pixel(img, W, 8, H - 8));
    MESSAGE("authored spot centre " << centre << " corner " << corner);
    CHECK(centre > 200);
    CHECK(corner < 20);
    std::filesystem::remove_all(project);
}

TEST_CASE("gpu: the sky background can differ from the ambient terms") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    RenderSettings s = plain_settings();
    (*sr)->set_settings(s);
    // Looking up: the whole frame is sky.
    const Camera3D cam = Camera3D::look_at({0, 1, 0}, {0, 100, -1}, {0, 0, -1});
    Environment env;
    env.vignette = 0.0f;
    env.tonemap = false;
    env.draw_sky = true;
    env.clear_color = {0, 0, 0, 1};
    env.sky_zenith = env.sky_horizon = env.ground = {0.02f, 0.02f, 0.02f, 1}; // dim ambient
    (*sr)->begin(cam, no_sun(), env);
    const auto dim = shoot(**sr);
    env.sky_background = true;
    env.background_zenith = env.background_horizon = {0.1f, 0.3f, 0.9f, 1};
    (*sr)->begin(cam, no_sun(), env);
    const auto bright = shoot(**sr);
    const std::uint32_t c = pixel(dim, W, W / 2, H / 2), b = pixel(bright, W, W / 2, H / 2);
    CHECK(lum(c) < 150); // sum of three sRGB channels; 0.02 linear is ~39 each
    CHECK(channel(b, 2) > 150);
    CHECK(channel(b, 2) > channel(b, 0) + 60);
}

TEST_CASE("gpu: shadow-only draws cast a shadow but are not drawn") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({40, 40}), "floor");
    auto cube = ctx.create_mesh(make_cube({1.5f, 0.5f, 1.5f}), "blocker");
    REQUIRE(plane.ok());
    REQUIRE(cube.ok());
    const Camera3D cam = Camera3D::look_at({0, 10, 10}, {0, 0, 0}, {0, 1, 0});
    DirectionalLight sun;
    sun.direction = {0, -1, 0};
    sun.intensity = 3.0f;
    Environment env = dark_env();
    env.ambient_intensity = 0.05f;
    Material floor;
    floor.base_color = {0.8f, 0.8f, 0.8f, 1};
    floor.roughness = 0.9f;
    Material red;
    red.base_color = {0.9f, 0.05f, 0.05f, 1};
    const auto model = math::Mat4::translation({0, 3, 0});
    const auto frame = [&](bool shadow_only) {
        (*sr)->begin(cam, sun, env);
        (*sr)->submit(*plane, floor, math::Mat4::identity());
        Material m = red;
        m.shadow_only = shadow_only;
        (*sr)->submit(*cube, m, model);
        return shoot(**sr);
    };
    const auto drawn = frame(false), hidden = frame(true);
    test::save_output("shadow_only", W, H, hidden);
    const auto [bx, by] = project(cam, {0, 3.5f, 0});  // the cube's top face
    const auto [sx, sy] = project(cam, {0, 0, 1.2f});  // floor under the cube, seen past it
    const auto [lx, ly] = project(cam, {5, 0, 1.2f});  // floor in the sun
    const std::uint32_t box_drawn = pixel(drawn, W, bx, by), box_hidden = pixel(hidden, W, bx, by);
    const std::uint32_t shade = pixel(hidden, W, sx, sy), sunny = pixel(hidden, W, lx, ly);
    MESSAGE("box " << lum(box_drawn) << "/" << lum(box_hidden) << " shade " << lum(shade) << " sun " << lum(sunny));
    CHECK(channel(box_drawn, 0) > channel(box_drawn, 1) + 60); // drawn: the red cube
    CHECK(channel(box_hidden, 0) < channel(box_hidden, 1) + 20); // shadow-only: the grey floor shows through
    CHECK(lum(shade) + 120 < lum(sunny));                        // ...and the floor under it is still in shadow
    ctx.destroy(*plane);
    ctx.destroy(*cube);
}

namespace {
Material matte_floor() {
    Material m;
    m.base_color = {0.8f, 0.8f, 0.8f, 1};
    m.roughness = 0.9f;
    return m;
}
// Pixels brighter than half of the image's own peak: the size of a highlight, independent of
// how bright it is (a larger source of the same intensity has a dimmer, larger reflection).
int highlight_pixels(const std::vector<std::uint8_t>& img) {
    int peak = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) peak = std::max(peak, lum(pixel(img, W, x, y)));
    const int threshold = peak / 2;
    int n = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) n += lum(pixel(img, W, x, y)) > threshold ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("gpu: area lights are one-sided, shaped by their rectangle and match a point light from afar (ADR-0194)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({20, 20}), "floor");
    REQUIRE(plane.ok());
    const Camera3D cam = Camera3D::look_at({0, 10, 6}, {0, 0, 0}, {0, 1, 0});
    const auto render_with = [&](const auto& add) {
        (*sr)->begin(cam, no_sun(), dark_env());
        (*sr)->submit(*plane, matte_floor(), math::Mat4::identity());
        add(**sr);
        return shoot(**sr);
    };
    // A long, thin strip 1 m above the floor: it spreads light along its width axis.
    AreaLight strip;
    strip.position = {0, 1, 0};
    strip.direction = {0, -1, 0};
    strip.right = {1, 0, 0};
    strip.width = 4.0f;
    strip.height = 0.3f;
    strip.intensity = 3.0f;
    strip.range = 12.0f;
    const auto along_x = render_with([&](SceneRenderer& r) { r.add_area_light(strip); });
    CHECK((*sr)->stats().area_lights == 1);
    CHECK((*sr)->stats().spot_lights == 0);
    test::save_output("area_light_strip", W, H, along_x);
    const auto [ex, ey] = project(cam, {1.8f, 0, 0});
    const auto [ez, ezy] = project(cam, {0, 0, 1.8f});
    const int x_end = lum(pixel(along_x, W, ex, ey)), z_end = lum(pixel(along_x, W, ez, ezy));
    MESSAGE("strip along x: x end " << x_end << " z end " << z_end);
    CHECK(x_end > 2 * z_end);
    // Turning its width to Z swaps them (the width axis survives the GPU packing).
    strip.right = {0, 0, 1};
    const auto along_z = render_with([&](SceneRenderer& r) { r.add_area_light(strip); });
    const int x_end2 = lum(pixel(along_z, W, ex, ey)), z_end2 = lum(pixel(along_z, W, ez, ezy));
    MESSAGE("strip along z: x end " << x_end2 << " z end " << z_end2);
    CHECK(z_end2 > 2 * x_end2);
    // One-sided: facing up, the floor stays dark.
    strip.direction = {0, 1, 0};
    const auto up = render_with([&](SceneRenderer& r) { r.add_area_light(strip); });
    CHECK(lum(pixel(up, W, W / 2, H / 2)) < 6);
    // Far away a small panel is as bright as a point light of the same intensity.
    AreaLight small;
    small.position = {0, 4, 0};
    small.direction = {0, -1, 0};
    small.width = small.height = 0.2f;
    small.intensity = 30.0f;
    small.range = 30.0f;
    PointLight point;
    point.position = small.position;
    point.intensity = small.intensity;
    point.range = small.range;
    const auto [cx, cy] = project(cam, {0, 0, 0});
    const int a = lum(pixel(render_with([&](SceneRenderer& r) { r.add_area_light(small); }), W, cx, cy));
    const int p = lum(pixel(render_with([&](SceneRenderer& r) { r.add_point_light(point); }), W, cx, cy));
    MESSAGE("small panel " << a << " point " << p);
    CHECK(a > 40);
    CHECK(static_cast<float>(a) > 0.85f * static_cast<float>(p));
    CHECK(static_cast<float>(a) < 1.15f * static_cast<float>(p));
    ctx.destroy(*plane);
}

TEST_CASE("gpu: bulb radius and area lights widen glossy highlights") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({20, 20}), "floor");
    REQUIRE(plane.ok());
    const Camera3D cam = Camera3D::look_at({0, 2, 5}, {0, 0, -1}, {0, 1, 0});
    Material glossy;
    glossy.base_color = {0.9f, 0.9f, 0.9f, 1};
    glossy.metallic = 1.0f;
    glossy.roughness = 0.06f;
    const auto render_with = [&](const auto& add) {
        (*sr)->begin(cam, no_sun(), dark_env());
        (*sr)->submit(*plane, glossy, math::Mat4::identity());
        add(**sr);
        return shoot(**sr);
    };
    PointLight point;
    point.position = {0, 2, -2};
    point.intensity = 4.0f;
    point.range = 15.0f;
    const auto tiny = render_with([&](SceneRenderer& r) { r.add_point_light(point); });
    test::save_output("area_light_glossy_point", W, H, tiny);
    point.radius = 0.5f;
    const auto bulb = render_with([&](SceneRenderer& r) { r.add_point_light(point); });
    test::save_output("area_light_glossy_bulb", W, H, bulb);
    AreaLight panel;
    panel.position = point.position;
    panel.direction = {0, -1, 0};
    panel.width = panel.height = 1.5f;
    panel.intensity = point.intensity;
    panel.range = point.range;
    const auto area = render_with([&](SceneRenderer& r) { r.add_area_light(panel); });
    test::save_output("area_light_glossy", W, H, area);
    const int n_tiny = highlight_pixels(tiny), n_bulb = highlight_pixels(bulb), n_area = highlight_pixels(area);
    MESSAGE("highlight pixels: point " << n_tiny << " bulb " << n_bulb << " panel " << n_area);
    CHECK(n_tiny > 0);
    CHECK(n_bulb > 2 * n_tiny);
    CHECK(n_area > 4 * n_tiny);
    // The panel's reflection sits on the mirror point.
    const auto [mx, my] = project(cam, {0, 0, 1.5f}); // mirror of (0, 2, -2) seen from (0, 2, 5): halfway, z = 1.5
    CHECK(lum(pixel(area, W, mx, my)) > 40);
    ctx.destroy(*plane);
}

TEST_CASE("gpu: scenes author area lights through LightComponent; baked ones stay lit until a lightmap holds them") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_area_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "assets");
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    scene::Scene authored;
    const auto floor = authored.create("Floor");
    authored.world().add<scene::MeshRendererComponent>(floor).primitive = "plane";
    authored.world().get<scene::Transform>(floor)->scale = {10, 1, 10};
    const auto panel = authored.create("Panel");
    authored.world().get<scene::Transform>(panel)->position = {0, 2, 0};
    authored.world().get<scene::Transform>(panel)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90)); // -Z -> down
    auto& l = authored.world().add<scene::LightComponent>(panel);
    l.type = scene::LightComponent::Type::Area;
    l.intensity = 30.0f;
    l.width = 3.0f;
    l.height = 0.5f;
    l.baked = true;
    const auto bulb = authored.create("Bulb");
    authored.world().get<scene::Transform>(bulb)->position = {3, 1, 0};
    auto& b = authored.world().add<scene::LightComponent>(bulb);
    b.type = scene::LightComponent::Type::Point;
    b.radius = 0.25f;
    const auto cam = authored.create("Camera");
    authored.world().get<scene::Transform>(cam)->position = {0, 10, 6};
    authored.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, std::atan2(-10.0f, 6.0f));
    authored.world().add<scene::CameraComponent>(cam);
    REQUIRE(authored.save_file(project / "assets" / "area.vscene").ok());
    scene::Scene s;
    REQUIRE(s.load_file(project / "assets" / "area.vscene").ok());
    const auto* loaded = s.world().get<scene::LightComponent>(s.find_by_name("Panel"));
    REQUIRE(loaded);
    CHECK(loaded->type == scene::LightComponent::Type::Area);
    CHECK(loaded->width == doctest::Approx(3.0f));
    CHECK(loaded->height == doctest::Approx(0.5f));
    CHECK(loaded->baked);
    CHECK(s.world().get<scene::LightComponent>(s.find_by_name("Bulb"))->radius == doctest::Approx(0.25f));
    s.update_transforms();
    runtime::GpuAssetCache cache(ctx, db->get());
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    runtime::SceneViewSettings view;
    view.environment = dark_env();
    (void)runtime::extract_scene(s, cache, **sr, view);
    const auto img = shoot(**sr);
    // No lightmap is loaded, so the baked panel is still lit in real time.
    CHECK((*sr)->stats().area_lights == 1);
    CHECK((*sr)->stats().point_lights == 2);
    const int centre = lum(pixel(img, W, W / 2, H / 2)), corner = lum(pixel(img, W, 8, H - 8));
    MESSAGE("authored panel centre " << centre << " corner " << corner);
    CHECK(centre > 150);
    CHECK(corner < 40);
    std::filesystem::remove_all(project);
}

namespace {
// 64x64 cookie: top-left white, bottom-left red, right half black.
Image quadrant_cookie() {
    Image img;
    img.width = img.height = 64;
    img.pixels.resize(64 * 64 * 4);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            std::uint8_t* p = &img.pixels[(y * 64 + x) * 4];
            const bool left = x < 32, top = y < 32;
            p[0] = left ? 255 : 0;
            p[1] = p[2] = (left && top) ? 255 : 0;
            p[3] = 255;
        }
    return img;
}
} // namespace

TEST_CASE("gpu: spot light cookies project their image through the cone (ADR-0195)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    auto plane = ctx.create_mesh(make_plane({20, 20}), "floor");
    auto cube = ctx.create_mesh(make_cube({0.3f, 0.05f, 0.3f}), "blocker");
    REQUIRE(plane.ok());
    REQUIRE(cube.ok());
    Material floor;
    floor.base_color = {0.8f, 0.8f, 0.8f, 1};
    floor.roughness = 0.9f;
    const Camera3D cam = Camera3D::look_at({0, 10, 6}, {0, 0, 0}, {0, 1, 0});
    constexpr std::uint64_t kCookie = 77;
    REQUIRE((*sr)->set_cookie(kCookie, quadrant_cookie()).ok());
    CHECK((*sr)->has_cookie(kCookie));
    CHECK_FALSE((*sr)->set_cookie(0, quadrant_cookie()).ok());
    SpotLight s;
    s.position = {0, 4, 0};
    s.direction = {0, -1, 0};
    s.right = {1, 0, 0}; // image top towards -Z (up = right x direction)
    s.intensity = 60.0f;
    s.range = 12.0f;
    s.inner_angle = 0.6f;
    s.outer_angle = 0.7f;
    const auto render_with = [&](const SpotLight& light, bool blocker = false) {
        (*sr)->begin(cam, no_sun(), dark_env());
        (*sr)->submit(*plane, floor, math::Mat4::identity());
        if (blocker) (*sr)->submit(*cube, floor, math::Mat4::translation({-0.4f, 2.4f, -0.4f}));
        (*sr)->add_spot_light(light);
        return shoot(**sr);
    };
    const auto at = [&](const std::vector<std::uint8_t>& img, math::Vec3 p) {
        const auto [x, y] = project(cam, p);
        return pixel(img, W, x, y);
    };
    const math::Vec3 top_left{-1, 0, -1}, bottom_left{-1, 0, 1}, top_right{1, 0, -1}, bottom_right{1, 0, 1};
    SpotLight plain = s;
    const auto open = render_with(plain);
    s.cookie = kCookie;
    const auto img = render_with(s);
    test::save_output("spot_cookie", W, H, img);
    CHECK((*sr)->stats().cookie_lights == 1);
    CHECK((*sr)->stats().missing_cookies == 0);
    const int open_tl = lum(at(open, top_left)), tl = lum(at(img, top_left));
    const std::uint32_t bl = at(img, bottom_left);
    MESSAGE("open " << open_tl << " white quadrant " << tl << " red quadrant r " << channel(bl, 0) << " g " << channel(bl, 1)
                    << " right " << lum(at(img, top_right)) << "/" << lum(at(img, bottom_right)));
    CHECK(open_tl > 150);
    CHECK(static_cast<float>(tl) > 0.9f * static_cast<float>(open_tl)); // white passes the light unchanged
    CHECK(channel(bl, 0) > 80);
    CHECK(channel(bl, 1) < 8); // red filters out green and blue
    CHECK(lum(at(img, top_right)) < 8);
    CHECK(lum(at(img, bottom_right)) < 8);
    // Turning the light's right vector to +Z turns the image: the white quadrant moves to +X -Z.
    SpotLight turned = s;
    turned.right = {0, 0, 1}; // up = (0,0,1) x (0,-1,0) = (1,0,0): image top towards +X
    const auto rot = render_with(turned);
    MESSAGE("turned: +X-Z " << lum(at(rot, top_right)) << " -X-Z " << lum(at(rot, top_left)));
    CHECK(lum(at(rot, top_right)) > 150);  // image top-left = top (+X), left (-Z)
    CHECK(channel(at(rot, top_left), 0) > 80); // (-X, -Z) is now the image's bottom-left: red
    CHECK(channel(at(rot, top_left), 1) < 8);
    CHECK(lum(at(rot, bottom_right)) < 8);     // (+X, +Z): the image's top-right, black
    // Shadows and cookies share spot.z: a shadowed cookie light keeps both.
    SpotLight shadowed = s;
    shadowed.cast_shadows = true;
    const auto sh = render_with(shadowed);
    const auto blocked = render_with(shadowed, true);
    MESSAGE("shadowed " << lum(at(sh, top_left)) << " blocked " << lum(at(blocked, top_left)) << " right " << lum(at(sh, top_right)));
    CHECK(lum(at(sh, top_left)) > 150);
    CHECK(lum(at(sh, top_right)) < 8);
    CHECK(lum(at(blocked, top_left)) < 0.3f * static_cast<float>(lum(at(sh, top_left))));
    // An unknown key draws the light without a cookie and is counted.
    SpotLight missing = plain;
    missing.cookie = 999;
    const auto miss = render_with(missing);
    CHECK((*sr)->stats().missing_cookies == 1);
    CHECK(lum(at(miss, top_right)) > 150);
    // Removing frees the slot; the light falls back to no cookie.
    (*sr)->remove_cookie(kCookie);
    CHECK_FALSE((*sr)->has_cookie(kCookie));
    ctx.destroy(*plane);
    ctx.destroy(*cube);
}

TEST_CASE("gpu: scenes author spot cookies through LightComponent texture assets (ADR-0195)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    const fs::Path root = std::filesystem::temp_directory_path() / ("victus_cookie_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(root / "assets");
    auto png = encode_png(quadrant_cookie());
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(root / "assets" / "cookie.png", *png).ok());
    auto db = assets::AssetDatabase::open(root);
    REQUIRE(db.ok());
    REQUIRE((*db)->import_all().failed == 0);
    const auto* rec = (*db)->find_path("cookie.png");
    REQUIRE(rec);
    const Uuid cookie = rec->uuid;
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
    l.inner_angle_degrees = 34.0f;
    l.outer_angle_degrees = 40.0f;
    l.cookie = cookie;
    const auto lost = authored.create("Lost");
    authored.world().get<scene::Transform>(lost)->position = {30, 4, 0};
    authored.world().get<scene::Transform>(lost)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    auto& m = authored.world().add<scene::LightComponent>(lost);
    m.type = scene::LightComponent::Type::Spot;
    m.cookie = Uuid::generate(); // no such asset
    const auto cam = authored.create("Camera");
    authored.world().get<scene::Transform>(cam)->position = {0, 10, 6};
    authored.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, std::atan2(-10.0f, 6.0f));
    authored.world().add<scene::CameraComponent>(cam);
    REQUIRE(authored.save_file(root / "assets" / "cookie.vscene").ok());
    scene::Scene s;
    REQUIRE(s.load_file(root / "assets" / "cookie.vscene").ok());
    const auto* loaded = s.world().get<scene::LightComponent>(s.find_by_name("Lamp"));
    REQUIRE(loaded);
    CHECK(loaded->cookie == cookie);
    s.update_transforms();
    runtime::GpuAssetCache cache(ctx, db->get());
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    runtime::SceneViewSettings view;
    view.environment = dark_env();
    const auto stats = runtime::extract_scene(s, cache, **sr, view);
    CHECK(stats.light_cookies == 1);
    CHECK(stats.missing_assets >= 1);
    const auto img = shoot(**sr);
    CHECK((*sr)->stats().cookie_lights == 1);
    const Camera3D view_cam = Camera3D::look_at({0, 10, 6}, {0, 0, 0}, {0, 1, 0});
    const auto at = [&](math::Vec3 p) {
        const auto [x, y] = project(view_cam, p);
        return pixel(img, W, x, y);
    };
    const int tl = lum(at({-1, 0, -1})), tr = lum(at({1, 0, -1}));
    const std::uint32_t bl = at({-1, 0, 1});
    MESSAGE("authored cookie: white " << tl << " red r " << channel(bl, 0) << " g " << channel(bl, 1) << " right " << tr);
    CHECK(tl > 120);
    CHECK(channel(bl, 0) > 60);
    CHECK(channel(bl, 1) < 10);
    CHECK(tr < 10);
    // A second extraction reuses the registered cookie.
    (void)runtime::extract_scene(s, cache, **sr, view);
    CHECK((*sr)->cookie_count() == 1);
    std::filesystem::remove_all(root);
}
