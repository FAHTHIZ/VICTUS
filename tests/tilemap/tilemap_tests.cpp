// SPDX-License-Identifier: Apache-2.0
// Tile sets (.vtileset), Tilemap editing, autotiling, collision merging and physics (Slice 42).
#include "victus/core/json.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/tilemap/tilemap.hpp"

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <format>

using namespace victus;
using namespace victus::tilemap;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

Result<Tileset> parse_text(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE(doc.ok());
    return parse_tileset(*doc);
}

std::shared_ptr<Tileset> blocks() {
    auto t = std::make_shared<Tileset>();
    t->texture = Uuid::generate();
    t->columns = 4;
    t->rows = 5;
    t->default_collision = TileCollision::Solid;
    t->collision[17] = TileCollision::OneWay;
    t->collision[18] = TileCollision::None; // decoration
    Terrain ground;
    ground.name = "ground";
    for (int i = 0; i < 16; ++i) ground.tiles[static_cast<std::size_t>(i)] = i; // tile index == neighbour mask
    t->terrains.push_back(ground);
    return t;
}

std::unique_ptr<physics::PhysicsWorld> make_world() {
    auto w = physics::PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

Entity make_map(Scene& scene, const Uuid& tileset) {
    register_components();
    const Entity e = scene.create("Level");
    TilemapComponent m;
    m.tileset = tileset;
    scene.world().add<TilemapComponent>(e, m);
    return e;
}

} // namespace

TEST_CASE("tile set: parse, validation and round trip") {
    const Uuid tex = Uuid::generate();
    auto t = parse_text(std::format(R"({{"format":"victus.tileset","version":1,"texture":"{}","columns":8,"rows":4,
        "collision":"solid","tiles":{{"9":{{"collision":"one_way"}},"31":{{"collision":"none"}}}},
        "terrains":{{"grass":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,-1]}}}})",
                                    tex.to_string()));
    REQUIRE(t.ok());
    CHECK(t->texture == tex);
    CHECK(t->tile_count() == 32);
    CHECK(t->collision_of(0) == TileCollision::Solid);
    CHECK(t->collision_of(9) == TileCollision::OneWay);
    CHECK(t->collision_of(31) == TileCollision::None);
    CHECK(t->collision_of(-1) == TileCollision::None);
    CHECK(t->terrain_index("grass") == 0);
    CHECK(t->terrain_index("sand") == -1);
    CHECK(t->terrains[0].tiles[15] == -1);
    const math::Vec4 uv = t->tile_uv(9); // column 1, row 1
    CHECK(uv.x == doctest::Approx(0.125f));
    CHECK(uv.y == doctest::Approx(0.25f));
    CHECK(uv.z == doctest::Approx(0.25f));
    CHECK(uv.w == doctest::Approx(0.5f));

    auto again = parse_tileset(to_json(*t));
    REQUIRE(again.ok());
    CHECK(again->columns == 8);
    CHECK(again->collision_of(9) == TileCollision::OneWay);
    CHECK(again->collision_of(31) == TileCollision::None);
    CHECK(again->terrains[0].tiles == t->terrains[0].tiles);

    const auto fails = [](std::string_view text) { return !parse_text(text).ok(); };
    CHECK(fails(R"({"format":"victus.sheet","version":1,"columns":1,"rows":1})"));
    CHECK(fails(R"({"format":"victus.tileset","version":2,"columns":1,"rows":1})"));
    CHECK(parse_text(R"({"format":"victus.tileset","version":2,"columns":1,"rows":1})").error().code == ErrorCode::VersionMismatch);
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":0,"rows":1})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":1.5,"rows":1})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":300,"rows":300})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"collision":"lava"})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"tiles":{"2":{"collision":"solid"}}})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"tiles":{"x":{}}})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"terrains":{"a":[0,1]}})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"terrains":{"a":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9]}})"));
    CHECK(fails(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1,"texture":"nope"})"));
    CHECK(parse_text(R"({"format":"victus.tileset","version":1,"columns":2,"rows":1})").ok());

    const auto dir = std::filesystem::temp_directory_path() / "victus_tileset_test";
    std::filesystem::create_directories(dir);
    REQUIRE(save_tileset_file(dir / "a.vtileset", *t).ok());
    auto loaded = load_tileset_file(dir / "a.vtileset");
    REQUIRE(loaded.ok());
    CHECK(loaded->texture == tex);
    std::filesystem::remove_all(dir);
}

