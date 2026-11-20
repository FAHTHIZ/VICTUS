// SPDX-License-Identifier: Apache-2.0
// Scene baked lighting (ADR-0125): a scene's Lightmapped mesh renderers are baked into a
// .vlightmap file, referenced by the Environment, saved and reloaded with the scene, and drawn
// with the bake by extract_scene. Stale, missing and unsafe lightmaps fall back to plain ambient.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/runtime/scene_lightmap.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {

render::RenderSettings plain_settings() {
    render::RenderSettings s = render::RenderSettings::preset(render::QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.ssao = false;
    s.bloom = false;
    return s;
}

// A 6 m lightmapped floor with a dark 1 m cube on it under a uniform grey sky, no sun.
void author(Scene& s) {
    const Entity floor = s.create("Floor");
    s.world().get<Transform>(floor)->scale = {6, 1, 6};
    auto& fm = s.world().add<MeshRendererComponent>(floor);
    fm.primitive = "plane";
    fm.roughness = 1.0f;
    fm.lightmapped = true;
    const Entity cube = s.create("Cube");
    s.world().get<Transform>(cube)->position = {0, 0.5f, 0};
    auto& cm = s.world().add<MeshRendererComponent>(cube);
    cm.primitive = "cube";
    cm.base_color = {0.05f, 0.05f, 0.05f, 1};
    const Entity env_e = s.create("Environment");
    auto& env = s.world().add<gameplay::EnvironmentComponent>(env_e);
    env.sky_zenith = env.sky_horizon = env.ground = {0.5f, 0.5f, 0.5f, 1};
    env.ambient_intensity = 1.0f;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    env.vignette = 0.0f;
    env.tonemapper = gameplay::EnvironmentComponent::Tonemapper::None;
    env.auto_exposure = false;
}

gameplay::EnvironmentComponent& environment(Scene& s) {
    return *s.world().get<gameplay::EnvironmentComponent>(s.find_by_name("Environment"));
}

} // namespace

TEST_CASE("scene lightmap: default path next to the scene") {
    CHECK(runtime::default_lightmap_path("scenes/main.vscene") == "scenes/main.vlightmap");
    CHECK(runtime::default_lightmap_path("main") == "main.vlightmap");
    CHECK(runtime::default_lightmap_path("a.b/level") == "a.b/level.vlightmap");
}

TEST_CASE("scene lightmap: albedo texture mean for bounce colour (ADR-0145)") {
    assets::CookedTexture t;
    const math::Vec3 white = runtime::texture_mean_color(t); // no mips: white (factor only)
    CHECK(white.x == doctest::Approx(1.0f));
    CHECK(white.z == doctest::Approx(1.0f));
    render::Image img;
    img.width = 2;
    img.height = 1;
    img.pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    t.srgb = false;
    t.mips = {img};
    const math::Vec3 m = runtime::texture_mean_color(t);
    CHECK(m.x == doctest::Approx(0.5f));
    CHECK(m.y == doctest::Approx(0.0f));
    CHECK(m.z == doctest::Approx(0.5f));
    // The smallest mip is used and sRGB bytes are decoded: 188 is about 0.5 linear.
    render::Image big;
    big.width = big.height = 2;
    big.pixels.assign(16, 255);
    render::Image small;
    small.width = small.height = 1;
    small.pixels = {188, 188, 188, 255};
    t.srgb = true;
    t.mips = {big, small};
    CHECK(runtime::texture_mean_color(t).y == doctest::Approx(0.5f).epsilon(0.02));
    // A damaged mip (fewer bytes than its size) counts as white instead of reading past the end.
    small.pixels.resize(2);
    t.mips = {small};
    CHECK(runtime::texture_mean_color(t).x == doctest::Approx(1.0f));
}

