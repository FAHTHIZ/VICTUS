// SPDX-License-Identifier: Apache-2.0
// Irradiance volumes (ADR-0056): probe grid layout and interpolation on the CPU, baking from scene
// captures (direct light and extra bounces), and lit surfaces taking diffuse light from the volume.
#include "gpu_fixture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/render/irradiance_bake.hpp"
#include "victus/render/irradiance_volume.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
SphericalHarmonics9 constant_sh(math::Vec3 radiance) {
    // A uniform environment of radiance L gives diffuse(n) = L for every n (irradiance / pi).
    SphericalHarmonics9 sh;
    sh.coefficients[0] = radiance * (1.0f / 0.282095f);
    return sh;
}
} // namespace

TEST_CASE("irradiance grid: probe positions, counts and sanitizing") {
    IrradianceGrid g;
    g.bounds = {{-2, 0, -4}, {2, 2, 4}};
    g.counts = {3, 2, 5};
    CHECK(g.probe_count() == 30);
    CHECK(g.spacing().x == doctest::Approx(2.0f));
    CHECK(g.spacing().z == doctest::Approx(2.0f));
    const math::Vec3 first = g.probe_position(0), last = g.probe_position(g.probe_count() - 1);
    CHECK(first.x == doctest::Approx(-2.0f));
    CHECK(first.z == doctest::Approx(-4.0f));
    CHECK(last.x == doctest::Approx(2.0f));
    CHECK(last.y == doctest::Approx(2.0f));
    CHECK(last.z == doctest::Approx(4.0f));
    const math::Vec3 p = g.probe_position(g.index(1, 1, 2));
    CHECK(p.x == doctest::Approx(0.0f));
    CHECK(p.y == doctest::Approx(2.0f));
    CHECK(p.z == doctest::Approx(0.0f));
    IrradianceGrid bad;
    bad.bounds = {{1, 1, 1}, {1, -3, std::nanf("")}};
    bad.counts = {0, 100, 1};
    const IrradianceGrid s = bad.sanitized();
    CHECK(s.counts[0] == 2);
    CHECK(s.counts[1] == kMaxIrradianceProbesY);
    CHECK(s.bounds.valid());
    const auto counts = irradiance_counts_for({20, 4, 7}, 2.0f);
    CHECK(counts[0] == 11);
    CHECK(counts[1] == 3);
    CHECK(counts[2] == 5);
    CHECK(irradiance_counts_for({1000, 1000, 1000}, 1.0f)[1] == kMaxIrradianceProbesY);
}

TEST_CASE("gpu: irradiance volume interpolates valid probes and falls back elsewhere") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    IrradianceGrid g;
    g.bounds = {{0, 0, 0}, {2, 2, 2}};
    g.counts = {2, 2, 2};
    auto v = IrradianceVolume::create(*gpu().ctx, g);
    REQUIRE(v.ok());
    const math::Vec3 fallback{0.1f, 0.2f, 0.3f}, up{0, 1, 0};
    CHECK((*v)->valid_count() == 0);
    CHECK((*v)->sample({1, 1, 1}, up, fallback).x == doctest::Approx(0.1f)); // nothing baked yet
    for (std::uint32_t i = 0; i < 8; ++i) (*v)->set_probe(i, constant_sh({(*v)->grid().probe_position(i).x, 0, 0}));
    CHECK((*v)->valid_count() == 8);
    CHECK((*v)->sample({1, 1, 1}, up, fallback).x == doctest::Approx(1.0f));
    CHECK((*v)->sample({0.5f, 0.3f, 1.7f}, up, fallback).x == doctest::Approx(0.5f));
    CHECK((*v)->sample({3, 1, 1}, up, fallback).x == doctest::Approx(0.1f)); // outside the box
    // Half the probes missing: their weight goes to the fallback.
    for (std::uint32_t z = 0; z < 2; ++z)
        for (std::uint32_t y = 0; y < 2; ++y) (*v)->clear_probe((*v)->grid().index(1, y, z));
    const math::Vec3 half = (*v)->sample({1, 1, 1}, up, fallback);
    CHECK(half.x == doctest::Approx(0.05f)); // probes at x = 0 carry 0, blended 50/50 with the fallback
    CHECK(half.z == doctest::Approx(0.15f));
    CHECK((*v)->upload().ok());
    CHECK((*v)->texture_width() == 4);
    CHECK((*v)->texture_height() == 18);
}