TEST_CASE("tilemap: set, fill, clear, bounds and revisions") {
    TilemapComponent m;
    CHECK(bounds(m).empty());
    const std::uint64_t r0 = m.revision;
    CHECK(set_tile(m, 2, -3, 5, true, false));
    CHECK(m.revision > r0);
    CHECK(m.tile(2, -3) == 5);
    CHECK(m.cell(2, -3)->flip_x);
    CHECK(m.tile(0, 0) == -1);
    const std::uint64_t r1 = m.revision;
    CHECK(set_tile(m, 2, -3, 5, true, false)); // unchanged: no revision bump
    CHECK(m.revision == r1);
    CHECK_FALSE(set_tile(m, kMaxCoordinate + 1, 0, 1));
    CHECK(fill_tiles(m, 0, 0, 3, 1, 7) == 8);
    CHECK(fill_tiles(m, 3, 1, 0, 0, 7) == 0); // reversed corners, already painted
    CHECK(m.cells.size() == 9);
    TileBounds b = bounds(m);
    CHECK(b.min_x == 0);
    CHECK(b.min_y == -3);
    CHECK(b.max_x == 3);
    CHECK(b.max_y == 1);
    CHECK(fill_tiles(m, 0, 0, 3, 0, -1) == 4); // erase a row
    CHECK(m.cells.size() == 5);
    CHECK(fill_tiles(m, 0, 0, 2000, 2000, 1) == 0); // over kMaxFillCells
    clear_tiles(m);
    CHECK(m.cells.empty());
    CHECK(cell_key(-1, 2) != cell_key(2, -1));
    CHECK(key_x(cell_key(-7, 9)) == -7);
    CHECK(key_y(cell_key(-7, 9)) == 9);
}

TEST_CASE("tilemap: terrain painting picks tiles from neighbour masks") {
    const auto ts = blocks();
    TilemapComponent m;
    CHECK(paint_terrain(m, *ts, 0, 0, 0));
    CHECK(m.tile(0, 0) == 0); // isolated
    paint_terrain(m, *ts, 1, 0, 0);
    CHECK(m.tile(0, 0) == 2); // east neighbour
    CHECK(m.tile(1, 0) == 8); // west neighbour
    paint_terrain(m, *ts, 1, 1, 0);
    CHECK(m.tile(1, 0) == (8 | 1));
    CHECK(m.tile(1, 1) == 4); // south neighbour
    paint_terrain(m, *ts, 2, 0, 0);
    paint_terrain(m, *ts, 1, -1, 0);
    CHECK(m.tile(1, 0) == 15); // surrounded
    // Erasing re-picks the neighbours.
    paint_terrain(m, *ts, 1, 0, -1);
    CHECK(m.tile(1, 0) == -1);
    CHECK(m.tile(0, 0) == 0);
    CHECK(m.tile(1, 1) == 0);
    CHECK_FALSE(paint_terrain(m, *ts, 0, 0, 3)); // unknown terrain
    // A plain tile is not part of the terrain.
    set_tile(m, 3, 0, 16);
    paint_terrain(m, *ts, 4, 0, 0);
    CHECK(m.tile(4, 0) == 0);
    CHECK(m.cell(3, 0)->terrain == -1);
    // refresh_terrain re-applies changed rules.
    auto changed = std::make_shared<Tileset>(*ts);
    changed->terrains[0].tiles[0] = 19;
    refresh_terrain(m, *changed);
    CHECK(m.tile(4, 0) == 19);
    CHECK(m.tile(3, 0) == 16);
}

