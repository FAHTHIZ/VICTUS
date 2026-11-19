// SPDX-License-Identifier: Apache-2.0
// V.A.U.L.T. asynchronous loading through GpuAssetCache (ADR-0087): assets requested by the
// renderer decode on background jobs, upload under a per-pump budget, are skipped (not drawn
// wrong) while loading, and failures are cached until evicted.
#include "gpu_fixture.hpp"
#include "victus/core/jobs.hpp"
#include "victus/render/image.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <filesystem>

using namespace victus;
using victus::test::gpu;
namespace sfs = std::filesystem;

namespace {
struct AsyncProject {
    sfs::path root;
    std::unique_ptr<assets::AssetDatabase> db;
    Uuid texture, model;
};

AsyncProject make_project() {
    AsyncProject p;
    p.root = sfs::temp_directory_path() / ("victus_vault_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(p.root / "assets" / "textures");
    sfs::create_directories(p.root / "assets" / "models");
    REQUIRE(render::save_png(p.root / "assets" / "textures" / "wall.png", render::make_checker(8, 1, 0xff0000ffu, 0xff0000ffu)).ok());
    sfs::copy_file(fs::Path(VICTUS_SOURCE_DIR) / "samples" / "arena" / "assets" / "models" / "robot.gltf",
                   p.root / "assets" / "models" / "robot.gltf");
    auto db = assets::AssetDatabase::open(p.root);
    REQUIRE(db.ok());
    p.db = std::move(*db);
    REQUIRE(p.db->import_all().failed == 0);
    p.texture = p.db->find_path("textures/wall.png")->uuid;
    p.model = p.db->find_path("models/robot.gltf")->uuid;
    return p;
}
} // namespace

TEST_CASE("gpu async loading: textures and models stream in under the upload budget") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    jobs::JobSystem js(2); // declared before the cache: the loader waits on it when destroyed
    runtime::GpuAssetCache cache(*gpu().ctx, p.db.get());
    // A 1-byte budget still uploads one asset per pump, never two.
    cache.set_async_loading(&js, {.upload_budget_bytes = 1, .max_in_flight = 1});
    REQUIRE(cache.async_loading());

    auto tex = cache.texture(p.texture);
    REQUIRE_FALSE(tex.ok());
    CHECK(tex.error().code == ErrorCode::Pending);
    auto model = cache.model(p.model);
    REQUIRE_FALSE(model.ok());
    CHECK(model.error().code == ErrorCode::Pending);
    CHECK(cache.async_stats().pending == 2);

    // A mesh that references the loading model is skipped and counted as loading, not missing.
    scene::Scene s;
    const scene::Entity e = s.create("Robot");
    s.world().add<scene::MeshRendererComponent>(e).mesh_asset = p.model;
    s.update_transforms();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());

    std::uint32_t pumps = 0, uploads = 0;
    while (cache.async_stats().pending > 0 && pumps < 2000) {
        cache.wait_loads();
        const std::uint32_t n = cache.pump();
        CHECK(n <= 1);
        uploads += n;
        ++pumps;
    }
    CHECK(uploads == 2);
    CHECK(cache.async_stats().uploaded == 2);
    tex = cache.texture(p.texture);
    REQUIRE(tex.ok());
    const auto px = gpu().ctx->device().read_texture(*tex);
    REQUIRE(px.ok());
    CHECK((*px)[0] == 255);
    CHECK((*px)[1] == 0);
    model = cache.model(p.model);
    REQUIRE(model.ok());
    CHECK_FALSE((*model)->meshes.empty());
    const runtime::ExtractStats drawn = runtime::extract_scene(s, cache, **sr);
    CHECK(drawn.meshes >= 1);
    CHECK(drawn.loading_assets == 0);
    CHECK(drawn.missing_assets == 0);

    // A model evicted (hot reload) is requested again; extraction skips it until it is back.
    cache.evict(p.model);
    const runtime::ExtractStats waiting = runtime::extract_scene(s, cache, **sr);
    CHECK(waiting.meshes == 0);
    CHECK(waiting.loading_assets == 1);
    CHECK(waiting.missing_assets == 0);
    cache.finish_loads();
    CHECK(runtime::extract_scene(s, cache, **sr).meshes >= 1);
}

