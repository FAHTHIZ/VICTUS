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
