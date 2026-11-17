// SPDX-License-Identifier: Apache-2.0
// Terrain (ADR-0063) renders its chunks through the built-in splat surface shader: layer tints
// follow the splat map and edits re-upload only the touched chunks.
#include "gpu_fixture.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/terrain/terrain.hpp"

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
constexpr std::uint32_t W = 96, H = 64;

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
};

Frame draw(Scene& s, runtime::GpuAssetCache& cache) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    auto target = test::make_target(W, H);
    s.update_transforms();
    Frame f;
    f.stats = runtime::extract_scene(s, cache, **sr, {});
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("terrain", W, H, *px);
    f.px = std::move(*px);
    dev.destroy(target);
    return f;
}
} // namespace

TEST_CASE("gpu: terrain chunks draw with splat-blended layer tints") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    // Flat field: layer 0 (red) for x < 0, layer 1 (green) for x > 0.
    auto hf_r = terrain::make_flat(65, 0.0f);
    REQUIRE(hf_r.ok());
    auto hf = std::make_shared<terrain::Heightfield>(std::move(*hf_r));
    for (std::uint32_t z = 0; z < hf->resolution; ++z)
        for (std::uint32_t x = 0; x < hf->resolution; ++x)
            hf->splat[hf->index(x, z)] = terrain::pack_weights(x < 32 ? math::Vec4{1, 0, 0, 0} : math::Vec4{0, 1, 0, 0});
    hf->touch_all();
    const Uuid id = Uuid::generate();
    terrain::TerrainSystem system;
    system.add_data(id, hf);

    Scene s;
    const Entity cam = s.create("Camera");
    s.world().get<Transform>(cam)->position = {0, 12, 0};
    s.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    s.world().add<CameraComponent>(cam);
    const Entity t = s.create("Terrain");
    terrain::TerrainComponent comp;
    comp.data = id;
    comp.size = {40, 4, 40};
    comp.layer0_tint = {0.8f, 0.05f, 0.05f, 1};
    comp.layer1_tint = {0.05f, 0.8f, 0.05f, 1};
    comp.collision = false;
    s.world().add<terrain::TerrainComponent>(t, comp);
    s.update_transforms();
    system.update(s);

    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    REQUIRE(cache.terrain_shader().ok());
    Frame a = draw(s, cache);
    CHECK(a.stats.terrain_chunks == 4);
    CHECK(a.stats.missing_assets == 0);
    const auto l = pixel(a.px, W, 20, 32), r = pixel(a.px, W, 76, 32);
    MESSAGE("left " << channel(l, 0) << "," << channel(l, 1) << " right " << channel(r, 0) << "," << channel(r, 1));
    CHECK(channel(l, 0) > channel(l, 1) + 40);
    CHECK(channel(r, 1) > channel(r, 0) + 40);
    CHECK(cache.procedural_count() == 4);

    // Repaint the left half green: only the two left chunks are rebuilt and uploaded.
    for (std::uint32_t z = 0; z < hf->resolution; ++z)
        for (std::uint32_t x = 0; x < 30; ++x) hf->splat[hf->index(x, z)] = terrain::pack_weights({0, 1, 0, 0});
    hf->touch(0, 0, 29, 64);
    const std::uint32_t rebuilds = system.stats().chunk_rebuilds;
    system.update(s);
    CHECK(system.stats().chunk_rebuilds == rebuilds + 2);
    a = draw(s, cache);
    const auto l2 = pixel(a.px, W, 20, 32);
    CHECK(channel(l2, 1) > channel(l2, 0) + 40);
    cache.clear();
}

TEST_CASE("gpu: foliage draws its mesh renderer on the painted layer within the draw distance") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    // Layer 1 painted on the right half only: blue cubes scatter there and nowhere else.
    auto hf_r = terrain::make_flat(65, 0.0f);
    REQUIRE(hf_r.ok());
    auto hf = std::make_shared<terrain::Heightfield>(std::move(*hf_r));
    for (std::uint32_t z = 0; z < hf->resolution; ++z)
        for (std::uint32_t x = 0; x < hf->resolution; ++x)
            hf->splat[hf->index(x, z)] = terrain::pack_weights(x < 32 ? math::Vec4{1, 0, 0, 0} : math::Vec4{0, 1, 0, 0});
    hf->touch_all();
    const Uuid id = Uuid::generate();
    terrain::TerrainSystem system;
    system.add_data(id, hf);

    Scene s;
    const Entity cam = s.create("Camera");
    s.world().get<Transform>(cam)->position = {0, 12, 0};
    s.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    s.world().add<CameraComponent>(cam);
    const Entity t = s.create("Terrain");
    terrain::TerrainComponent comp;
    comp.data = id;
    comp.size = {40, 4, 40};
    comp.layer0_tint = {0.6f, 0.6f, 0.6f, 1};
    comp.layer1_tint = {0.6f, 0.6f, 0.6f, 1};
    comp.collision = false;
    s.world().add<terrain::TerrainComponent>(t, comp);
    const Entity bushes = s.create("Bushes", t);
    terrain::FoliageComponent f;
    f.layer = 1;
    f.min_weight = 0.5f;
    f.density = 2.0f;
    f.min_scale = f.max_scale = 0.6f;
    s.world().add<terrain::FoliageComponent>(bushes, f);
    MeshRendererComponent mr;
    mr.primitive = "cube";
    mr.base_color = {0.05f, 0.1f, 0.9f, 1};
    s.world().add<MeshRendererComponent>(bushes, mr);
    s.update_transforms();
    system.update(s);
    const auto* rt = s.world().get<terrain::FoliageRuntime>(bushes);
    REQUIRE(rt);
    CHECK(rt->instances > 1000u);
    for (const auto& chunk : rt->chunks)
        for (const auto& m : chunk) CHECK(m.transform_point({}).x >= -0.4f);

    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    Frame a = draw(s, cache);
    CHECK(a.stats.foliage_instances == rt->instances);
    CHECK(a.stats.meshes == 1);
    const auto l = pixel(a.px, W, 20, 32), r = pixel(a.px, W, 76, 32);
    MESSAGE("left " << channel(l, 0) << "," << channel(l, 2) << " right " << channel(r, 0) << "," << channel(r, 2));
    // Bare (lit grey) ground on the left; the right is mostly covered by blue cubes.
    CHECK(channel(r, 0) + 60 < channel(l, 0));
    CHECK(channel(r, 2) > channel(r, 0) + 100);
    CHECK(channel(l, 2) < channel(l, 0) + 80);

    // A short draw distance keeps only instances near the camera (12 m above the centre).
    s.world().get<terrain::FoliageComponent>(bushes)->draw_distance = 14.0f;
    system.update(s);
    a = draw(s, cache);
    CHECK(a.stats.foliage_instances > 0u);
    CHECK(a.stats.foliage_instances < rt->instances / 2);
    cache.clear();
}