TEST_CASE("gpu: scene lightmap baked, saved with the scene and drawn by extract_scene") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components(); // the Environment component must survive the save/load round trip
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_lm_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "assets");
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());

    // Bake: one receiver (the floor), one blocker (the cube), no lights.
    Scene authored;
    author(authored);
    authored.update_transforms();
    render::LightmapSettings ls;
    ls.texels_per_unit = 8.0f;
    ls.samples = 96;
    auto bake = runtime::bake_scene_lightmap(authored, db->get(), ls);
    REQUIRE_MESSAGE(bake.ok(), (bake.ok() ? "" : bake.error().message));
    CHECK(bake->receivers == 1);
    CHECK(bake->blockers == 1);
    CHECK(bake->lights == 0);
    REQUIRE(bake->file.entries.size() == 1);
    const Entity floor = authored.find_by_name("Floor");
    CHECK(bake->file.entries[0].entity == authored.uuid(floor));
    auto floor_mesh = runtime::static_mesh_data(authored, floor, db->get());
    REQUIRE(floor_mesh.ok());
    CHECK(bake->file.entries[0].mesh_fingerprint ==
          runtime::scene_lightmap_fingerprint(*floor_mesh, authored.world().get<WorldTransform>(floor)->matrix));
    std::filesystem::create_directories(project / "lighting");
    REQUIRE(fs::write_atomic(project / "lighting" / "main.vlightmap", render::encode_lightmap_file(bake->file)).ok());
    environment(authored).lightmap = "lighting/main.vlightmap";
    REQUIRE(authored.save_file(project / "assets" / "main.vscene").ok());

    // Nothing marked lightmapped: nothing to bake.
    {
        Scene none;
        author(none);
        none.world().get<MeshRendererComponent>(none.find_by_name("Floor"))->lightmapped = false;
        none.update_transforms();
        CHECK_FALSE(runtime::bake_scene_lightmap(none, db->get(), ls).ok());
    }

    Scene scene;
    REQUIRE(scene.load_file(project / "assets" / "main.vscene").ok());
    CHECK(scene.world().get<MeshRendererComponent>(scene.find_by_name("Floor"))->lightmapped);
    CHECK_FALSE(scene.world().get<MeshRendererComponent>(scene.find_by_name("Cube"))->lightmapped);
    CHECK(environment(scene).lightmap == "lighting/main.vlightmap");
    scene.update_transforms();

    auto sr = render::SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    constexpr std::uint32_t W = 96, H = 96;
    const render::Camera3D camera = render::Camera3D::look_at({0, 7.0f, 0.3f}, {0, 0, 0});
    runtime::GpuAssetCache cache(ctx, db->get());
    auto shoot = [&](Scene& s, runtime::ExtractStats* stats_out) {
        const auto stats = runtime::extract_scene(s, cache, **sr, {}, &camera);
        if (stats_out) *stats_out = stats;
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    runtime::ExtractStats stats;
    const auto lit = shoot(scene, &stats);
    CHECK(stats.lightmapped == 1);
    CHECK(cache.lightmap_count() == 1);
    test::save_output("scene_lightmap_baked", W, H, lit);
    // Same scene without the lightmap reference: evenly lit floor.
    environment(scene).lightmap.clear();
    const auto plain = shoot(scene, &stats);
    CHECK(stats.lightmapped == 0);
    test::save_output("scene_lightmap_plain", W, H, plain);

    const std::uint32_t near_x = W / 2 + 9, far_x = W / 2 + 30, y = H / 2;
    const auto p_near = pixel(plain, W, near_x, y), p_far = pixel(plain, W, far_x, y);
    const auto l_near = pixel(lit, W, near_x, y), l_far = pixel(lit, W, far_x, y);
    MESSAGE("plain near/far " << int(channel(p_near, 1)) << "/" << int(channel(p_far, 1)) << ", baked near/far "
                              << int(channel(l_near, 1)) << "/" << int(channel(l_far, 1)));
    CHECK(std::abs(int(channel(p_near, 1)) - int(channel(p_far, 1))) <= 2);
    CHECK(int(channel(l_near, 1)) < int(channel(l_far, 1)) - 12); // contact darkening from the bake
    CHECK(std::abs(int(channel(l_far, 1)) - int(channel(p_far, 1))) <= 12);

    // Geometry changed since the bake (the floor became a cube): the stale entry is not used.
    {
        Scene edited;
        REQUIRE(edited.load_file(project / "assets" / "main.vscene").ok());
        edited.world().get<MeshRendererComponent>(edited.find_by_name("Floor"))->primitive = "cube";
        edited.update_transforms();
        cache.evict_lightmaps();
        (void)shoot(edited, &stats);
        CHECK(stats.lightmapped == 0);
        CHECK(stats.meshes == 2);
    }
    // Moved since the bake: also stale.
    {
        Scene moved;
        REQUIRE(moved.load_file(project / "assets" / "main.vscene").ok());
        moved.world().get<Transform>(moved.find_by_name("Floor"))->position = {0.5f, 0, 0};
        moved.update_transforms();
        cache.evict_lightmaps();
        (void)shoot(moved, &stats);
        CHECK(stats.lightmapped == 0);
    }
    // Missing and unsafe lightmap paths draw without baked lighting.
    for (const char* bad : {"lighting/missing.vlightmap", "../outside.vlightmap"}) {
        environment(scene).lightmap = bad;
        (void)shoot(scene, &stats);
        CHECK(stats.lightmapped == 0);
        CHECK(stats.meshes == 2);
    }
    // A damaged file is rejected, not drawn.
    REQUIRE(fs::write_atomic(project / "lighting" / "bad.vlightmap", std::vector<std::uint8_t>{'V', 'I', 'C', 'T', 'U', 'S', 0, 1}).ok());
    environment(scene).lightmap = "lighting/bad.vlightmap";
    (void)shoot(scene, &stats);
    CHECK(stats.lightmapped == 0);

    cache.clear();
    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}