TEST_CASE("tilemap: collision boxes merge runs and rows") {
    const auto ts = blocks();
    TilemapComponent m;
    m.cell_size = {0.5f, 0.5f};
    fill_tiles(m, 0, 0, 9, 2, 1);    // 10x3 solid block -> one box
    fill_tiles(m, 12, 0, 13, 0, 1);  // separate 2x1 run
    set_tile(m, 14, 0, 18);          // decoration: no collider
    fill_tiles(m, 0, 5, 3, 6, 17);   // one-way tiles: one box per row
    auto boxes = collision_boxes(m, *ts);
    REQUIRE(boxes.size() == 4);
    CHECK(boxes[0].min.x == doctest::Approx(0.0f));
    CHECK(boxes[0].min.y == doctest::Approx(0.0f));
    CHECK(boxes[0].max.x == doctest::Approx(5.0f));
    CHECK(boxes[0].max.y == doctest::Approx(1.5f));
    CHECK(boxes[0].type == TileCollision::Solid);
    CHECK(boxes[1].min.x == doctest::Approx(6.0f));
    CHECK(boxes[1].max.x == doctest::Approx(7.0f));
    CHECK(boxes[2].type == TileCollision::OneWay);
    CHECK(boxes[2].min.y == doctest::Approx(2.5f));
    CHECK(boxes[2].max.y == doctest::Approx(3.0f));
    CHECK(boxes[3].min.y == doctest::Approx(3.0f));
    // A notch splits the block into separate spans but keeps the total area.
    set_tile(m, 5, 1, -1);
    boxes = collision_boxes(m, *ts);
    float area = 0;
    for (const auto& b : boxes)
        if (b.type == TileCollision::Solid) area += (b.max.x - b.min.x) * (b.max.y - b.min.y);
    CHECK(area == doctest::Approx((30 - 1 + 2) * 0.25f));
}

TEST_CASE("tilemap: scene JSON keeps cells, flips and terrains") {
    register_components();
    const Uuid tsid = Uuid::generate();
    Scene scene;
    const Entity e = make_map(scene, tsid);
    auto* m = scene.world().get<TilemapComponent>(e);
    m->cell_size = {2, 1};
    m->layer = -3;
    m->collision = false;
    set_tile(*m, -4, 7, 3, true, true);
    set_tile(*m, 0, 0, 1);
    paint_terrain(*m, *blocks(), 5, 5, 0);
    const json::Value saved = scene.save();
    Scene copy;
    REQUIRE(copy.load(saved).ok());
    Entity c{};
    copy.world().each<TilemapComponent>([&](Entity ce, TilemapComponent&) { c = ce; });
    REQUIRE(copy.world().alive(c));
    const auto* cm = copy.world().get<TilemapComponent>(c);
    CHECK(cm->tileset == tsid);
    CHECK(cm->cell_size.x == doctest::Approx(2.0f));
    CHECK(cm->layer == -3);
    CHECK_FALSE(cm->collision);
    CHECK(cm->cells.size() == 3);
    CHECK(cm->tile(-4, 7) == 3);
    CHECK(cm->cell(-4, 7)->flip_x);
    CHECK(cm->cell(-4, 7)->flip_y);
    CHECK(cm->cell(5, 5)->terrain == 0);
    CHECK(cm->revision > 0);
    // Saving is deterministic regardless of hash order.
    CHECK(json::stringify(copy.save()) == json::stringify(saved));
    // Malformed cells are rejected.
    json::Value cell = json::Value::array();
    cell.push(1);
    cell.push("x");
    cell.push(2);
    json::Value cells = json::Value::array();
    cells.push(cell);
    json::Value bad = json::Value::object();
    bad.set("cells", cells);
    const auto* ser = scene::SerializerRegistry::instance().by_name("Tilemap");
    REQUIRE(ser);
    TilemapComponent target;
    CHECK_FALSE(ser->load(&target, bad).ok());
}