namespace {
struct Room {
    GpuMesh slab;
    GpuMesh cube;
    Environment env;
    DirectionalLight sun;
};

Room make_room() {
    auto& ctx = *gpu().ctx;
    Room r;
    auto slab = ctx.create_mesh(make_cube({3, 0.05f, 3}), "gi_slab");
    auto cube = ctx.create_mesh(make_cube({0.5f, 0.5f, 0.5f}), "gi_cube");
    REQUIRE(slab.ok());
    REQUIRE(cube.ok());
    r.slab = *slab;
    r.cube = *cube;
    r.env.sky_zenith = r.env.sky_horizon = r.env.ground = {0, 0, 0, 1};
    r.env.draw_sky = false;
    r.env.clear_color = {0, 0, 0, 1};
    r.env.vignette = 0.0f;
    r.env.tonemap = false;
    r.env.auto_exposure = false;
    r.sun.intensity = 0.0f;
    r.sun.shadows = false;
    return r;
}

// A glowing red floor below the volume and, optionally, a white diffuse ceiling above it.
void fill_room(const Room& room, SceneRenderer& r, const Camera3D& cam, bool ceiling, bool object,
               math::Vec4 floor_color = {1, 0, 0, 1}) {
    r.begin(cam, room.sun, room.env);
    Material floor;
    floor.unlit = true;
    floor.base_color = {floor_color.x, floor_color.y, floor_color.z, 1};
    r.submit(room.slab, floor, math::Mat4::translation({0, -1.0f, 0}));
    Material white;
    white.base_color = {1, 1, 1, 1};
    white.roughness = 1.0f;
    if (ceiling) r.submit(room.slab, white, math::Mat4::translation({0, 2.2f, 0}));
    if (object) r.submit(room.cube, white, math::Mat4::identity());
}

IrradianceGrid room_grid() {
    IrradianceGrid g;
    g.bounds = {{-2, -0.5f, -2}, {2, 2.0f, 2}};
    g.counts = {3, 3, 3};
    return g;
}
} // namespace

TEST_CASE("gpu: baked probes see the glowing floor below and later passes add a bounce") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    Room room = make_room();
    const CaptureFill fill = [&](SceneRenderer& r, const Camera3D& cam) { fill_room(room, r, cam, true, false); };
    auto one = IrradianceVolume::create(ctx, room_grid());
    auto two = IrradianceVolume::create(ctx, room_grid());
    REQUIRE(one.ok());
    REQUIRE(two.ok());
    IrradianceBakeSettings bs;
    bs.bounces = 1;
    bs.normal_offset = 0.3f; // the ceiling sits just above the top probes
    // Progressive: a budget of 10 probes needs three steps for 27 probes.
    IrradianceBaker baker;
    REQUIRE(baker.step(ctx, **sr, **one, fill, bs, 10).ok());
    CHECK_FALSE(baker.done());
    CHECK((*one)->valid_count() == 10);
    CHECK(baker.progress(**one, bs) == doctest::Approx(10.0f / 27.0f));
    REQUIRE(baker.step(ctx, **sr, **one, fill, bs, 10).ok());
    REQUIRE(baker.step(ctx, **sr, **one, fill, bs, 10).ok());
    CHECK(baker.done());
    CHECK(baker.probes_baked() == 27);
    CHECK((*one)->valid_count() == 27);
    bs.bounces = 2;
    REQUIRE(bake_irradiance_volume(ctx, **sr, **two, fill, bs).ok());

    const std::uint32_t top = (*one)->grid().index(1, 2, 1), bottom = (*one)->grid().index(1, 0, 1);
    const math::Vec3 down_b = (*one)->probe(bottom).diffuse({0, -1, 0}), up_b = (*one)->probe(bottom).diffuse({0, 1, 0});
    const math::Vec3 up1 = (*one)->probe(top).diffuse({0, 1, 0}), up2 = (*two)->probe(top).diffuse({0, 1, 0});
    MESSAGE("bottom probe: facing down " << down_b.x << "," << down_b.y << " facing up " << up_b.x << "; top probe facing up: 1 pass "
                                         << up1.x << ", 2 passes " << up2.x);
    CHECK(down_b.x > 0.5f);       // a red floor filling the lower hemisphere
    CHECK(down_b.y < 0.05f);
    CHECK(up_b.x < down_b.x * 0.5f);
    CHECK(up1.x < 0.1f);          // the ceiling is unlit in the first pass
    CHECK(up2.x > up1.x + 0.05f); // ... and reflects the floor's light in the second
    ctx.destroy(room.slab);
    ctx.destroy(room.cube);
}

