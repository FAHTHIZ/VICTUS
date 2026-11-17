// SPDX-License-Identifier: Apache-2.0
// Image-based lighting: an HDR environment baked on the GPU lights geometry and fills the sky.
#include "gpu_fixture.hpp"
#include "victus/render/environment_map.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 128, H = 96;

ImageF split_env() { // upper hemisphere red, lower hemisphere blue
    ImageF img;
    img.width = 64;
    img.height = 32;
    img.pixels.resize(64 * 32 * 4);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            float* p = &img.pixels[(y * 64 + x) * 4];
            p[0] = y < 16 ? 2.0f : 0.0f;
            p[1] = 0.0f;
            p[2] = y < 16 ? 0.0f : 2.0f;
            p[3] = 1.0f;
        }
    return img;
}

std::vector<std::uint8_t> draw(const Environment& env, float roughness) {
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto target = test::make_target(W, H);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto mesh = ctx.create_mesh(make_uv_sphere(1.0f, 48, 24), "sphere");
    REQUIRE(mesh.ok());
    Camera3D cam = Camera3D::look_at({0, 0, 3.2f}, {0, 0, 0});
    DirectionalLight sun;
    sun.intensity = 0.0f; // environment lighting only
    Material m;
    m.base_color = {1, 1, 1, 1};
    m.roughness = roughness;
    m.metallic = 0.0f;
    (*sr)->begin(cam, sun, env);
    (*sr)->submit(*mesh, m, math::Mat4::identity());
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    ctx.destroy(*mesh);
    dev.destroy(target);
    return std::move(*px);
}
} // namespace

TEST_CASE("gpu: baked environment map lights the sphere and draws the sky") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto map = EnvironmentMap::create(*gpu().ctx, split_env(), EnvironmentBakeSettings::low());
    REQUIRE(map.ok());
    CHECK((*map)->size() == 128);
    CHECK((*map)->mip_count() >= 4);
    CHECK((*map)->sh().diffuse({0, 1, 0}).x > (*map)->sh().diffuse({0, -1, 0}).x);

    Environment env;
    env.vignette = 0.0f;
    env.environment_map = map->get();
    const auto px = draw(env, 0.9f);
    test::save_output("environment_ibl", W, H, px);
    const auto sky_top = pixel(px, W, 4, 2), sky_bottom = pixel(px, W, 4, H - 3);
    const auto top = pixel(px, W, W / 2, H / 2 - 22), bottom = pixel(px, W, W / 2, H / 2 + 22);
    MESSAGE("sky top " << int(channel(sky_top, 0)) << "," << int(channel(sky_top, 2)) << " bottom "
                       << int(channel(sky_bottom, 0)) << "," << int(channel(sky_bottom, 2)) << " sphere top "
                       << int(channel(top, 0)) << "," << int(channel(top, 2)) << " bottom " << int(channel(bottom, 0))
                       << "," << int(channel(bottom, 2)));
    CHECK(channel(sky_top, 0) > channel(sky_top, 2) + 60);
    CHECK(channel(sky_bottom, 2) > channel(sky_bottom, 0) + 60);
    CHECK(channel(top, 0) > channel(top, 2) + 30);       // lit by the red sky
    CHECK(channel(bottom, 2) > channel(bottom, 0) + 30); // lit by the blue ground

    // Intensity scales both lighting and background; zero turns them off.
    Environment dark = env;
    dark.environment_intensity = 0.0f;
    const auto off = draw(dark, 0.9f);
    const auto off_top = pixel(off, W, W / 2, H / 2 - 22), off_sky = pixel(off, W, 4, 2);
    CHECK(channel(off_top, 0) + 40 < channel(top, 0));
    CHECK(channel(off_sky, 0) + 40 < channel(sky_top, 0));

    // A smooth sphere mirrors the environment: the centre reflects what is behind the camera
    // (horizon), the top rim reflects the sky.
    const auto mirror = draw(env, 0.05f);
    const auto rim = pixel(mirror, W, W / 2, H / 2 - 24);
    CHECK(channel(rim, 0) > channel(rim, 2));
}

#include "victus/assets/database.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <filesystem>

