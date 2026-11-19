// SPDX-License-Identifier: Apache-2.0
// V.A.U.L.T. asynchronous loading through GpuAssetCache (ADR-0087): assets requested by the
// renderer decode on background jobs, upload under a per-pump budget, are skipped (not drawn
// wrong) while loading, and failures are cached until evicted.
#include "../assets/gltf_writer.hpp"
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

namespace {
Uuid add_texture(AsyncProject& p, const char* name, std::uint32_t size) {
    REQUIRE(render::save_png(p.root / "assets" / "textures" / name, render::make_checker(size, 8, 0xff0000ffu, 0xff00ff00u)).ok());
    (void)p.db->refresh();
    REQUIRE(p.db->import_all().failed == 0);
    return p.db->find_path(std::string("textures/") + name)->uuid;
}
std::uint32_t gpu_width(runtime::GpuAssetCache& cache, const Uuid& id) {
    auto h = cache.texture(id);
    REQUIRE(h.ok());
    const rhi::TextureDesc* d = gpu().ctx->device().describe(*h);
    REQUIRE(d);
    return d->width;
}
} // namespace

TEST_CASE("gpu mip streaming: texture levels follow the on-screen footprint and the budget") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    const Uuid big = add_texture(p, "big.png", 256);
    runtime::GpuAssetCache cache(*gpu().ctx, p.db.get());
    cache.set_residency({.min_idle_frames = 60, .mip_streaming = true, .min_streamed_size = 16, .mip_drop_frames = 2});

    // Seen at 32 px before it loads: only 32x32 and below are uploaded.
    cache.note_footprint(big, 32.0f);
    REQUIRE(cache.texture(big).ok());
    CHECK(cache.texture_level(big) == 3u);
    CHECK(gpu_width(cache, big) == 32);
    const std::size_t small_bytes = cache.residency().resident_bytes;
    CHECK(cache.residency().streamed_textures == 1);

    const auto frame = [&](float px) {
        (void)cache.pump();
        (void)cache.texture(big);
        cache.note_footprint(big, px);
    };
    // Close up: full resolution on the next pump (raises are immediate).
    frame(300.0f);
    frame(300.0f);
    CHECK(cache.texture_level(big) == 0u);
    CHECK(gpu_width(cache, big) == 256);
    CHECK(cache.residency().resident_bytes > small_bytes * 30);
    CHECK(cache.residency().streamed_textures == 0);
    // Far away: drops after mip_drop_frames, never below min_streamed_size (level 4 = 16 px).
    frame(5.0f);
    frame(5.0f);
    CHECK(cache.texture_level(big) == 0u); // hysteresis: not yet
    frame(5.0f);
    frame(5.0f);
    CHECK(cache.texture_level(big) == 4u);
    CHECK(gpu_width(cache, big) == 16);
    const std::uint64_t restreams = cache.residency().restreams;
    CHECK(restreams == 2);

    // A raise that does not fit the budget stops at the most detail that does.
    const std::size_t level4 = cache.residency().resident_bytes;
    cache.set_residency({.budget_bytes = level4 * 20, .min_idle_frames = 60, .mip_streaming = true, .min_streamed_size = 16,
                         .mip_drop_frames = 2});
    frame(300.0f);
    frame(300.0f);
    CHECK(cache.texture_level(big) == 2u); // 64x64 chain ~ 16x the 16x16 one; 128x128 would not fit
    // Shrinking the budget under a texture in use drops its top level, one per pump.
    cache.set_residency({.budget_bytes = level4 * 2, .min_idle_frames = 60, .mip_streaming = true, .min_streamed_size = 16,
                         .mip_drop_frames = 2});
    for (int i = 0; i < 4; ++i) frame(300.0f);
    CHECK(cache.texture_level(big) == 4u);
    CHECK(cache.residency().budget_drops == 2);
    CHECK(cache.residency().resident_bytes <= level4 * 2);

    // Eviction forgets the levels; footprints noted since make the reload start small again.
    cache.evict(big);
    (void)cache.pump();
    cache.note_footprint(big, 10.0f);
    CHECK(gpu_width(cache, big) == 16);
    // Small textures never stream.
    const Uuid tiny = add_texture(p, "tiny.png", 16);
    cache.note_footprint(tiny, 1.0f);
    CHECK(gpu_width(cache, tiny) == 16);
    CHECK(cache.texture_level(tiny) == 0u);
}