TEST_CASE("gpu: lit surfaces inside the volume take diffuse light from its probes") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.ssao = false;
    s.bloom = false;
    (*sr)->set_settings(s);
    Room room = make_room();
    auto vol = IrradianceVolume::create(ctx, room_grid());
    REQUIRE(vol.ok());
    REQUIRE(bake_irradiance_volume(ctx, **sr, **vol, [&](SceneRenderer& r, const Camera3D& cam) {
                fill_room(room, r, cam, false, false);
            }).ok());
    constexpr std::uint32_t W = 64, H = 64;
    auto shoot = [&](bool with_volume, float intensity = 1.0f) {
        fill_room(room, **sr, Camera3D::look_at({0, 0.0f, 3.0f}, {0, 0, 0}), false, true);
        if (with_volume) (*sr)->set_irradiance_volume(vol->get(), intensity, 0.2f);
        auto target = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        dev.destroy(target);
        return px;
    };
    const auto dark = shoot(false);
    CHECK((*sr)->stats().irradiance_probes == 0);
    const auto lit = shoot(true);
    CHECK((*sr)->stats().irradiance_probes == 27);
    const auto bright = shoot(true, 2.0f);
    test::save_output("irradiance_volume_lit", W, H, lit);
    const auto a = pixel(dark, W, W / 2, H / 2), b = pixel(lit, W, W / 2, H / 2), c = pixel(bright, W, W / 2, H / 2);
    MESSAGE("cube front face: no volume " << int(channel(a, 0)) << ", volume " << int(channel(b, 0)) << "," << int(channel(b, 1))
                                          << "," << int(channel(b, 2)) << ", intensity 2 " << int(channel(c, 0)));
    CHECK(channel(a, 0) < 4);                  // black environment, no lights
    CHECK(channel(b, 0) > 30);                 // red light bounced from the floor
    CHECK(channel(b, 1) < channel(b, 0) / 4);
    CHECK(channel(c, 0) > channel(b, 0) + 10); // intensity scales the volume
    // Low tier has volumes off.
    RenderSettings low = RenderSettings::preset(QualityTier::Low);
    CHECK_FALSE(low.irradiance_volumes);
    low.fxaa = false;
    (*sr)->set_settings(low);
    (void)shoot(true);
    CHECK((*sr)->stats().irradiance_probes == 0);
    ctx.destroy(room.slab);
    ctx.destroy(room.cube);
}

