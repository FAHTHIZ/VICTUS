// SPDX-License-Identifier: Apache-2.0
// Baked lighting on the GPU (ADR-0122): lightmapped meshes sample the atlas through their own uv
// stream in place of the sky ambient, a CPU bake darkens the floor next to an object, and meshes
// without a lightmap (or a scene without one) render exactly as before.
#include "gpu_fixture.hpp"
#include "victus/render/lightmap.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {

Environment grey_sky() {
    Environment env;
    env.sky_zenith = env.sky_horizon = env.ground = {0.5f, 0.5f, 0.5f, 1};
    env.ambient_intensity = 1.0f;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.vignette = 0.0f;
    env.tonemap = false;
    env.auto_exposure = false;
    return env;
}

RenderSettings plain_settings() {
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.ssao = false;
    s.bloom = false;
    return s;
}

} // namespace

TEST_CASE("gpu: lightmapped meshes take ambient light from the baked atlas") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());

    const MeshData floor = make_plane({6, 6}, 4);
    const MeshData cube = make_cube();
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occluder{&cube, math::Mat4::translation({0, 0.5f, 0})};
    occluder.receives = false;
    occluder.albedo = {0.05f, 0.05f, 0.05f}; // dark: little bounce light refills the contact shadow
    scene.instances.push_back(occluder);
    scene.sky.sky = scene.sky.horizon = scene.sky.ground = math::Vec3{0.5f};
    LightmapSettings ls;
    ls.texels_per_unit = 8.0f;
    ls.samples = 128;
    auto bake = bake_lightmaps(scene, ls);
    REQUIRE(bake.ok());

    auto lm_floor = ctx.create_lightmapped_mesh(floor, bake->instances[0], "lm_floor");
    REQUIRE(lm_floor.ok());
    CHECK(lm_floor->lightmapped());
    auto plain_floor = ctx.create_mesh(floor, "plain_floor");
    auto gpu_cube = ctx.create_mesh(cube, "lm_cube");
    REQUIRE(plain_floor.ok());
    REQUIRE(gpu_cube.ok());
    auto baked = ctx.create_lightmap_texture(bake->atlas);
    REQUIRE(baked.ok());
    ImageF green;
    green.width = green.height = 4;
    for (int i = 0; i < 16; ++i) green.pixels.insert(green.pixels.end(), {0.0f, 1.0f, 0.0f, 1.0f});
    auto green_tex = ctx.create_lightmap_texture(green, "green_lightmap");
    REQUIRE(green_tex.ok());
    CHECK_FALSE(ctx.create_lightmap_texture(ImageF{}).ok());

    constexpr std::uint32_t W = 96, H = 96;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    Material white;
    white.base_color = {1, 1, 1, 1};
    white.roughness = 1.0f;
    auto shoot = [&](const GpuMesh& floor_mesh, rhi::TextureHandle lightmap) {
        Environment env = grey_sky();
        env.lightmap = lightmap;
        (*sr)->begin(Camera3D::look_at({0, 7.0f, 0.3f}, {0, 0, 0}), sun, env);
        (*sr)->submit(floor_mesh, white, math::Mat4::identity());
        (*sr)->submit(*gpu_cube, white, math::Mat4::translation({0, 0.5f, 0}));
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const auto plain = shoot(*plain_floor, {});
    const auto no_atlas = shoot(*lm_floor, {});   // lightmapped mesh, scene without a lightmap
    const auto green_px = shoot(*lm_floor, *green_tex);
    const auto lit = shoot(*lm_floor, *baked);
    test::save_output("lightmap_plain", W, H, plain);
    test::save_output("lightmap_baked", W, H, lit);

    // Pixels on the floor: next to the cube (screen right of centre) and near the edge.
    const std::uint32_t near_x = W / 2 + 9, far_x = W / 2 + 30, y = H / 2;
    const auto p_near = pixel(plain, W, near_x, y), p_far = pixel(plain, W, far_x, y);
    const auto l_near = pixel(lit, W, near_x, y), l_far = pixel(lit, W, far_x, y);
    MESSAGE("plain near/far " << int(channel(p_near, 1)) << "/" << int(channel(p_far, 1)) << ", baked near/far "
                              << int(channel(l_near, 1)) << "/" << int(channel(l_far, 1)));
    // Without a scene lightmap a lightmapped mesh renders like the plain one.
    CHECK(no_atlas == plain);
    // The atlas replaces the ambient: a green lightmap turns the white floor green.
    const auto g = pixel(green_px, W, far_x, y);
    CHECK(channel(g, 1) > channel(g, 0) + 60);
    CHECK(channel(g, 1) > channel(g, 2) + 60);
    // The plain floor is evenly lit by the sky; the baked one is darker next to the cube and
    // matches the sky ambient away from it.
    CHECK(std::abs(int(channel(p_near, 1)) - int(channel(p_far, 1))) <= 2);
    CHECK(int(channel(l_near, 1)) < int(channel(l_far, 1)) - 18);
    CHECK(std::abs(int(channel(l_far, 1)) - int(channel(p_far, 1))) <= 12);

    dev.destroy(*baked);
    dev.destroy(*green_tex);
    ctx.destroy(*lm_floor);
    ctx.destroy(*plain_floor);
    ctx.destroy(*gpu_cube);
}

