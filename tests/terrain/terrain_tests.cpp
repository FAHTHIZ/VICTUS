// SPDX-License-Identifier: Apache-2.0
// Heightmap terrain (ADR-0063): .vterrain files, generation, chunk meshes with LODs, the
// TerrainSystem (incremental rebuilds) and height-field collision.
#include "victus/core/math.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/terrain/terrain.hpp"

#include <doctest.h>

#include <cmath>
#include <filesystem>

using namespace victus;
using namespace victus::terrain;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

Heightfield hills(std::uint32_t res = 65, std::uint64_t seed = 7) {
    auto hf = make_flat(res);
    REQUIRE(hf.ok());
    GenerateSettings g;
    g.seed = seed;
    generate(*hf, g);
    auto_paint(*hf, {64, 16, 64});
    return std::move(*hf);
}

std::unique_ptr<physics::PhysicsWorld> make_world() {
    auto w = physics::PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

} // namespace

TEST_CASE("terrain: resolutions, flat fields and weight packing") {
    CHECK(valid_resolution(33));
    CHECK(valid_resolution(129));
    CHECK(valid_resolution(4097));
    CHECK_FALSE(valid_resolution(17));
    CHECK_FALSE(valid_resolution(64));
    CHECK_FALSE(valid_resolution(8193));
    CHECK_FALSE(make_flat(100).ok());
    auto flat = make_flat(33, 0.25f);
    REQUIRE(flat.ok());
    CHECK(flat->heights.size() == 33u * 33u);
    CHECK(flat->chunks_per_side() == 1);
    CHECK(flat->sample(0.5f, 0.5f) == doctest::Approx(0.25f));
    CHECK(flat->weights(0.3f, 0.7f).x == doctest::Approx(1.0f));

    const math::Vec4 w = unpack_weights(pack_weights({0.5f, 0.25f, 0.25f, 0.0f}));
    CHECK(w.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(w.y == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(w.x + w.y + w.z + w.w == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(unpack_weights(pack_weights({0, 0, 0, 0})).x == doctest::Approx(1.0f)); // degenerate: layer 0
    CHECK(unpack_weights(pack_weights({NAN, 1, 0, 0})).y == doctest::Approx(1.0f));
}

TEST_CASE("terrain: .vterrain round trip and corrupt input rejection") {
    const Heightfield hf = hills();
    const auto bytes = serialize(hf);
    CHECK(bytes.size() == 16 + 65u * 65u * 6 + 8);
    auto back = deserialize(bytes);
    REQUIRE(back.ok());
    CHECK(back->resolution == 65);
    CHECK(back->splat == hf.splat);
    for (std::size_t i = 0; i < hf.heights.size(); ++i) REQUIRE(back->heights[i] == doctest::Approx(hf.heights[i]).epsilon(1e-4));
    CHECK(back->chunk_revisions.size() == 4);

    auto bad = bytes;
    bad[20] ^= 0x40; // payload byte: checksum mismatch
    CHECK_FALSE(deserialize(bad).ok());
    CHECK_FALSE(deserialize(std::span(bytes).first(bytes.size() - 1)).ok());
    CHECK_FALSE(deserialize({}).ok());
    auto magic = bytes;
    magic[0] = 'X';
    CHECK_FALSE(deserialize(magic).ok());

    const auto dir = std::filesystem::temp_directory_path() / "victus_terrain_test";
    std::filesystem::create_directories(dir);
    REQUIRE(save_file(dir / "t.vterrain", hf).ok());
    auto loaded = load_file(dir / "t.vterrain");
    REQUIRE(loaded.ok());
    CHECK(loaded->splat == hf.splat);
    CHECK_FALSE(load_file(dir / "missing.vterrain").ok());
    std::filesystem::remove_all(dir);
}

TEST_CASE("terrain: generation is deterministic and auto paint follows slope and height") {
    const Heightfield a = hills(65, 3), b = hills(65, 3), c = hills(65, 4);
    CHECK(a.heights == b.heights);
    CHECK(a.splat == b.splat);
    CHECK(a.heights != c.heights);
    float lo = 1, hi = 0;
    for (float h : a.heights) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    CHECK(lo >= 0.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi - lo > 0.1f);
    // A flat field paints layer 0 in the middle band, layer 2 below `low` and layer 3 above `high`.
    auto flat = make_flat(33, 0.4f);
    REQUIRE(flat.ok());
    auto_paint(*flat, {64, 16, 64});
    CHECK(unpack_weights(flat->splat[500]).x > 0.95f);
    auto low = make_flat(33, 0.0f);
    auto_paint(*low, {64, 16, 64});
    CHECK(unpack_weights(low->splat[500]).z > 0.95f);
    auto high = make_flat(33, 1.0f);
    auto_paint(*high, {64, 16, 64});
    CHECK(unpack_weights(high->splat[500]).w > 0.95f);
}

TEST_CASE("terrain: chunk meshes have skirts and monotonic LOD errors") {
    const Heightfield hf = hills();
    const math::Vec3 size{64, 16, 64};
    auto mesh = build_chunk(hf, size, 1, 0);
    REQUIRE(mesh.ok());
    const scene::ProceduralMesh& m = **mesh;
    CHECK(m.positions.size() == 33u * 33u + 4u * 33u);
    CHECK(m.normals.size() == m.positions.size());
    CHECK(m.colors.size() == m.positions.size());
    CHECK(m.lods.size() == 5); // strides 2, 4, 8, 16, 32
    float prev = 0.0f;
    std::size_t prev_count = m.indices.size();
    for (const auto& lod : m.lods) {
        CHECK(lod.error >= prev);
        CHECK(lod.indices.size() < prev_count);
        prev = lod.error;
        prev_count = lod.indices.size();
    }
    CHECK(m.lods.back().error > 0.0f);
    // Chunk (1, 0) starts at x = 0 (the terrain is centred) and every top vertex lies on the field.
    for (std::size_t i = 0; i < 33u * 33u; i += 37) {
        const math::Vec3 p = m.positions[i];
        CHECK(p.x >= -0.01f);
        const auto h = local_height(hf, size, p.x, p.z);
        REQUIRE(h);
        CHECK(*h == doctest::Approx(p.y).epsilon(0.001));
    }
    // Skirts hang below their edge vertices.
    CHECK(m.positions[33u * 33u].y < m.positions[0].y);
    // Up-facing triangles (counter-clockwise seen from +y).
    const math::Vec3 a = m.positions[m.indices[0]], b = m.positions[m.indices[1]], c = m.positions[m.indices[2]];
    CHECK(math::cross(b - a, c - a).y > 0.0f);
    CHECK_FALSE(build_chunk(hf, size, 2, 0).ok());

    auto flat = make_flat(33, 0.5f);
    auto fm = build_chunk(*flat, size, 0, 0);
    REQUIRE(fm.ok());
    for (const auto& lod : (*fm)->lods) CHECK(lod.error == doctest::Approx(0.0f));
    // Painted detail on flat ground still needs finer levels up close (splat-aware errors).
    for (std::uint32_t z = 0; z < 33; ++z)
        for (std::uint32_t x = 0; x < 16; ++x) flat->splat[flat->index(x, z)] = pack_weights({0, 1, 0, 0});
    auto painted = build_chunk(*flat, size, 0, 0);
    REQUIRE(painted.ok());
    CHECK((*painted)->lods.front().error > 0.0f);
    CHECK((*painted)->lods.back().error >= 16.0f); // a full weight swing over the 64 m coarse cell
    for (std::uint32_t z = 0; z < 33; ++z)
        for (std::uint32_t x = 0; x < 16; ++x) flat->splat[flat->index(x, z)] = pack_weights({1, 0, 0, 0});
    CHECK(sample_normal(*flat, size, 10, 10).y == doctest::Approx(1.0f));
    CHECK_FALSE(local_height(*flat, size, 40.0f, 0.0f));
    CHECK(*local_height(*flat, size, 0.0f, 0.0f) == doctest::Approx(8.0f));
}

TEST_CASE("terrain: touch marks only the chunks an edit reaches") {
    Heightfield hf = hills(129);
    REQUIRE(hf.chunks_per_side() == 4);
    hf.touch_all();
    const auto before = hf.chunk_revisions;
    hf.touch(10, 10, 12, 12); // well inside chunk (0, 0)
    for (std::size_t c = 0; c < before.size(); ++c) CHECK((hf.chunk_revisions[c] != before[c]) == (c == 0));
    const auto mid = hf.chunk_revisions;
    hf.touch(32, 70, 32, 70); // shared border column of chunks x 0/1, row 2
    for (std::size_t c = 0; c < mid.size(); ++c) CHECK((hf.chunk_revisions[c] != mid[c]) == (c == 8 || c == 9));
}

TEST_CASE("terrain: TerrainSystem builds chunks, rebuilds only dirty ones and keeps collision current") {
    auto shared = std::make_shared<Heightfield>(hills(129));
    const Uuid id = Uuid::generate();
    int provided = 0;
    TerrainSystem system([&](const Uuid& u) -> std::shared_ptr<Heightfield> {
        ++provided;
        return u == id ? shared : nullptr;
    });
    Scene scene;
    const Entity t = scene.create("Terrain");
    TerrainComponent comp;
    comp.data = id;
    comp.size = {128, 20, 128};
    scene.world().add<TerrainComponent>(t, comp);
    scene.update_transforms();
    system.update(scene);
    CHECK(provided == 1);
    CHECK(system.stats().terrains == 1);
    CHECK(system.stats().chunks == 16);
    CHECK(system.stats().chunk_rebuilds == 16);
    CHECK(system.stats().collision_rebuilds == 1);
    const auto* rt = scene.world().get<TerrainRuntime>(t);
    REQUIRE(rt);
    CHECK(rt->chunks.size() == 16);
    const auto* shape = scene.world().get<physics::StaticHeightfieldComponent>(t);
    REQUIRE(shape);
    CHECK(shape->resolution == 129);
    CHECK(shape->spacing.x == doctest::Approx(1.0f));

    system.update(scene); // nothing changed
    CHECK(system.stats().chunk_rebuilds == 16);
    CHECK(system.stats().collision_rebuilds == 1);

    shared->heights[shared->index(5, 5)] = 1.0f;
    shared->touch(5, 5, 5, 5);
    system.update(scene);
    CHECK(system.stats().chunk_rebuilds == 17);
    // The collider is updated in place: only the touched chunk's samples, as a dirty rectangle.
    CHECK(system.stats().collision_rebuilds == 1);
    CHECK(system.stats().collision_updates == 1);
    shape = scene.world().get<physics::StaticHeightfieldComponent>(t);
    REQUIRE(shape);
    CHECK(shape->height_revision == 1);
    CHECK(shape->dirty.x0 == 0);
    CHECK(shape->dirty.x1 == 32); // chunk 0 spans samples 0..32
    CHECK(shape->dirty.z1 == 32);
    CHECK((*shape->heights)[shared->index(5, 5)] == doctest::Approx(20.0f));
    CHECK(shape->height_range.y == doctest::Approx(20.0f));

    // Changing the size rebuilds everything; disabling collision drops the collider.
    scene.world().get<TerrainComponent>(t)->size = {64, 20, 64};
    system.update(scene);
    CHECK(system.stats().chunk_rebuilds == 33);
    scene.world().get<TerrainComponent>(t)->collision = false;
    system.update(scene);
    CHECK_FALSE(scene.world().get<physics::StaticHeightfieldComponent>(t));

    // Missing data clears the runtime.
    scene.world().get<TerrainComponent>(t)->data = Uuid::generate();
    system.update(scene);
    CHECK(system.stats().missing_data == 1);
    CHECK(scene.world().get<TerrainRuntime>(t)->chunks.empty());
}

TEST_CASE("terrain: height queries, raycasts and resting bodies follow the transform") {
    auto shared = std::make_shared<Heightfield>(hills(129, 11));
    const Uuid id = Uuid::generate();
    TerrainSystem system;
    system.add_data(id, shared);
    Scene scene;
    const Entity t = scene.create("Terrain");
    scene.world().get<Transform>(t)->position = {10, -2, -5};
    TerrainComponent comp;
    comp.data = id;
    comp.size = {128, 20, 128};
    scene.world().add<TerrainComponent>(t, comp);

    auto w = make_world();
    gameplay::Simulation sim;
    add_terrain_systems(sim, system);
    physics::add_physics_systems(sim, *w);
    const Entity box = scene.create("Box");
    scene.world().get<Transform>(box)->position = {12, 30, -3};
    scene.world().add<physics::RigidBodyComponent>(box);
    scene.world().add<physics::ColliderComponent>(box).size = {1, 1, 1};
    for (int i = 0; i < 240; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(w->has_body(t));

    const auto ground = height_at(scene, t, 12, -3);
    REQUIRE(ground);
    const auto local = local_height(*shared, comp.size, 2, 2);
    REQUIRE(local);
    CHECK(*ground == doctest::Approx(*local - 2.0f).epsilon(0.001));
    CHECK(height_at(scene, 12, -3).value_or(-1e9f) == doctest::Approx(*ground));
    CHECK_FALSE(height_at(scene, t, 200, 0));

    auto hit = w->raycast({12, 50, -3}, {0, -1, 0}, 100, physics::kAllLayers, box);
    REQUIRE(hit);
    CHECK(hit->entity == t);
    CHECK(hit->point.y == doctest::Approx(*ground).epsilon(0.02));
    CHECK(hit->normal.y > 0.3f);
    // The box settled on the surface (within its half extent plus slope tolerance).
    const float y = scene.world().get<Transform>(box)->position.y;
    MESSAGE("box y " << y << " ground " << *ground);
    CHECK(y > *ground - 0.1f);
    CHECK(y < *ground + 1.5f);

    // Moving the terrain moves its collider.
    scene.world().get<Transform>(t)->position.y = 3;
    for (int i = 0; i < 2; ++i) sim.tick(scene, 1.0f / 60);
    auto hit2 = w->raycast({12, 50, -3}, {0, -1, 0}, 100, physics::kAllLayers, box);
    REQUIRE(hit2);
    CHECK(hit2->point.y == doctest::Approx(*ground + 5.0f).epsilon(0.02));

    // Editing the heightfield updates the existing collider in place (no rebuild): a raised
    // patch under (12, -3) (local (2, 2) = sample (66, 66)) is what rays and bodies now meet.
    const auto builds = w->stats().heightfield_builds;
    CHECK(builds == 1);
    for (std::uint32_t z = 63; z <= 69; ++z)
        for (std::uint32_t x = 63; x <= 69; ++x) shared->heights[shared->index(x, z)] = 0.9f;
    shared->touch(63, 63, 69, 69);
    const float raised = 3.0f + 0.9f * 20.0f; // terrain at y = 3, 0.9 of 20 m
    const Entity crate = scene.create("Crate"); // dropped onto the patch after the edit
    scene.world().get<Transform>(crate)->position = {12, raised + 3.0f, -3};
    scene.world().add<physics::RigidBodyComponent>(crate);
    scene.world().add<physics::ColliderComponent>(crate).size = {1, 1, 1};
    for (int i = 0; i < 120; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(w->stats().heightfield_builds == builds);
    CHECK(w->stats().heightfield_updates == 1);
    CHECK(system.stats().collision_rebuilds == 1);
    CHECK(system.stats().collision_updates == 1);
    CHECK(scene.world().get<physics::StaticHeightfieldComponent>(t)->dirty.empty()); // consumed
    auto hit3 = w->raycast({12, 50, -3}, {0, -1, 0}, 100, physics::kAllLayers, crate);
    REQUIRE(hit3);
    CHECK(hit3->point.y == doctest::Approx(raised).epsilon(0.01));
    const float box_y = scene.world().get<Transform>(crate)->position.y;
    MESSAGE("crate y on the raised patch " << box_y << ", surface " << raised);
    CHECK(box_y > raised - 0.1f);
    CHECK(box_y < raised + 1.5f);
    // Away from the edit the old surface is unchanged.
    auto far = w->raycast({60, 50, 40}, {0, -1, 0}, 100, physics::kAllLayers, box);
    REQUIRE(far);
    const auto far_local = local_height(*shared, comp.size, 50, 45);
    REQUIRE(far_local);
    CHECK(far->point.y == doctest::Approx(*far_local + 3.0f).epsilon(0.02));
}

TEST_CASE("terrain: brushes raise, lower, smooth, flatten and paint inside their radius") {
    const math::Vec3 size{64, 16, 64}; // 1 m between samples at 65
    auto flat = make_flat(65, 0.5f);
    REQUIRE(flat.ok());
    Heightfield hf = std::move(*flat);
    hf.touch_all();
    CHECK(brush_weight({.radius = 4, .falloff = 0.5f}, 0.0f) == doctest::Approx(1.0f));
    CHECK(brush_weight({.radius = 4, .falloff = 0.5f}, 1.9f) == doctest::Approx(1.0f));
    CHECK(brush_weight({.radius = 4, .falloff = 0.5f}, 3.0f) == doctest::Approx(0.5f));
    CHECK(brush_weight({.radius = 4, .falloff = 0.5f}, 4.0f) == doctest::Approx(0.0f));
    CHECK(brush_weight({.radius = 4, .falloff = 0.0f}, 3.9f) == doctest::Approx(1.0f));

    Brush raise{.mode = BrushMode::Raise, .radius = 4, .strength = 1, .falloff = 0.5f};
    const std::uint64_t rev = hf.revision;
    const SampleRect r = apply_brush(hf, size, raise, 0, 0, 1.0f); // centre sample (32, 32)
    REQUIRE_FALSE(r.empty());
    CHECK(hf.revision > rev);
    CHECK(r.x0 >= 28);
    CHECK(r.x1 <= 36);
    CHECK(hf.heights[hf.index(32, 32)] == doctest::Approx(0.5f + 2.0f / 16.0f)); // radius / 2 metres
    CHECK(hf.heights[hf.index(20, 32)] == doctest::Approx(0.5f));
    CHECK(hf.heights[hf.index(35, 32)] > 0.5f);
    CHECK(hf.heights[hf.index(35, 32)] < hf.heights[hf.index(32, 32)]);
    for (int i = 0; i < 100; ++i) (void)apply_brush(hf, size, raise, 0, 0, 1.0f);
    CHECK(hf.heights[hf.index(32, 32)] == doctest::Approx(1.0f)); // clamped
    CHECK(apply_brush(hf, size, raise, 0, 0, 1.0f).count() < r.count()); // only the rim still moves

    Brush lower = raise;
    lower.mode = BrushMode::Lower;
    (void)apply_brush(hf, size, lower, 10, 10, 1.0f);
    CHECK(hf.heights[hf.index(42, 42)] < 0.5f);

    // Smoothing pulls a spike towards its neighbours; flatten moves towards the target.
    auto spike = make_flat(65, 0.2f);
    spike->heights[spike->index(32, 32)] = 0.8f;
    (void)apply_brush(*spike, size, {.mode = BrushMode::Smooth, .radius = 2, .strength = 1}, 0, 0, 1.0f);
    CHECK(spike->heights[spike->index(32, 32)] < 0.4f);
    CHECK(spike->heights[spike->index(31, 32)] > 0.2f);
    (void)apply_brush(*spike, size, {.mode = BrushMode::Flatten, .radius = 3, .strength = 1, .target = 0.6f}, 0, 0, 1.0f);
    CHECK(spike->heights[spike->index(32, 32)] == doctest::Approx(0.6f));
    CHECK(spike->heights[spike->index(33, 32)] == doctest::Approx(0.6f));

    // Painting converges on the layer; untouched samples keep theirs.
    Brush paint{.mode = BrushMode::Paint, .radius = 3, .strength = 0.2f, .layer = 2};
    for (int i = 0; i < 40; ++i) (void)apply_brush(hf, size, paint, -20, -20, 0.25f);
    CHECK(unpack_weights(hf.splat[hf.index(12, 12)]).z > 0.95f);
    CHECK(unpack_weights(hf.splat[hf.index(32, 32)]).x == doctest::Approx(1.0f));
    CHECK(apply_brush(hf, size, {.mode = BrushMode::Paint, .layer = 7}, 0, 0, 1.0f).empty());

    // Degenerate input changes nothing.
    CHECK(apply_brush(hf, size, raise, 500, 0, 1.0f).empty());
    CHECK(apply_brush(hf, size, raise, NAN, 0, 1.0f).empty());
    CHECK(apply_brush(hf, size, raise, 0, 0, 0.0f).empty());
    CHECK(brush_mode_name(BrushMode::Flatten) == "Flatten");
}

TEST_CASE("terrain: regions restore edits exactly; rays hit the surface") {
    Heightfield hf = hills(65, 5);
    const math::Vec3 size{64, 16, 64};
    const HeightfieldRegion before = copy_region(hf, {-5, 10, 20, 30});
    CHECK(before.rect == SampleRect{0, 10, 20, 30});
    CHECK(before.heights.size() == 21u * 21u);
    const auto original = hf.heights;
    const auto splat = hf.splat;
    (void)apply_brush(hf, size, {.mode = BrushMode::Raise, .radius = 5, .strength = 1}, -22, -12, 1.0f);
    (void)apply_brush(hf, size, {.mode = BrushMode::Paint, .radius = 5, .strength = 1, .layer = 3}, -22, -12, 1.0f);
    CHECK(hf.heights != original);
    const std::uint64_t rev = hf.revision;
    paste_region(hf, before);
    CHECK(hf.revision > rev);
    CHECK(hf.heights == original);
    CHECK(hf.splat == splat);
    HeightfieldRegion bad = before;
    bad.heights.pop_back();
    paste_region(hf, bad); // mismatched sizes are ignored
    CHECK(hf.heights == original);

    // Straight down onto the field, and a slanted ray.
    const auto down = raycast_local(hf, size, {3.3f, 40, -7.1f}, {0, -1, 0}, 100);
    REQUIRE(down);
    CHECK(down->y == doctest::Approx(*local_height(hf, size, 3.3f, -7.1f)).epsilon(0.001));
    const auto slant = raycast_local(hf, size, {-40, 30, 0}, {1, -0.5f, 0.2f}, 200);
    REQUIRE(slant);
    CHECK(slant->y == doctest::Approx(*local_height(hf, size, slant->x, slant->z)).epsilon(0.01));
    CHECK_FALSE(raycast_local(hf, size, {0, 40, 0}, {0, 1, 0}, 100)); // pointing away
    CHECK_FALSE(raycast_local(hf, size, {0, 40, 0}, {0, -1, 0}, 5));  // too short
    CHECK_FALSE(raycast_local(hf, size, {100, 40, 0}, {0, -1, 0}, 100)); // outside

    // World rays through a placed, rotated terrain.
    auto shared = std::make_shared<Heightfield>(hf);
    TerrainSystem system;
    const Uuid id = Uuid::generate();
    system.add_data(id, shared);
    Scene scene;
    const Entity t = scene.create("Terrain");
    scene.world().get<Transform>(t)->position = {100, 5, 0};
    scene.world().get<Transform>(t)->rotation = math::Quat::axis_angle({0, 1, 0}, math::radians(90));
    scene.world().get<Transform>(t)->scale = {3, 3, 3}; // ignored
    TerrainComponent comp;
    comp.data = id;
    comp.size = size;
    comp.collision = false;
    scene.world().add<TerrainComponent>(t, comp);
    scene.update_transforms();
    system.update(scene);
    const auto hit = raycast(scene, {104, 60, 2}, {0, -1, 0}, 200);
    REQUIRE(hit);
    CHECK(hit->entity == t);
    CHECK(hit->point.y == doctest::Approx(*height_at(scene, t, 104, 2)).epsilon(0.01));
    CHECK(hit->normal.y > 0.2f);
    CHECK(hit->distance == doctest::Approx(60.0f - hit->point.y).epsilon(0.001));
    CHECK_FALSE(raycast(scene, {0, 60, 0}, {0, -1, 0}, 200));
}

TEST_CASE("terrain: foliage scatter is deterministic and follows layers, slope and height") {
    const math::Vec3 size{64, 16, 64};
    auto flat = make_flat(65, 0.5f);
    REQUIRE(flat.ok());
    Heightfield hf = std::move(*flat);
    hf.touch_all();
    FoliageComponent f;
    f.layer = -1;
    f.density = 1.0f;
    const float spacing = foliage_spacing(size, f);
    CHECK(spacing == doctest::Approx(1.0f));
    std::size_t total = 0;
    for (std::uint32_t cz = 0; cz < 2; ++cz)
        for (std::uint32_t cx = 0; cx < 2; ++cx) total += scatter_chunk(hf, size, f, spacing, cx, cz).size();
    CHECK(total == 64u * 64u); // one per grid cell, each in exactly one chunk
    const auto a = scatter_chunk(hf, size, f, spacing, 1, 0);
    const auto b = scatter_chunk(hf, size, f, spacing, 1, 0);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i].transform_point({}) == b[i].transform_point({}));
    for (const auto& m : a) {
        const math::Vec3 p = m.transform_point({});
        CHECK(p.x >= -0.001f);
        CHECK(p.z < 0.001f);
        CHECK(p.y == doctest::Approx(8.0f));
        const float s = math::length(m.transform_vector({1, 0, 0}));
        CHECK(s >= f.min_scale - 1e-4f);
        CHECK(s <= f.max_scale + 1e-4f);
        CHECK(m.transform_vector({0, 1, 0}).y == doctest::Approx(s)); // upright
    }
    FoliageComponent other = f;
    other.seed = 2;
    CHECK(scatter_chunk(hf, size, other, spacing, 1, 0)[0].transform_point({}) != a[0].transform_point({}));

    // Layer-driven: none on layer 0 ground, all where layer 2 is painted at full weight.
    f.layer = 2;
    CHECK(scatter_chunk(hf, size, f, spacing, 0, 0).empty());
    for (std::uint32_t z = 0; z <= 32; ++z)
        for (std::uint32_t x = 0; x <= 32; ++x) hf.splat[hf.index(x, z)] = pack_weights({0, 0, 1, 0});
    CHECK(scatter_chunk(hf, size, f, spacing, 0, 0).size() > 32u * 32u * 9u / 10u);
    f.min_weight = 1.01f; // above every weight
    CHECK(scatter_chunk(hf, size, f, spacing, 0, 0).empty());
    f.min_weight = 0.3f;

    // Height range and slope filters.
    f.layer = -1;
    f.min_height = 0.6f;
    CHECK(scatter_chunk(hf, size, f, spacing, 0, 0).empty());
    f.min_height = 0.0f;
    Heightfield steep = hf;
    for (std::uint32_t z = 0; z < 65; ++z)
        for (std::uint32_t x = 0; x < 65; ++x) steep.heights[steep.index(x, z)] = static_cast<float>(x) / 64.0f * 1.0f;
    // 16 m rise over 64 m: ~14 degrees.
    f.max_slope = 10.0f;
    CHECK(scatter_chunk(steep, size, f, spacing, 0, 0).empty());
    f.max_slope = 20.0f;
    f.align_to_normal = 1.0f;
    const auto tilted = scatter_chunk(steep, size, f, spacing, 0, 0);
    REQUIRE_FALSE(tilted.empty());
    const math::Vec3 up = math::normalize(tilted[0].transform_vector({0, 1, 0}));
    CHECK(up.x < -0.2f); // leans with the slope (surface rises along +x)
    CHECK(local_normal(steep, size, 0, 0).x == doctest::Approx(up.x).epsilon(0.01));

    // The instance budget coarsens the grid; degenerate settings place nothing.
    f.max_instances = 100;
    CHECK(foliage_spacing(size, f) == doctest::Approx(6.4f));
    f.density = 0;
    CHECK(foliage_spacing(size, f) == 0.0f);
    f.density = NAN;
    CHECK(foliage_spacing(size, f) == 0.0f);
    CHECK(scatter_chunk(hf, size, f, 0.0f, 0, 0).empty());
    CHECK(scatter_chunk(hf, size, FoliageComponent{}, 1.0f, 5, 0).empty());
}

TEST_CASE("terrain: the system scatters foliage for child entities and rebuilds only edited chunks") {
    register_components();
    auto data = std::make_shared<Heightfield>(hills(129, 3));
    TerrainSystem system;
    const Uuid id = Uuid::generate();
    system.add_data(id, data);
    Scene scene;
    const Entity t = scene.create("Terrain");
    TerrainComponent comp;
    comp.data = id;
    comp.size = {128, 20, 128};
    comp.collision = false;
    scene.world().add<TerrainComponent>(t, comp);
    const Entity grass = scene.create("Grass", t);
    FoliageComponent f;
    f.layer = -1;
    f.density = 0.25f;
    f.max_slope = 90.0f;
    scene.world().add<FoliageComponent>(grass, f);
    const Entity orphan = scene.create("Orphan");
    scene.world().add<FoliageComponent>(orphan, f);
    scene.update_transforms();
    system.update(scene);
    const auto* rt = scene.world().get<FoliageRuntime>(grass);
    REQUIRE(rt);
    CHECK(rt->terrain == t);
    CHECK(rt->chunks.size() == 16u);
    CHECK(rt->instances == 64u * 64u);
    CHECK(system.stats().foliage == 2);
    CHECK(system.stats().foliage_instances == 64u * 64u);
    CHECK(system.stats().foliage_rebuilds == 16u);
    CHECK(scene.world().get<FoliageRuntime>(orphan)->instances == 0);

    // Unchanged: nothing rebuilt. One dab in a chunk interior: only that chunk.
    system.update(scene);
    CHECK(system.stats().foliage_rebuilds == 16u);
    (void)apply_brush(*data, comp.size, {.mode = BrushMode::Raise, .radius = 3, .strength = 1}, -48, -48, 1.0f);
    system.update(scene);
    CHECK(system.stats().foliage_rebuilds == 17u);
    // Changing a setting rebuilds everything and changes the count.
    scene.world().get<FoliageComponent>(grass)->density = 1.0f;
    system.update(scene);
    CHECK(system.stats().foliage_rebuilds == 33u);
    CHECK(scene.world().get<FoliageRuntime>(grass)->instances == 128u * 128u);
    // Instances sit on the surface (terrain local space).
    for (const auto& m : scene.world().get<FoliageRuntime>(grass)->chunks[5]) {
        const math::Vec3 p = m.transform_point({});
        CHECK(p.y == doctest::Approx(*local_height(*data, comp.size, p.x, p.z)).epsilon(0.001));
    }

    // Reflection round trip.
    const auto* ser = scene::SerializerRegistry::instance().by_name("Foliage");
    REQUIRE(ser);
    FoliageComponent edited = f;
    edited.layer = 3;
    edited.draw_distance = 42;
    const json::Value saved = ser->save(&edited);
    FoliageComponent back;
    REQUIRE(ser->load(&back, saved).ok());
    CHECK(back.layer == 3);
    CHECK(back.draw_distance == doctest::Approx(42.0f));
}