TEST_CASE("gpu: Environment component resolves an .hdr asset into image-based lighting") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    namespace sfs = std::filesystem;
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    const fs::Path project = sfs::temp_directory_path() / ("victus_ibl_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets");
    auto hdr = encode_hdr(split_env());
    REQUIRE(hdr.ok());
    REQUIRE(fs::write_atomic(project / "assets" / "sky.hdr", *hdr).ok());
    auto png = encode_png(make_checker(8, 2, 0xff0000ffu, 0xff00ff00u));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(project / "assets" / "ldr.png", *png).ok());
    {
        auto db = assets::AssetDatabase::open(project);
        REQUIRE(db.ok());
        (void)(*db)->import_all();
        const Uuid sky = (*db)->find_path("sky.hdr")->uuid;
        const Uuid ldr = (*db)->find_path("ldr.png")->uuid;
        runtime::GpuAssetCache cache(ctx, db->get());
        cache.set_environment_bake(EnvironmentBakeSettings::low());

        auto render_scene = [&](const Uuid& map, runtime::ExtractStats* stats_out) {
            auto sr = SceneRenderer::create(ctx);
            REQUIRE(sr.ok());
            scene::Scene s;
            const scene::Entity cam = s.create("Camera");
            s.world().get<scene::Transform>(cam)->position = {0, 0, 3.2f};
            s.world().add<scene::CameraComponent>(cam);
            const scene::Entity e = s.create("Environment");
            auto& env = s.world().add<gameplay::EnvironmentComponent>(e);
            env.environment_map = map;
            env.vignette = 0.0f;
            s.update_transforms();
            const auto stats = runtime::extract_scene(s, cache, **sr);
            if (stats_out) *stats_out = stats;
            auto target = test::make_target(W, H);
            auto cmd = ctx.device().begin_commands();
            REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
            cmd.submit_and_wait();
            auto px = ctx.device().read_texture(target).value();
            ctx.device().destroy(target);
            return px;
        };
        runtime::ExtractStats stats;
        const auto px = render_scene(sky, &stats);
        CHECK(stats.missing_assets == 0);
        auto map = cache.environment(sky);
        REQUIRE(map.ok());
        CHECK((*map)->size() == 128);
        const auto top = pixel(px, W, 4, 2), bottom = pixel(px, W, 4, H - 3);
        CHECK(channel(top, 0) > channel(top, 2) + 60);
        CHECK(channel(bottom, 2) > channel(bottom, 0) + 60);

        // An LDR texture is not an environment map: counted as missing, analytic sky used.
        runtime::ExtractStats bad;
        (void)render_scene(ldr, &bad);
        CHECK(bad.missing_assets == 1);
        CHECK_FALSE(cache.environment(ldr).ok());
        cache.evict(sky);
        CHECK(cache.environment(sky).ok()); // rebuilt after eviction
    }
    std::error_code ec;
    sfs::remove_all(project, ec);
}

TEST_CASE("gpu: physical sky follows the sun and re-bakes only when it moves") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity cam = s.create("Camera");
    s.world().get<scene::Transform>(cam)->position = {0, 1, 0};
    s.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(20));
    s.world().add<scene::CameraComponent>(cam);
    const scene::Entity sun = s.create("Sun");
    s.world().add<scene::LightComponent>(sun).shadows = false;
    const scene::Entity e = s.create("Environment");
    auto& env = s.world().add<gameplay::EnvironmentComponent>(e);
    env.physical_sky = true;
    env.vignette = 0.0f;
    auto shoot = [&](float elevation_deg) {
        // Light travels along local -Z; pitch it down by the elevation, facing the camera's view.
        s.world().get<scene::Transform>(sun)->rotation =
            math::Quat::axis_angle({0, 1, 0}, math::radians(180)) * math::Quat::axis_angle({1, 0, 0}, math::radians(-elevation_deg));
        s.update_transforms();
        auto sr = SceneRenderer::create(ctx);
        REQUIRE(sr.ok());
        (void)runtime::extract_scene(s, cache, **sr);
        auto target = test::make_target(W, H);
        auto cmd = ctx.device().begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = ctx.device().read_texture(target).value();
        ctx.device().destroy(target);
        return px;
    };
    const auto noon = shoot(70.0f);
    CHECK(cache.atmosphere_bakes() == 1);
    (void)shoot(70.2f); // below the re-bake threshold
    CHECK(cache.atmosphere_bakes() == 1);
    const auto dusk = shoot(3.0f);
    CHECK(cache.atmosphere_bakes() == 2);
    test::save_output("physical_sky_noon", W, H, noon);
    test::save_output("physical_sky_dusk", W, H, dusk);
    const auto n_top = pixel(noon, W, W / 2, 4), d_low = pixel(dusk, W, W / 2, H - 4);
    MESSAGE("noon top " << int(channel(n_top, 0)) << "," << int(channel(n_top, 1)) << "," << int(channel(n_top, 2))
                        << " dusk low " << int(channel(d_low, 0)) << "," << int(channel(d_low, 1)) << ","
                        << int(channel(d_low, 2)));
    CHECK(channel(n_top, 2) > channel(n_top, 0) + 40); // blue sky overhead
    CHECK(channel(d_low, 0) > channel(d_low, 2));       // warm sky towards the low sun
    const auto night = shoot(-30.0f);
    const auto k = pixel(night, W, W / 2, 4);
    CHECK(int(channel(k, 0)) + channel(k, 1) + channel(k, 2) < 30);
}