TEST_CASE("gpu: IrradianceVolume components bake progressively and rebake on change") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity env = s.create("Environment");
    auto& ec = s.world().add<gameplay::EnvironmentComponent>(env);
    ec.sky_zenith = ec.sky_horizon = ec.ground = {0, 0, 0, 1};
    ec.ambient_intensity = 0.0f;
    ec.draw_sky = false;
    ec.clear_color = {0, 0, 0, 1};
    const scene::Entity cam = s.create("Camera");
    s.world().get<scene::Transform>(cam)->position = {0, 0, 3};
    s.world().add<scene::CameraComponent>(cam);
    const scene::Entity floor = s.create("Floor");
    s.world().get<scene::Transform>(floor)->position = {0, -1, 0};
    s.world().get<scene::Transform>(floor)->scale = {6, 0.1f, 6};
    auto& fm = s.world().add<scene::MeshRendererComponent>(floor);
    fm.primitive = "cube";
    fm.base_color = {0, 0, 0, 1};
    auto& fo = s.world().add<gameplay::MaterialOverridesComponent>(floor);
    fo.override_emissive = true;
    fo.emissive = {1, 0, 0, 1};
    fo.emissive_strength = 1.0f;
    const scene::Entity box = s.create("Box");
    auto& bm = s.world().add<scene::MeshRendererComponent>(box);
    bm.primitive = "cube";
    bm.roughness = 1.0f;
    const scene::Entity vol = s.create("Volume");
    s.world().get<scene::Transform>(vol)->position = {0, 0.5f, 0};
    auto& vc = s.world().add<gameplay::IrradianceVolumeComponent>(vol);
    vc.size = {4, 2, 4};
    vc.spacing = 2.0f;
    vc.bounces = 1;
    s.update_transforms();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    RenderSettings rs = RenderSettings::preset(QualityTier::High);
    rs.taa = false;
    rs.ssao = false;
    rs.bloom = false;
    (*sr)->set_settings(rs);
    cache.set_irradiance_probes_per_frame(10); // 3x2x3 = 18 probes: two frames
    auto stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 10);
    CHECK(stats.irradiance_volumes == 1); // the partial first pass already lights the scene
    CHECK(cache.irradiance_bakes_completed() == 0);
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 8);
    CHECK(cache.irradiance_bakes_completed() == 1);
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 0); // unchanged volume is reused
    constexpr std::uint32_t W = 64, H = 64;
    auto target = test::make_target(W, H);
    auto cmd = ctx.device().begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    CHECK((*sr)->stats().irradiance_probes == 18);
    auto px = ctx.device().read_texture(target).value();
    ctx.device().destroy(target);
    const auto c = pixel(px, W, W / 2, H / 2);
    MESSAGE("box lit by the volume: " << int(channel(c, 0)) << "," << int(channel(c, 1)) << "," << int(channel(c, 2)));
    CHECK(channel(c, 0) > channel(c, 2) + 30);
    s.world().get<gameplay::IrradianceVolumeComponent>(vol)->revision = 1;
    cache.set_irradiance_probes_per_frame(0); // no budget: the old bake keeps lighting
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 0);
    CHECK(stats.irradiance_volumes == 1);
    cache.set_irradiance_probes_per_frame(100);
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 18);
    CHECK(cache.irradiance_bakes_completed() == 2);
    s.destroy(vol);
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_volumes == 0);
    CHECK(cache.irradiance_bakes().empty());
}

TEST_CASE("gpu: dynamic refresh follows a lighting change and stays bounded with feedback") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    Room room = make_room();
    math::Vec4 floor_color{1, 0, 0, 1};
    const CaptureFill fill = [&](SceneRenderer& r, const Camera3D& cam) { fill_room(room, r, cam, true, false, floor_color); };
    auto vol = IrradianceVolume::create(ctx, room_grid());
    REQUIRE(vol.ok());
    IrradianceBakeSettings bs;
    bs.bounces = 1;
    bs.normal_offset = 0.3f;
    REQUIRE(bake_irradiance_volume(ctx, **sr, **vol, fill, bs).ok());
    const std::uint32_t bottom = (*vol)->grid().index(1, 0, 1), top = (*vol)->grid().index(1, 2, 1);
    CHECK((*vol)->probe(bottom).diffuse({0, -1, 0}).x > 0.5f);
    floor_color = {0, 1, 0, 1}; // the floor turns green
    IrradianceRefresher refresher;
    REQUIRE(refresher.step(ctx, **sr, **vol, fill, bs, 20, 1.0f).ok());
    CHECK(refresher.next_probe() == 20);
    REQUIRE(refresher.step(ctx, **sr, **vol, fill, bs, 20, 1.0f).ok()); // wraps around
    CHECK(refresher.next_probe() == 13);
    CHECK(refresher.probes_refreshed() == 40);
    const math::Vec3 now = (*vol)->probe(bottom).diffuse({0, -1, 0});
    MESSAGE("bottom probe after the change: " << now.x << "," << now.y);
    CHECK(now.y > 0.5f);
    CHECK(now.x < 0.05f);
    // Feedback through the ceiling over many cycles converges instead of growing without bound.
    float prev = 0.0f, last = 0.0f;
    for (int cycle = 0; cycle < 6; ++cycle) {
        REQUIRE(refresher.step(ctx, **sr, **vol, fill, bs, 0, 0.5f).ok());
        prev = last;
        last = (*vol)->probe(top).diffuse({0, 1, 0}).y;
    }
    MESSAGE("top probe facing the ceiling: " << prev << " -> " << last);
    CHECK(last > 0.2f);   // the ceiling bounce arrived
    CHECK(last < 1.0f);   // bounded by the floor's radiance
    CHECK(std::abs(last - prev) < 0.1f);
    ctx.destroy(room.slab);
    ctx.destroy(room.cube);
}