TEST_CASE("gpu: baked light probes shade dynamic objects under cover (ADR-0133)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_lp_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "lighting");
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());

    // A lightmapped floor, a roof slab (blocker) over the left sphere, and two identical white
    // spheres that are not lightmapped (dynamic objects).
    Scene scene;
    author(scene);
    scene.destroy(scene.find_by_name("Cube"));
    const Entity roof = scene.create("Roof");
    scene.world().get<Transform>(roof)->position = {-2.5f, 2.0f, 0};
    scene.world().get<Transform>(roof)->scale = {5, 0.2f, 5};
    scene.world().add<MeshRendererComponent>(roof).primitive = "cube";
    for (const auto& [name, x] : {std::pair{"Covered", -2.5f}, std::pair{"Open", 2.5f}}) {
        const Entity e = scene.create(name);
        scene.world().get<Transform>(e)->position = {x, 1.0f, 0};
        auto& m = scene.world().add<MeshRendererComponent>(e);
        m.primitive = "sphere";
        m.roughness = 1.0f;
        m.base_color = {0.8f, 0.8f, 0.8f, 1};
    }
    scene.world().get<Transform>(scene.find_by_name("Floor"))->scale = {12, 1, 12};
    scene.world().get<MeshRendererComponent>(scene.find_by_name("Floor"))->base_color = {0.3f, 0.3f, 0.3f, 1}; // weak bounce
    scene.update_transforms();
    render::LightmapSettings ls;
    ls.texels_per_unit = 2.0f;
    ls.samples = 32;
    ls.probe_samples = 256;
    auto bake = runtime::bake_scene_lightmap(scene, db->get(), ls);
    REQUIRE_MESSAGE(bake.ok(), (bake.ok() ? "" : bake.error().message));
    REQUIRE_FALSE(bake->file.probes.empty());
    CHECK(bake->stats.probes == bake->file.probes.probe_count());
    // The spheres overlap a probe each; dilation fills them from the neighbours.
    CHECK(bake->stats.dilated_probes > 0);
    {
        const auto& pr = bake->file.probes;
        const float covered = pr.sample({-2.5f, 1, 0}, {0, 1, 0}, math::Vec3{-1}).x, open = pr.sample({2.5f, 1, 0}, {0, 1, 0}, math::Vec3{-1}).x;
        MESSAGE("probes " << pr.counts[0] << "x" << pr.counts[1] << "x" << pr.counts[2] << ", " << bake->stats.invalid_probes << " invalid, "
                          << bake->stats.dilated_probes << " dilated; up irradiance covered " << covered << ", open " << open);
        CHECK(covered > 0.0f);
        CHECK(covered < 0.75f * open);
    }
    REQUIRE(fs::write_atomic(project / "lighting" / "probes.vlightmap", render::encode_lightmap_file(bake->file)).ok());

    auto sr = render::SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(plain_settings());
    constexpr std::uint32_t W = 128, H = 64;
    const render::Camera3D camera = render::Camera3D::look_at({0, 1.0f, 9.0f}, {0, 1.0f, 0});
    runtime::GpuAssetCache cache(ctx, db->get());
    auto shoot = [&](runtime::ExtractStats& stats) {
        stats = runtime::extract_scene(scene, cache, **sr, {}, &camera);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    // Upward-facing part of each sphere: find the sphere's centre column on the centre row (the
    // brightest pixel of that half), then average the three rows above it, whose normals tilt
    // 30-70 degrees towards the roof / open sky.
    auto sphere_level = [&](const std::vector<std::uint8_t>& px, bool left) {
        int best = -1;
        std::uint32_t col = 0;
        const std::uint32_t y = H / 2;
        for (std::uint32_t x = left ? 4 : W / 2 + 4; x < (left ? W / 2 - 4 : W - 4); ++x) {
            const int v = int(channel(pixel(px, W, x, y), 1));
            if (v > best) best = v, col = x;
        }
        int sum = 0;
        for (std::uint32_t r = 1; r <= 3; ++r) sum += int(channel(pixel(px, W, col, y - r), 1));
        return sum / 3;
    };

    runtime::ExtractStats stats;
    environment(scene).lightmap.clear();
    const auto sky_only = shoot(stats);
    CHECK_FALSE(stats.lightmap_probes);
    environment(scene).lightmap = "lighting/probes.vlightmap";
    const auto probed = shoot(stats);
    CHECK(stats.lightmap_probes);
    CHECK((*sr)->stats().irradiance_probes > 0);
    test::save_output("lightmap_probes_sky_only", W, H, sky_only);
    test::save_output("lightmap_probes_baked", W, H, probed);
    const int sky_covered = sphere_level(sky_only, true), sky_open = sphere_level(sky_only, false);
    const int lp_covered = sphere_level(probed, true), lp_open = sphere_level(probed, false);
    MESSAGE("sky-only covered/open " << sky_covered << "/" << sky_open << ", probes covered/open " << lp_covered << "/" << lp_open);
    // Without probes the roof does not matter to the dynamic spheres; with them the sheltered one
    // loses a clear part of its sky light while the open one keeps it.
    CHECK(std::abs(sky_covered - sky_open) <= 3);
    CHECK(lp_covered < lp_open - 10);
    CHECK(std::abs(lp_open - sky_open) <= 20);

    // A hand-placed irradiance volume around the camera takes precedence over the baked probes.
    const Entity vol = scene.create("Volume");
    scene.world().get<Transform>(vol)->position = {0, 1.0f, 9.0f};
    auto& iv = scene.world().add<gameplay::IrradianceVolumeComponent>(vol);
    iv.size = {2, 2, 2};
    scene.update_transforms();
    for (int frame = 0; frame < 16; ++frame) { // the volume bakes over a few frames
        (void)shoot(stats);
        if (!stats.lightmap_probes) break;
    }
    CHECK_FALSE(stats.lightmap_probes);

    cache.clear();
    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}