TEST_CASE("gpu mip streaming: extraction measures material and decal footprints (sync and async)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    const Uuid big = add_texture(p, "big.png", 256);
    assets::MaterialAsset m;
    m.base_color_texture = big;
    sfs::create_directories(p.root / "assets" / "materials");
    REQUIRE(assets::save_material_file(p.root / "assets" / "materials" / "wall.vmat", m).ok());
    (void)p.db->refresh();
    const Uuid mat = p.db->find_path("materials/wall.vmat")->uuid;
    const Uuid decal_tex = add_texture(p, "decal.png", 256);

    for (const bool async : {false, true}) {
        CAPTURE(async);
        jobs::JobSystem js(1);
        runtime::GpuAssetCache cache(*gpu().ctx, p.db.get());
        if (async) cache.set_async_loading(&js);
        cache.set_residency({.min_idle_frames = 60, .mip_streaming = true, .screen_height = 720, .min_streamed_size = 16,
                             .mip_drop_frames = 1});
        scene::Scene s;
        const scene::Entity e = s.create("Wall");
        auto& mr = s.world().add<scene::MeshRendererComponent>(e);
        mr.primitive = "cube";
        mr.material_asset = mat;
        s.world().add<gameplay::DecalComponent>(s.create("Decal")).texture = decal_tex;
        auto sr = render::SceneRenderer::create(*gpu().ctx);
        REQUIRE(sr.ok());
        const auto run = [&](float distance, int frames) {
            const render::Camera3D cam = render::Camera3D::look_at({0, 0, distance}, {0, 0, 0});
            s.update_transforms();
            for (int i = 0; i < frames; ++i) {
                (void)runtime::extract_scene(s, cache, **sr, {}, &cam);
                if (async) cache.wait_loads();
            }
        };
        // A 1 m cube 200 m away covers ~6 px of a 720 px screen: it keeps the 16 px floor.
        run(200.0f, 8);
        CHECK(cache.texture_level(big) == 4u);
        CHECK(cache.texture_level(decal_tex) == 4u); // decals: footprint of the projection box
        // 2 m away it covers several hundred pixels: full resolution.
        run(2.0f, 6);
        CHECK(cache.texture_level(big) == 0u);
        CHECK(cache.texture_level(decal_tex) == 0u);
        // The material's texture: synchronous loads upload it in full before the first footprint
        // and then drop it; async loads decode after it and upload only the far level. The decal
        // is noted before its first lookup, so it starts small in both modes and is raised once.
        CHECK(cache.residency().restreams == (async ? 2u : 3u));
        // 30 m: ~37 px wants the 64 px level (256 / 2^2).
        run(30.0f, 6);
        CHECK(cache.texture_level(big) == 2u);
    }
}

TEST_CASE("gpu mip streaming: textures embedded in glTF models follow their mesh footprints") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    AsyncProject p = make_project();
    REQUIRE(render::save_png(p.root / "assets" / "models" / "albedo.png", render::make_checker(256, 8, 0xff0000ffu, 0xff00ff00u)).ok());
    test::GltfFixture fx;
    fx.add_texture("albedo.png");
    REQUIRE(fs::write_text_atomic(p.root / "assets" / "models" / "quad.gltf", fx.gltf_with_data_uri()).ok());
    (void)p.db->refresh();
    REQUIRE(p.db->import_all().failed == 0);
    const Uuid model = p.db->find_path("models/quad.gltf")->uuid;

    // One texture decodes without the meshes, and matches the full model decode.
    auto full = p.db->load_model(model);
    REQUIRE(full.ok());
    REQUIRE(full->textures.size() == 1);
    auto only = p.db->load_model_texture(model, 0);
    REQUIRE(only.ok());
    CHECK(only->mips.size() == full->textures[0].mips.size());
    CHECK(only->mips[0].width == 256);
    CHECK(only->mips.back().pixels == full->textures[0].mips.back().pixels);
    CHECK(p.db->load_model_texture(model, 1).error().code == ErrorCode::NotFound);

    for (const bool async : {false, true}) {
        CAPTURE(async);
        jobs::JobSystem js(1);
        runtime::GpuAssetCache cache(*gpu().ctx, p.db.get());
        if (async) cache.set_async_loading(&js);
        cache.set_residency({.min_idle_frames = 60, .mip_streaming = true, .screen_height = 720, .min_streamed_size = 16,
                             .mip_drop_frames = 1});
        scene::Scene s;
        const scene::Entity e = s.create("Quad");
        auto& mr = s.world().add<scene::MeshRendererComponent>(e);
        mr.mesh_asset = model;
        mr.mesh_index = 0;
        auto sr = render::SceneRenderer::create(*gpu().ctx);
        REQUIRE(sr.ok());
        const auto run = [&](float distance, int frames) {
            const render::Camera3D cam = render::Camera3D::look_at({0, 0, distance}, {0, 0, 0});
            s.update_transforms();
            for (int i = 0; i < frames; ++i) {
                (void)runtime::extract_scene(s, cache, **sr, {}, &cam);
                if (async) cache.wait_loads();
            }
        };
        const auto width = [&]() -> std::uint32_t {
            auto m = cache.model(model);
            REQUIRE(m.ok());
            const rhi::TextureDesc* d = gpu().ctx->device().describe((*m)->textures[0]);
            REQUIRE(d);
            // The mesh's resolved material samples the live (re-streamed) handle.
            CHECK((*m)->mesh_materials[0].base_color_texture == (*m)->textures[0]);
            return d->width;
        };
        // Uploaded in full on arrival, then dropped to the 16 px floor ~200 m away.
        run(200.0f, 8);
        CHECK(cache.model_texture_level(model, 0) == 4u);
        CHECK(width() == 16);
        const std::size_t far_bytes = cache.residency().resident_bytes;
        run(2.0f, 6);
        CHECK(cache.model_texture_level(model, 0) == 0u);
        CHECK(width() == 256);
        CHECK(cache.residency().resident_bytes > far_bytes + 200000); // the 256 px chain is accounted to the model
        run(25.0f, 6); // a ~0.7 m-radius quad: ~36 px wants the 64 px level
        CHECK(cache.model_texture_level(model, 0) == 2u);
        CHECK(width() == 64);
        const std::uint64_t restreams = cache.residency().restreams;
        CHECK(restreams == 3);
        // Evicted and seen again far away: the reload uploads the far level directly.
        run(200.0f, 4);
        cache.evict(model);
        run(200.0f, 4);
        CHECK(cache.model_texture_level(model, 0) == 4u);
        CHECK(width() == 16);
        CHECK(cache.residency().restreams == restreams + 1); // only the drop before the eviction
        CHECK_FALSE(cache.model_texture_level(Uuid::generate(), 0).has_value());
    }
}