TEST_CASE("gpu: dynamic IrradianceVolume components follow emissive changes on High only") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    gameplay::register_components();
    auto& ctx = *gpu().ctx;
    runtime::GpuAssetCache cache(ctx, nullptr);
    scene::Scene s;
    const scene::Entity env = s.create("Environment");
    auto& ec = s.world().add<gameplay::EnvironmentComponent>(env);
    ec.sky_zenith = ec.sky_horizon = ec.ground = {0, 0, 0, 1};
    ec.ambient_intensity = 0.0f;
    ec.draw_sky = false;
    const scene::Entity floor = s.create("Floor");
    s.world().get<scene::Transform>(floor)->position = {0, -1, 0};
    s.world().get<scene::Transform>(floor)->scale = {6, 0.1f, 6};
    auto& fm = s.world().add<scene::MeshRendererComponent>(floor);
    fm.primitive = "cube";
    fm.base_color = {0, 0, 0, 1};
    auto& fo = s.world().add<gameplay::MaterialOverridesComponent>(floor);
    fo.override_emissive = true;
    fo.emissive = {1, 0, 0, 1};
    fo.emissive_strength = 1.0f;
    const scene::Entity vol = s.create("Volume");
    s.world().get<scene::Transform>(vol)->position = {0, 0.5f, 0};
    auto& vc = s.world().add<gameplay::IrradianceVolumeComponent>(vol);
    vc.size = {4, 2, 4};
    vc.spacing = 2.0f;
    vc.bounces = 1;
    vc.dynamic = true;
    vc.dynamic_blend = 1.0f;
    s.update_transforms();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    cache.set_irradiance_probes_per_frame(18);
    auto stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_baked == 18);
    CHECK(stats.irradiance_probes_refreshed == 0); // the budget went to the bake
    auto& bake = cache.irradiance_bakes().begin()->second;
    REQUIRE(bake.active);
    const std::uint32_t probe = bake.active->grid().index(1, 0, 1);
    CHECK(bake.active->probe(probe).diffuse({0, -1, 0}).x > 0.5f);
    s.world().get<gameplay::MaterialOverridesComponent>(floor)->emissive = {0, 0, 1, 1};
    // Medium tier: dynamic GI is off, the bake stays as it was.
    RenderSettings medium = RenderSettings::preset(QualityTier::Medium);
    CHECK_FALSE(medium.dynamic_gi);
    (*sr)->set_settings(medium);
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_refreshed == 0);
    (*sr)->set_settings(RenderSettings::preset(QualityTier::High));
    stats = runtime::extract_scene(s, cache, **sr);
    CHECK(stats.irradiance_probes_refreshed == 18);
    CHECK(stats.irradiance_probes_baked == 0);
    const math::Vec3 now = cache.irradiance_bakes().begin()->second.active->probe(probe).diffuse({0, -1, 0});
    MESSAGE("after the emissive change: " << now.x << "," << now.y << "," << now.z);
    CHECK(now.z > 0.5f);
    CHECK(now.x < 0.05f);
}
