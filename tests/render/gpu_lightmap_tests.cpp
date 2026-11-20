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

TEST_CASE("gpu: the baked shadowmask shades static sun shadows beyond the shadow distance (ADR-0131)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());

    const MeshData floor = make_plane({6, 6}, 4);
    const MeshData roof = make_cube({2, 0.1f, 2});
    const math::Mat4 roof_model = math::Mat4::translation({0, 1.5f, 0});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occluder{&roof, roof_model};
    occluder.receives = false;
    scene.instances.push_back(occluder);
    scene.sky.sky = scene.sky.horizon = scene.sky.ground = math::Vec3{0.5f};
    LightmapLight baked_sun;
    baked_sun.direction = math::normalize(math::Vec3{0.2f, -1.0f, 0.1f});
    scene.lights.push_back(baked_sun);
    LightmapSettings ls;
    ls.samples = 32;
    auto bake = bake_lightmaps(scene, ls);
    REQUIRE(bake.ok());
    REQUIRE(math::length(bake->shadowmask_direction) > 0.5f);

    auto lm_floor = ctx.create_lightmapped_mesh(floor, bake->instances[0], "mask_floor");
    auto gpu_roof = ctx.create_mesh(roof, "mask_roof");
    auto atlas = ctx.create_lightmap_texture(bake->atlas);
    REQUIRE(lm_floor.ok());
    REQUIRE(gpu_roof.ok());
    REQUIRE(atlas.ok());

    constexpr std::uint32_t W = 96, H = 96;
    Material white;
    white.base_color = {1, 1, 1, 1};
    white.roughness = 1.0f;
    // Camera looks at the floor under the roof from the side, so the shadowed patch is visible.
    auto shoot = [&](math::Vec3 sun_dir, math::Vec3 mask_dir, float shadow_distance) {
        DirectionalLight sun;
        sun.direction = sun_dir;
        sun.intensity = 3.0f;
        sun.shadow_distance = shadow_distance;
        Environment env = grey_sky();
        env.lightmap = *atlas;
        env.lightmap_shadowmask_direction = mask_dir;
        (*sr)->begin(Camera3D::look_at({0, 1.0f, 5.0f}, {0, 0, 0}), sun, env);
        (*sr)->submit(*lm_floor, white, math::Mat4::identity());
        (*sr)->submit(*gpu_roof, white, roof_model);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const math::Vec3 sun_dir = baked_sun.direction;
    const auto realtime = shoot(sun_dir, {0, 0, 0}, 40.0f); // cascades cover the scene
    const auto no_mask = shoot(sun_dir, {0, 0, 0}, 0.5f);   // cascades end before the floor
    const auto masked = shoot(sun_dir, sun_dir, 0.5f);
    const math::Vec3 moved = math::normalize(math::Vec3{-0.6f, -1.0f, 0.0f});
    const auto stale = shoot(moved, sun_dir, 0.5f);         // the sun moved: the mask is ignored
    const auto stale_ref = shoot(moved, {0, 0, 0}, 0.5f);
    test::save_output("shadowmask_realtime", W, H, realtime);
    test::save_output("shadowmask_masked", W, H, masked);

    // The floor pixel at the screen centre is the world origin, under the middle of the roof.
    const std::uint32_t x = W / 2, y = H / 2;
    const int rt = int(channel(pixel(realtime, W, x, y), 1)), nm = int(channel(pixel(no_mask, W, x, y), 1));
    const int mk = int(channel(pixel(masked, W, x, y), 1));
    MESSAGE("under the roof: realtime " << rt << ", beyond shadow distance without/with mask " << nm << "/" << mk);
    CHECK(nm > rt + 40); // without a mask the shadow disappears past the cascades
    CHECK(std::abs(mk - rt) <= 12); // the mask brings it back
    CHECK(stale == stale_ref);

    dev.destroy(*atlas);
    ctx.destroy(*lm_floor);
    ctx.destroy(*gpu_roof);
}

TEST_CASE("gpu: congruent lightmapped copies share one mesh and batch (ADR-0135)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());

    const MeshData floor = make_plane({8, 8}, 4);
    const MeshData cube = make_cube();
    const math::Quat quarter = math::Quat::axis_angle({0, 1, 0}, math::radians(90.0f));
    const math::Mat4 models[3] = {math::Mat4::translation({-2, 0.5f, 0}), math::Mat4::trs({0, 0.5f, 0}, quarter, {1, 1, 1}),
                                  math::Mat4::translation({2, 0.5f, 0})};
    LightmapScene scene;
    scene.instances.push_back({&floor});
    for (const auto& m : models) scene.instances.push_back({&cube, m});
    scene.sky.sky = scene.sky.horizon = scene.sky.ground = math::Vec3{0.2f};
    LightmapLight key; // fully baked low sun from +x: lit, shadowed and dark faces differ clearly
    key.direction = math::normalize(math::Vec3{-1.0f, -0.6f, -0.3f});
    key.radiance = {3, 3, 3};
    scene.lights.push_back(key);
    LightmapSettings ls;
    ls.texels_per_unit = 8.0f;
    ls.samples = 64;
    ls.include_direct = true;
    ls.shadowmask = false;
    ls.probes = false;
    auto bake = bake_lightmaps(scene, ls);
    REQUIRE(bake.ok());
    std::optional<math::Vec2> offsets[3] = {math::Vec2{0, 0}, lightmap_uv_offset(bake->instances[1], bake->instances[2]),
                                            lightmap_uv_offset(bake->instances[1], bake->instances[3])};
    REQUIRE(offsets[1]);
    REQUIRE(offsets[2]);

    auto lm_floor = ctx.create_lightmapped_mesh(floor, bake->instances[0], "share_floor");
    REQUIRE(lm_floor.ok());
    std::vector<GpuMesh> own;
    for (int i = 0; i < 3; ++i) {
        auto g = ctx.create_lightmapped_mesh(cube, bake->instances[static_cast<std::size_t>(i) + 1], "share_cube");
        REQUIRE(g.ok());
        own.push_back(*g);
    }
    auto atlas = ctx.create_lightmap_texture(bake->atlas);
    REQUIRE(atlas.ok());

    constexpr std::uint32_t W = 128, H = 96;
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    Material white;
    white.base_color = {1, 1, 1, 1};
    white.roughness = 1.0f;
    std::uint32_t batches = 0;
    auto shoot = [&](bool shared, bool offsets_on = true) {
        Environment env = grey_sky();
        env.lightmap = *atlas;
        (*sr)->begin(Camera3D::look_at({1.5f, 3.0f, 5.0f}, {0, 0.5f, 0}), sun, env);
        (*sr)->submit(*lm_floor, white, math::Mat4::identity());
        for (int i = 0; i < 3; ++i) {
            if (shared) (*sr)->submit(own[0], white, models[i], 0, offsets_on ? *offsets[i] : math::Vec2{});
            else (*sr)->submit(own[static_cast<std::size_t>(i)], white, models[i]);
        }
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        batches = (*sr)->stats().main_batches;
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const auto separate = shoot(false);
    const std::uint32_t separate_batches = batches;
    const auto shared = shoot(true);
    const std::uint32_t shared_batches = batches;
    test::save_output("lightmap_shared_separate", W, H, separate);
    test::save_output("lightmap_shared", W, H, shared);
    MESSAGE("main batches: separate " << separate_batches << ", shared " << shared_batches);
    CHECK(shared_batches + 2 == separate_batches); // three cubes in one instanced draw
    int worst = 0;
    std::size_t off = 0;
    for (std::size_t i = 0; i < separate.size(); ++i) {
        const int d = std::abs(int(separate[i]) - int(shared[i]));
        worst = std::max(worst, d);
        if (d > 1) ++off;
    }
    MESSAGE("worst channel difference " << worst << ", channels off by more than 1: " << off);
    CHECK(worst <= 2);
    // The offsets matter: without them the second and third cubes read the first one's texels
    // (lit as if they stood at x = -2), which visibly changes the picture.
    const auto unshifted = shoot(true, false);
    std::size_t changed = 0;
    for (std::size_t i = 0; i < separate.size(); ++i)
        if (std::abs(int(separate[i]) - int(unshifted[i])) > 8) ++changed;
    MESSAGE("channels changed without offsets: " << changed);
    CHECK(changed > 100);

    dev.destroy(*atlas);
    ctx.destroy(*lm_floor);
    for (auto& g : own) ctx.destroy(g);
}

TEST_CASE("gpu: probe sun occlusion shadows dynamic objects beyond the shadow distance (ADR-0136)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());

    // A lightmapped floor and a 4 x 4 m roof slab 2.1..2.3 m up; the sun shines almost straight down.
    const MeshData floor = make_plane({12, 12}, 4);
    const MeshData roof = make_cube({2, 0.1f, 2});
    const math::Mat4 roof_model = math::Mat4::translation({0, 2.2f, 0});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occluder{&roof, roof_model};
    occluder.receives = false;
    scene.instances.push_back(occluder);
    scene.sky.sky = scene.sky.horizon = scene.sky.ground = math::Vec3{0.5f};
    LightmapLight baked_sun;
    baked_sun.direction = math::normalize(math::Vec3{0.1f, -1.0f, 0.05f});
    scene.lights.push_back(baked_sun);
    LightmapSettings ls;
    ls.samples = 16;
    ls.probe_samples = 64;
    auto bake = bake_lightmaps(scene, ls);
    REQUIRE(bake.ok());
    REQUIRE(bake->probes.sun.size() == bake->probes.sh.size());
    CHECK(bake->probes.sample_sun({0, 1, 0}) < 0.05f);

    // Two volumes with the same irradiance: one with the baked sun visibility, one without.
    IrradianceGrid grid;
    grid.bounds = bake->probes.bounds;
    grid.counts = bake->probes.counts;
    auto with_sun = IrradianceVolume::create(ctx, grid);
    auto without = IrradianceVolume::create(ctx, grid);
    REQUIRE(with_sun.ok());
    REQUIRE(without.ok());
    REQUIRE((*with_sun)->grid().counts == grid.counts);
    for (std::uint32_t i = 0; i < bake->probes.probe_count(); ++i) {
        if (!bake->probes.valid[i]) continue;
        SphericalHarmonics9 sh;
        for (std::size_t k = 0; k < 9; ++k) sh.coefficients[k] = bake->probes.sh[i][k];
        (*with_sun)->set_probe(i, sh);
        (*without)->set_probe(i, sh);
        (*with_sun)->set_probe_sun(i, bake->probes.sun[i]);
    }
    REQUIRE((*with_sun)->upload().ok());
    REQUIRE((*without)->upload().ok());
    CHECK((*with_sun)->sample_sun({0, 1, 0}) == doctest::Approx(bake->probes.sample_sun({0, 1, 0})).epsilon(1e-4).scale(1.0));
    CHECK((*without)->sample_sun({0, 1, 0}) == 1.0f);

    const MeshData sphere = make_uv_sphere(0.5f);
    auto gpu_sphere = ctx.create_mesh(sphere, "probe_sun_sphere");
    auto gpu_roof = ctx.create_mesh(roof, "probe_sun_roof");
    auto atlas = ctx.create_lightmap_texture(bake->atlas);
    REQUIRE(gpu_sphere.ok());
    REQUIRE(gpu_roof.ok());
    REQUIRE(atlas.ok());
    constexpr std::uint32_t W = 96, H = 96;
    Material white;
    white.base_color = {1, 1, 1, 1};
    white.roughness = 1.0f;
    const math::Mat4 sphere_model = math::Mat4::translation({0, 1.0f, 0});
    auto shoot = [&](const IrradianceVolume* volume, math::Vec3 sun_dir, math::Vec3 mask_dir, float shadow_distance) {
        DirectionalLight sun;
        sun.direction = sun_dir;
        sun.intensity = 3.0f;
        sun.shadow_distance = shadow_distance;
        Environment env = grey_sky();
        env.lightmap = *atlas;
        env.lightmap_shadowmask_direction = mask_dir;
        (*sr)->begin(Camera3D::look_at({0, 1.0f, 5.0f}, {0, 1.0f, 0}), sun, env);
        (*sr)->set_irradiance_volume(volume); // per frame, after begin()
        (*sr)->submit(*gpu_sphere, white, sphere_model); // dynamic: not lightmapped
        (*sr)->submit(*gpu_roof, white, roof_model);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const math::Vec3 sun_dir = baked_sun.direction;
    const auto realtime = shoot(without->get(), sun_dir, sun_dir, 40.0f); // cascades cover the sphere
    const auto no_probe_sun = shoot(without->get(), sun_dir, sun_dir, 0.5f);
    const auto probe_sun = shoot(with_sun->get(), sun_dir, sun_dir, 0.5f);
    CHECK((*sr)->stats().irradiance_probes > 0);
    const math::Vec3 moved = math::normalize(math::Vec3{-0.6f, -1.0f, 0.0f});
    const auto stale = shoot(with_sun->get(), moved, sun_dir, 0.5f); // the sun moved: ignored
    const auto stale_ref = shoot(without->get(), moved, sun_dir, 0.5f);
    test::save_output("probe_sun_realtime", W, H, realtime);
    test::save_output("probe_sun_masked", W, H, probe_sun);

    // The upper part of the sphere (normals towards the sun), a few rows above the centre.
    auto top = [&](const std::vector<std::uint8_t>& px) {
        int sum = 0;
        for (std::uint32_t r = 3; r <= 5; ++r) sum += int(channel(pixel(px, W, W / 2, H / 2 - r), 1));
        return sum / 3;
    };
    const int rt = top(realtime), nm = top(no_probe_sun), mk = top(probe_sun);
    MESSAGE("sphere top under the roof: realtime " << rt << ", beyond shadow distance without/with probe sun " << nm << "/" << mk);
    CHECK(nm > rt + 30);            // without probe sun the roof's shadow disappears past the cascades
    CHECK(mk < nm - 25);            // probe sun occlusion brings it back (probes interpolate, so
    CHECK(std::abs(mk - rt) <= 16); // it is a little softer than the realtime map)
    CHECK(stale == stale_ref);

    dev.destroy(*atlas);
    ctx.destroy(*gpu_sphere);
    ctx.destroy(*gpu_roof);
}
