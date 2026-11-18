// SPDX-License-Identifier: Apache-2.0
// Terrain (ADR-0063) renders its chunks through the built-in splat surface shader: layer tints
// follow the splat map and edits re-upload only the touched chunks.
#include "gpu_fixture.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/terrain/terrain.hpp"

#include <cmath>

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

// Roadmap 6b, Workstream H3 item 8: chunk boundaries during zoom. Rolling hills seen at a grazing
// angle mix LOD levels between neighbouring chunks; at every FOV the LOD frame must not open a
// crack where the LOD 0 frame shows ground (skirts hide level mismatches).
TEST_CASE("gpu: terrain chunk seams stay closed across FOV zoom and mixed LODs") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto hf_r = terrain::make_flat(129, 0.0f);
    REQUIRE(hf_r.ok());
    auto hf = std::make_shared<terrain::Heightfield>(std::move(*hf_r));
    for (std::uint32_t z = 0; z < hf->resolution; ++z)
        for (std::uint32_t x = 0; x < hf->resolution; ++x)
            hf->heights[hf->index(x, z)] =
                0.5f + 0.25f * std::sin(static_cast<float>(x) * 0.21f) * std::cos(static_cast<float>(z) * 0.17f) +
                0.1f * std::sin(static_cast<float>(x + 2 * z) * 0.53f);
    hf->touch_all();
    const Uuid id = Uuid::generate();
    terrain::TerrainSystem system;
    system.add_data(id, hf);
    auto build = [&](bool with_terrain) {
        auto s = std::make_unique<Scene>();
        if (with_terrain) {
            const Entity t = s->create("Terrain");
            terrain::TerrainComponent comp;
            comp.data = id;
            comp.size = {160, 12, 160};
            comp.layer0_tint = {0.2f, 0.5f, 0.15f, 1};
            comp.collision = false;
            s->world().add<terrain::TerrainComponent>(t, comp);
        }
        s->update_transforms();
        system.update(*s);
        return s;
    };
    auto with = build(true);
    auto without = build(false);

    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    REQUIRE(cache.terrain_shader().ok());
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    constexpr std::uint32_t TW = 240, TH = 136;
    float budget = 1.0f;
    auto frame = [&](Scene& s, float fov_deg, bool lods, render::SceneStats* st) {
        render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::High);
        rs.taa = false;
        rs.ssao = false;
        rs.bloom = false;
        rs.auto_exposure = false;
        rs.lods = lods;
        rs.lod_error_pixels = budget;
        sr.value()->set_settings(rs);
        render::Camera3D cam = render::Camera3D::look_at({-70, 16, -70}, {10, 4, 10});
        cam.fov_y = math::radians(fov_deg);
        s.update_transforms();
        (void)runtime::extract_scene(s, cache, **sr, {}, &cam);
        auto target = test::make_target(TW, TH);
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, TW, TH, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        if (st) *st = sr.value()->stats();
        dev.destroy(target);
        return px;
    };
    auto dmax = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, std::size_t i) {
        int m = 0;
        for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[i + c]) - int(b[i + c])));
        return m;
    };
    // The default 1 px budget, then a loose 8 px budget that forces large level differences
    // between neighbours (the skirts must still cover them).
    for (const float b : {1.0f, 8.0f})
    for (float fov : {75.0f, 35.0f, 12.0f, 4.0f}) {
        budget = b;
        render::SceneStats st{}, st_full{};
        const auto empty = frame(*without, fov, true, nullptr);
        const auto full = frame(*with, fov, false, &st_full);
        const auto lod = frame(*with, fov, true, &st);
        if (fov == 75.0f) test::save_output(budget > 1.0f ? "terrain_seams_lod8" : "terrain_seams_lod", TW, TH, lod);
        // A hole is a pixel where the LOD frame shows sky but LOD 0 shows ground. On a ridge line a
        // coarser level may lower the silhouette by up to the pixel budget (ground below, sky
        // above); a crack is a hole inside the ground, with LOD-frame ground both above and below
        // it within two pixels (a seam opening anywhere below the horizon has farther ground
        // above it).
        auto sky = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= int(TW) || y >= int(TH)) return true;
            return dmax(lod, empty, (std::size_t(y) * TW + std::size_t(x)) * 4) <= 6;
        };
        std::size_t ground = 0, holes = 0, cracks = 0;
        for (int y = 0; y < int(TH); ++y)
            for (int x = 0; x < int(TW); ++x) {
                const std::size_t i = (std::size_t(y) * TW + std::size_t(x)) * 4;
                if (dmax(full, empty, i) <= 24) continue; // no ground at LOD 0 either
                ++ground;
                if (!sky(x, y)) continue;
                ++holes;
                if ((!sky(x, y - 1) || !sky(x, y - 2)) && (!sky(x, y + 1) || !sky(x, y + 2))) ++cracks;
            }
        INFO("budget " << budget << " px, fov " << fov << ": " << st.visible << " chunks, " << st.lod_draws << " at a coarser level, ground " << ground
                    << ", silhouette holes " << holes << ", cracks " << cracks);
        CHECK(ground > TW * TH / 4);
        CHECK(cracks == 0);
        if (budget <= 1.0f) CHECK(holes <= ground / 1000 + 4);
        CHECK(st.triangles <= st_full.triangles);
        if (fov >= 70.0f) {
            // Neighbouring chunks really are at different levels in the wide view.
            CHECK(st.lod_draws > 0);
            CHECK(st.lod_draws < st.visible);
        }
    }
    cache.clear();
}