TEST_CASE("gpu: directional lightmaps let normal maps shade baked light (ADR-0130)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());

    const MeshData floor = make_plane({6, 6}, 2);
    LightmapScene scene;
    scene.instances.push_back({&floor});
    auto layout = unwrap_lightmaps(scene, {});
    REQUIRE(layout.ok());
    auto lm_floor = ctx.create_lightmapped_mesh(floor, layout->instances[0], "dir_floor");
    REQUIRE(lm_floor.ok());

    // A uniform grey bake and directional atlases where all light comes from +X or -X.
    ImageF grey;
    grey.width = grey.height = 4;
    for (int i = 0; i < 16; ++i) grey.pixels.insert(grey.pixels.end(), {0.25f, 0.25f, 0.25f, 1.0f});
    auto grey_tex = ctx.create_lightmap_texture(grey, "grey_lightmap");
    REQUIRE(grey_tex.ok());
    auto dir_image = [](std::uint8_t x) {
        Image img;
        img.width = img.height = 4;
        // direction (+-1, 0, 0), rebalancing at the +Y floor normal 0.5 * 0 + 0.5 = 0.5
        for (int i = 0; i < 16; ++i) img.pixels.insert(img.pixels.end(), {x, 128, 128, 128});
        return img;
    };
    auto from_px = ctx.create_lightmap_direction_texture(dir_image(255), "dir_px");
    auto from_nx = ctx.create_lightmap_direction_texture(dir_image(0), "dir_nx");
    REQUIRE(from_px.ok());
    REQUIRE(from_nx.ok());
    CHECK_FALSE(ctx.create_lightmap_direction_texture(Image{}).ok());

    // A normal map that tilts the surface 45 degrees about the tangent frame's Z axis.
    Image tilt;
    tilt.width = tilt.height = 4;
    for (int i = 0; i < 16; ++i) tilt.pixels.insert(tilt.pixels.end(), {218, 128, 218, 255});
    auto tilt_tex = ctx.create_texture(tilt, {false, false, "tilt_normal"});
    REQUIRE(tilt_tex.ok());

    constexpr std::uint32_t W = 64, H = 64;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    auto shoot = [&](rhi::TextureHandle direction, bool normal_mapped) {
        Material m;
        m.base_color = {1, 1, 1, 1};
        m.roughness = 1.0f;
        m.metallic = 0.0f;
        if (normal_mapped) m.normal_texture = *tilt_tex;
        Environment env = grey_sky();
        env.ambient_intensity = 0.0f; // no specular environment either: only the lightmap shades
        env.lightmap = *grey_tex;
        env.lightmap_direction = direction;
        (*sr)->begin(Camera3D::look_at({0, 6.0f, 0.01f}, {0, 0, 0}), sun, env);
        (*sr)->submit(*lm_floor, m, math::Mat4::identity());
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return int(channel(pixel(px, W, W / 2, H / 2), 1));
    };
    const int flat_plain = shoot({}, false);
    const int flat_px = shoot(*from_px, false);
    const int bumped_plain = shoot({}, true);
    const int bumped_px = shoot(*from_px, true);
    const int bumped_nx = shoot(*from_nx, true);
    MESSAGE("flat " << flat_plain << "/" << flat_px << ", bumped plain/+x/-x " << bumped_plain << "/" << bumped_px << "/"
                    << bumped_nx);
    // The baked normal reproduces the stored irradiance exactly (rebalancing).
    CHECK(std::abs(flat_px - flat_plain) <= 2);
    // Without a directional atlas the normal map cannot change baked light.
    CHECK(std::abs(bumped_plain - flat_plain) <= 2);
    // With one, tilting towards the light brightens and tilting away darkens.
    CHECK(std::abs(bumped_px - bumped_nx) > 40);
    CHECK(std::max(bumped_px, bumped_nx) > flat_plain + 20);
    CHECK(std::min(bumped_px, bumped_nx) < flat_plain - 20);

    dev.destroy(*grey_tex);
    dev.destroy(*from_px);
    dev.destroy(*from_nx);
    dev.destroy(*tilt_tex);
    ctx.destroy(*lm_floor);
}
