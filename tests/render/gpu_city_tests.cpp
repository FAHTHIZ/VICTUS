// SPDX-License-Identifier: Apache-2.0
// Procedural city (ADR-0202): the generated cell meshes draw lit and shadowed, from the air and
// from the street, with front faces kept by back-face culling.
#include "gpu_fixture.hpp"
#include "victus/procgen/city.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <chrono>
#include <cmath>
#include <set>

using namespace victus;
using namespace victus::scene;
using victus::test::gpu;

namespace {

struct Shot {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
    double extract_ms{0};
};

constexpr std::uint32_t W = 480, H = 270;

Shot shoot(Scene& s, runtime::GpuAssetCache& cache, Entity cam, math::Vec3 eye, float yaw_deg, float pitch_deg, const char* name) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    (*sr)->set_settings(render::RenderSettings::preset(render::QualityTier::High));
    auto* t = s.world().get<Transform>(cam);
    t->position = eye;
    t->rotation = math::Quat::axis_angle({0, 1, 0}, math::radians(yaw_deg)) * math::Quat::axis_angle({1, 0, 0}, math::radians(pitch_deg));
    s.update_transforms();
    auto target = test::make_target(W, H);
    Shot shot;
    using clock = std::chrono::steady_clock;
    for (int i = 0; i < 3; ++i) { // warm uploads and temporal history, keep the last frame
        const auto t0 = clock::now();
        shot.stats = runtime::extract_scene(s, cache, **sr, {});
        shot.extract_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output(name, W, H, *px);
    shot.px = std::move(*px);
    dev.destroy(target);
    return shot;
}

// Distinct quantised colours in the bottom two thirds: a flat or culled-away city has few.
std::size_t detail(const std::vector<std::uint8_t>& px) {
    std::set<std::uint32_t> colours;
    for (std::uint32_t y = H / 3; y < H; y += 2)
        for (std::uint32_t x = 0; x < W; x += 2) {
            const std::uint8_t* p = &px[(y * W + x) * 4];
            colours.insert((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
        }
    return colours.size();
}

} // namespace

TEST_CASE("gpu: procedural city draws from the air and from the street") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    procgen::register_components();
    Scene s;
    const Entity cam = s.create("Camera");
    s.world().add<CameraComponent>(cam).z_far = 2000.0f;
    const Entity sun = s.create("Sun");
    s.world().get<Transform>(sun)->rotation =
        math::Quat::axis_angle({0, 1, 0}, math::radians(35)) * math::Quat::axis_angle({1, 0, 0}, math::radians(-40));
    auto& light = s.world().add<LightComponent>(sun);
    light.shadows = true;
    light.intensity = 3.0f;
    const Entity owner = s.create("City");
    procgen::CityComponent cc;
    cc.seed = 11;
    cc.columns = 5;
    cc.rows = 5;
    cc.vehicles = 0;
    cc.pedestrians = 0;
    cc.colliders = false;
    s.world().add<procgen::CityComponent>(owner, cc);
    REQUIRE(procgen::build_city(s));
    const auto* st = procgen::city_stats(s);
    REQUIRE(st != nullptr);
    MESSAGE("city: " << st->buildings << " buildings, " << st->meshes << " meshes, " << st->triangles << " triangles, generate "
                     << st->generate_ms << " ms + meshes " << st->mesh_ms << " ms");

    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    const Shot air = shoot(s, cache, cam, {-260, 170, 260}, -45, -28, "city_aerial");
    CHECK(air.stats.meshes == st->meshes);
    CHECK(air.stats.missing_assets == 0);
    const std::size_t air_detail = detail(air.px);
    MESSAGE("aerial: " << air_detail << " colours, extract " << air.extract_ms << " ms");
    CHECK(air_detail > 150);

    // Street level on an avenue, looking down the road between the buildings.
    const procgen::City* city = procgen::scene_city(s);
    REQUIRE(city != nullptr);
    const float x = city->xs[city->xs.size() / 2];
    const Shot street = shoot(s, cache, cam, {x + 2.0f, 1.7f, city->bounds.max.z - 5.0f}, 0, 4, "city_street");
    const std::size_t street_detail = detail(street.px);
    MESSAGE("street: " << street_detail << " colours, extract " << street.extract_ms << " ms");
    CHECK(street_detail > 150);
}