TEST_CASE("gpu async loading: failures are cached, sprites wait, synchronous mode is unchanged") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    jobs::JobSystem js(1);
    runtime::GpuAssetCache cache(*gpu().ctx, p.db.get());
    cache.set_async_loading(&js);

    // Unknown asset: pending first, then the cached failure with the loader's message.
    const Uuid unknown = Uuid::generate();
    CHECK(cache.texture(unknown).error().code == ErrorCode::Pending);
    cache.finish_loads();
    auto failed = cache.texture(unknown);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == ErrorCode::NotFound);
    CHECK(cache.async_stats().pending == 0); // not retried every frame
    cache.evict(unknown);                    // eviction forgets the failure: the next call retries
    CHECK(cache.texture(unknown).error().code == ErrorCode::Pending);
    cache.finish_loads();

    // A sprite whose texture is loading is not drawn as a white quad.
    scene::Scene s;
    const scene::Entity e = s.create("Sprite");
    s.world().add<scene::SpriteComponent>(e).texture_asset = p.texture;
    s.update_transforms();
    auto batch = render::SpriteBatch::create(*gpu().ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    (*batch)->begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    CHECK(runtime::extract_sprites(s, cache, **batch) == 0);
    cache.finish_loads();
    (*batch)->begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    CHECK(runtime::extract_sprites(s, cache, **batch) == 1);

    // Detaching returns to synchronous loading: the first call loads and uploads in place.
    cache.set_async_loading(nullptr);
    CHECK_FALSE(cache.async_loading());
    cache.evict(p.texture);
    CHECK(cache.texture(p.texture).ok());
    CHECK(cache.model(p.model).ok());
}

TEST_CASE("gpu residency: over the budget, idle assets are evicted least recently used first") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    // Two more 8x8 textures: each uploads 8x8 + 4x4 + 2x2 + 1x1 RGBA8 mips (or one compressed chain).
    for (const char* name : {"b.png", "c.png"})
        REQUIRE(render::save_png(p.root / "assets" / "textures" / name, render::make_checker(8, 1, 0xff00ff00u, 0xff00ff00u)).ok());
    (void)p.db->refresh();
    REQUIRE(p.db->import_all().failed == 0);
    const Uuid a = p.texture, b = p.db->find_path("textures/b.png")->uuid, c = p.db->find_path("textures/c.png")->uuid;
    runtime::GpuAssetCache cache(*gpu().ctx, p.db.get()); // synchronous loads: residency works in both modes
    REQUIRE(cache.texture(a).ok());
    const std::size_t one = cache.residency().resident_bytes;
    REQUIRE(one > 0);
    cache.set_residency({.budget_bytes = one * 2, .min_idle_frames = 3});

    // Frame 1: a and b are used (two textures, at the budget).
    (void)cache.pump();
    REQUIRE(cache.texture(a).ok());
    REQUIRE(cache.texture(b).ok());
    CHECK(cache.residency().resident_assets == 2);
    // Then only c and b are used: a goes idle; c pushes residency over the budget.
    const std::uint64_t gpu_before = gpu().ctx->device().stats().texture_bytes;
    std::uint64_t evicted_at = 0;
    for (int frame = 0; frame < 6; ++frame) {
        (void)cache.pump();
        REQUIRE(cache.texture(c).ok());
        REQUIRE(cache.texture(b).ok());
        if (!evicted_at && cache.residency().evicted == 1) evicted_at = cache.residency().frame;
    }
    const runtime::ResidencyStats r = cache.residency();
    CHECK(r.evicted == 1);
    CHECK(r.evicted_bytes == one);
    CHECK(r.resident_assets == 2);
    CHECK(r.resident_bytes <= one * 2);
    CHECK_FALSE(r.over_budget);
    CHECK(evicted_at == 4); // a was last used in frame 1: idle for 3 frames at pump 4
    CHECK(gpu().ctx->device().stats().texture_bytes < gpu_before + one); // a's memory was released

    // Assets in recent use are never evicted, even far over the budget.
    cache.set_residency({.budget_bytes = 1, .min_idle_frames = 3});
    for (int frame = 0; frame < 5; ++frame) {
        (void)cache.pump();
        REQUIRE(cache.texture(c).ok());
        REQUIRE(cache.texture(b).ok());
    }
    CHECK(cache.residency().resident_assets == 2);
    CHECK(cache.residency().over_budget);

    // A material in use keeps its texture resident through the cached material alone.
    assets::MaterialAsset m;
    m.base_color_texture = a;
    sfs::create_directories(p.root / "assets" / "materials");
    REQUIRE(assets::save_material_file(p.root / "assets" / "materials" / "wall.vmat", m).ok());
    (void)p.db->refresh();
    const Uuid mat = p.db->find_path("materials/wall.vmat")->uuid;
    cache.set_residency({.budget_bytes = one, .min_idle_frames = 2});
    REQUIRE(cache.material(mat).ok()); // loads a (evicted above) through the material
    const std::uint64_t evicted_before = cache.residency().evicted;
    for (int frame = 0; frame < 6; ++frame) {
        (void)cache.pump();
        REQUIRE(cache.material(mat).ok());
    }
    // b and c went idle and were evicted; a stayed because the material kept touching it.
    CHECK(cache.residency().evicted == evicted_before + 2);
    CHECK(cache.residency().resident_assets == 1);
    CHECK(cache.material(mat)->base_color_texture == *cache.texture(a));

    // An evicted texture reloads on its next use.
    REQUIRE(cache.texture(b).ok());
    CHECK(cache.residency().resident_assets == 2);
}