TEST_CASE("tilemap system: tile colliders follow edits and stop characters") {
    const auto ts = blocks();
    const Uuid tsid = Uuid::generate();
    Scene scene;
    const Entity map = make_map(scene, tsid);
    scene.world().get<Transform>(map)->position = {-10, -1, 0}; // top surface of row 0 at y = 0
    fill_tiles(*scene.world().get<TilemapComponent>(map), 0, 0, 19, 0, 1);
    int provided = 0;
    TilemapSystem system([&](const Uuid& id) -> std::shared_ptr<const Tileset> {
        ++provided;
        return id == tsid ? ts : nullptr;
    });
    auto w = make_world();
    gameplay::Simulation sim;
    add_tilemap_systems(sim, system);
    physics::add_physics_systems(sim, *w);
    const Entity player = scene.create("Player");
    scene.world().get<Transform>(player)->position = {0, 2, 0};
    scene.world().add<physics::CharacterController2DComponent>(player, physics::CharacterController2DComponent{});
    for (int i = 0; i < 90; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(provided == 1); // cached
    CHECK(system.stats().maps == 1);
    CHECK(system.stats().boxes == 1);
    REQUIRE(scene.world().get<TilemapRuntime>(map));
    CHECK(scene.world().get<TilemapRuntime>(map)->tileset == ts);
    CHECK(w->has_body(map));
    const auto st = w->character_state(player);
    CHECK(st.grounded);
    CHECK(st.ground == map);
    CHECK(scene.world().get<Transform>(player)->position.y == doctest::Approx(0.5f).epsilon(0.03));
    auto hit = w->raycast_2d({3, 5}, {0, -1}, 20, physics::kAllLayers, player);
    REQUIRE(hit);
    CHECK(hit->entity == map);
    CHECK(hit->point.y == doctest::Approx(0.0f).epsilon(0.01));

    // Digging a hole under the character drops it to a lower row.
    auto* m = scene.world().get<TilemapComponent>(map);
    fill_tiles(*m, 0, -3, 19, -3, 1);
    fill_tiles(*m, 8, 0, 12, 0, -1);
    const std::uint32_t rebuilds = system.stats().collision_rebuilds;
    for (int i = 0; i < 120; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(system.stats().collision_rebuilds == rebuilds + 1);
    CHECK(system.stats().boxes == 3);
    CHECK(scene.world().get<Transform>(player)->position.y == doctest::Approx(-2.5f).epsilon(0.03));

    // One-way tiles: the character jumps up through and lands on top.
    m = scene.world().get<TilemapComponent>(map);
    fill_tiles(*m, 8, -1, 12, -1, 17);
    for (int i = 0; i < 10; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(w->jump_character(player, std::sqrt(2 * 9.81f * 2.5f)));
    for (int i = 0; i < 150; ++i) sim.tick(scene, 1.0f / 60);
    CHECK(scene.world().get<Transform>(player)->position.y == doctest::Approx(-0.5f).epsilon(0.03));

    // Turning collision off removes the body; a missing tile set is reported.
    scene.world().get<TilemapComponent>(map)->collision = false;
    sim.tick(scene, 1.0f / 60);
    CHECK_FALSE(scene.world().get<physics::StaticShapes2DComponent>(map));
    sim.tick(scene, 1.0f / 60);
    CHECK_FALSE(w->has_body(map));
    scene.world().get<TilemapComponent>(map)->tileset = Uuid::generate();
    sim.tick(scene, 1.0f / 60);
    CHECK(system.stats().missing_tilesets == 1);
    CHECK_FALSE(scene.world().get<TilemapRuntime>(map)->tileset);
    // Destroying the map destroys its body.
    scene.world().get<TilemapComponent>(map)->tileset = tsid;
    scene.world().get<TilemapComponent>(map)->collision = true;
    sim.tick(scene, 1.0f / 60);
    sim.tick(scene, 1.0f / 60);
    CHECK(w->has_body(map));
    scene.destroy(map);
    sim.tick(scene, 1.0f / 60);
    CHECK_FALSE(w->has_body(map));
}

TEST_CASE("tilemap system: moving and scaling the map moves its colliders") {
    const auto ts = blocks();
    const Uuid tsid = Uuid::generate();
    Scene scene;
    const Entity map = make_map(scene, tsid);
    fill_tiles(*scene.world().get<TilemapComponent>(map), 0, 0, 3, 0, 1);
    TilemapSystem system;
    system.add_tileset(tsid, ts);
    auto w = make_world();
    system.update(scene);
    w->step(scene, 1.0f / 60);
    auto hit = w->raycast_2d({2, 5}, {0, -1}, 20);
    REQUIRE(hit);
    CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(0.01));
    scene.world().get<Transform>(map)->position = {0, 3, 0};
    scene.world().get<Transform>(map)->scale = {2, 2, 1};
    w->step(scene, 1.0f / 60);
    hit = w->raycast_2d({7, 10}, {0, -1}, 20);
    REQUIRE(hit);
    CHECK(hit->entity == map);
    CHECK(hit->point.y == doctest::Approx(5.0f).epsilon(0.01));
    CHECK_FALSE(w->raycast_2d({9, 10}, {0, -1}, 20)); // the map ends at x = 8
}
