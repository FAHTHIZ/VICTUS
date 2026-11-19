// SPDX-License-Identifier: Apache-2.0
// World streaming (6h, ADR-0090): descriptors, partitioning, cell streaming with hysteresis,
// async loads and cancellation, dependency gating, cell state persistence and save games.
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"
#include "victus/world/world.hpp"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <set>

using namespace victus;
using namespace victus::world;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

Entity add_prop(Scene& s, std::string_view name, math::Vec3 pos, Entity parent = scene::kNullEntity) {
    const Entity e = s.create(name, parent);
    s.world().get<Transform>(e)->position = pos;
    scene::MeshRendererComponent mr;
    mr.primitive = "cube";
    s.world().add<scene::MeshRendererComponent>(e, mr);
    return e;
}

// A world of `n` cells along +X, each cell_size wide with one prop ("prop_i") at its center.
struct LineWorld {
    TempDir dir;
    WorldDesc desc;
    std::vector<Uuid> props;
    LineWorld(const char* name, int n, float cell = 100.0f) : dir(name) {
        desc.name = "Line";
        desc.load_radius = 60.0f;
        desc.unload_margin = 20.0f;
        for (int i = 0; i < n; ++i) {
            Scene s;
            const Entity p = add_prop(s, "prop_" + std::to_string(i), {cell * static_cast<float>(i) + cell * 0.5f, 0, 0});
            add_prop(s, "child", {0, 1, 0}, p);
            props.push_back(s.uuid(p));
            CellDesc c;
            c.id = Uuid::generate();
            c.name = "cell_" + std::to_string(i);
            c.scene = "cells/" + c.name + ".vscene";
            c.bounds.min = {cell * static_cast<float>(i), -10, -cell * 0.5f};
            c.bounds.max = {cell * static_cast<float>(i + 1), 10, cell * 0.5f};
            std::filesystem::create_directories(dir.path / "cells");
            REQUIRE(s.save_file(dir.path / c.scene).ok());
            desc.cells.push_back(std::move(c));
        }
    }
};

std::vector<StreamingSource> at(float x) { return {StreamingSource{{x, 0, 0}, 1.0f}}; }

} // namespace

TEST_CASE("world descriptor round-trips and validates") {
    register_components();
    WorldDesc w;
    w.name = "Island";
    w.load_radius = 128;
    CellDesc c;
    c.id = Uuid::generate();
    c.name = "a";
    c.scene = "worlds/a.vscene";
    c.bounds.min = {0, 0, 0};
    c.bounds.max = {64, 8, 64};
    c.load_radius = 200;
    w.cells.push_back(c);
    auto back = parse_world(world_to_json(w));
    REQUIRE(back.ok());
    CHECK(back->name == "Island");
    CHECK(back->load_radius == doctest::Approx(128));
    REQUIRE(back->cells.size() == 1);
    CHECK(back->cells[0].id == c.id);
    CHECK(back->cells[0].scene == "worlds/a.vscene");
    CHECK(back->cells[0].bounds.max.z == doctest::Approx(64));
    CHECK(back->cells[0].load_radius == doctest::Approx(200));

    WorldDesc twice = w;
    twice.cells.push_back(c);
    json::Value dup = world_to_json(twice);
    CHECK_FALSE(parse_world(dup).ok());
    json::Value bad_format = world_to_json(w);
    bad_format.set("format", "victus.scene");
    CHECK_FALSE(parse_world(bad_format).ok());
    w.cells[0].bounds.min = {100, 0, 0};
    CHECK_FALSE(parse_world(world_to_json(w)).ok());

    TempDir dir("victus_world_desc");
    w.cells[0].bounds.min = {0, 0, 0};
    REQUIRE(save_world_file(w, dir.path / "a.vworld").ok());
    auto loaded = load_world_file(dir.path / "a.vworld");
    REQUIRE(loaded.ok());
    CHECK(loaded->cells.size() == 1);
}

TEST_CASE("Scene::load_additive adds entities, parents into the scene and rolls back") {
    Scene s;
    const Entity anchor = s.create("anchor");
    s.create("other");
    Scene src;
    const Entity a = src.create("a");
    src.create("b", a);
    json::Value doc = src.save();
    // Re-parent root "a" under the existing anchor.
    json::Value entities = doc["entities"];
    for (json::Value& ev : entities.as_array())
        if (ev["name"].as_string_or("") == "a") ev.set("parent", s.uuid(anchor).to_string());
    doc.set("entities", std::move(entities));
    std::vector<Entity> created;
    REQUIRE(s.load_additive(doc, &created).ok());
    CHECK(created.size() == 2);
    CHECK(s.size() == 4);
    const Entity added = s.find(src.uuid(a));
    REQUIRE(added != scene::kNullEntity);
    CHECK(s.parent(added) == anchor);

    // The same document again collides on every uuid: nothing is added.
    CHECK_FALSE(s.load_additive(doc).ok());
    CHECK(s.size() == 4);
    CHECK(s.find_by_name("anchor") == anchor);
}

TEST_CASE("streamer loads near cells, unloads with hysteresis and loads nearest first") {
    register_components();
    LineWorld w("victus_world_stream", 4);
    Scene s;
    s.create("persistent");
    WorldStreamer st(w.desc, w.dir.path);
    const auto& cells = w.desc.cells;

    st.update(s, at(50)); // inside cell 0; cell 1 starts at 100, 50 away (< 60)
    // Synchronous: both load, one activates per update.
    CHECK(st.state(cells[0].id) == CellState::Active);
    CHECK(st.state(cells[1].id) == CellState::Waiting);
    CHECK(st.state(cells[2].id) == CellState::Unloaded);
    st.update(s, at(50));
    CHECK(st.state(cells[1].id) == CellState::Active);
    CHECK(s.size() == 5);
    CHECK(s.find(w.props[0]) != scene::kNullEntity);
    CHECK(s.world().get<CellMember>(s.find(w.props[1]))->cell == cells[1].id);

    // Move to x=170: cell 0 is 70 away — beyond 60 but inside 60+20, so it stays.
    st.update(s, at(170));
    CHECK(st.state(cells[0].id) == CellState::Active);
    // x=185: 85 away — unloaded; cell 2 (15 away) loads.
    st.update(s, at(185));
    CHECK(st.state(cells[0].id) == CellState::Unloaded);
    CHECK(s.find(w.props[0]) == scene::kNullEntity);
    CHECK(st.state(cells[2].id) == CellState::Active);
    CHECK(st.stats().unloads == 1);

    // No sources: everything unloads; the persistent entity stays.
    st.update(s, {});
    CHECK(st.stats().active == 0);
    CHECK(s.size() == 1);

    // Nearest-first with one load slot: from x=290 cell 2 (dist 0, contains it) beats cell 3 (10).
    StreamerSettings one;
    one.max_loading = 1;
    Scene s2;
    WorldStreamer st2(w.desc, w.dir.path, nullptr, {}, one);
    st2.update(s2, at(290));
    CHECK(st2.state(cells[2].id) == CellState::Active);
    CHECK(st2.state(cells[3].id) == CellState::Unloaded);
    st2.update(s2, at(290));
    CHECK(st2.state(cells[3].id) == CellState::Active);
}

TEST_CASE("streamer keeps the state of unloaded cells and saves it") {
    register_components();
    LineWorld w("victus_world_state", 2);
    Scene s;
    WorldStreamer st(w.desc, w.dir.path);
    st.update(s, at(50));
    REQUIRE(st.state(w.desc.cells[0].id) == CellState::Active);
    const Entity p0 = s.find(w.props[0]);
    s.world().get<Transform>(p0)->position = {42, 7, 0};
    s.destroy(s.children(p0).front()); // the child is gone for good

    st.update(s, at(1000));
    CHECK(st.state(w.desc.cells[0].id) == CellState::Unloaded);
    st.update(s, at(50));
    REQUIRE(st.state(w.desc.cells[0].id) == CellState::Active);
    const Entity again = s.find(w.props[0]);
    REQUIRE(again != scene::kNullEntity);
    CHECK(s.world().get<Transform>(again)->position.x == doctest::Approx(42));
    CHECK(s.children(again).empty());

    // A save game carries the changed cell into a fresh session.
    const json::Value save = st.save_state(s);
    Scene fresh;
    WorldStreamer st2(w.desc, w.dir.path);
    REQUIRE(st2.load_state(save).ok());
    st2.update(fresh, at(50));
    const Entity restored = fresh.find(w.props[0]);
    REQUIRE(restored != scene::kNullEntity);
    CHECK(fresh.world().get<Transform>(restored)->position.y == doctest::Approx(7));
    CHECK(fresh.children(restored).empty());
    // Loading state into an active cell is refused.
    CHECK_FALSE(st2.load_state(save).ok());
}

TEST_CASE("streamer waits for cell dependencies and times out") {
    register_components();
    LineWorld w("victus_world_deps", 1);
    // Give the prop a mesh asset reference the fake cache tracks.
    const Uuid mesh = Uuid::generate();
    {
        Scene s;
        REQUIRE(s.load_file(w.dir.path / w.desc.cells[0].scene).ok());
        s.world().get<scene::MeshRendererComponent>(s.find(w.props[0]))->mesh_asset = mesh;
        REQUIRE(s.save_file(w.dir.path / w.desc.cells[0].scene).ok());
    }
    std::set<Uuid> requested;
    bool resident = false;
    Dependencies deps;
    deps.request = [&](const Uuid& id) { requested.insert(id); };
    deps.ready = [&](const Uuid& id) { return id != mesh || resident; };
    Scene s;
    StreamerSettings settings;
    settings.max_wait_updates = 3;
    WorldStreamer st(w.desc, w.dir.path, nullptr, deps, settings);
    st.update(s, at(50));
    CHECK(requested.contains(mesh));
    CHECK(st.state(w.desc.cells[0].id) == CellState::Waiting);
    st.update(s, at(50));
    CHECK(st.state(w.desc.cells[0].id) == CellState::Waiting);
    resident = true;
    st.update(s, at(50));
    CHECK(st.state(w.desc.cells[0].id) == CellState::Active);
    CHECK(st.stats().dependency_timeouts == 0);

    // Never resident: activates after max_wait_updates anyway.
    resident = false;
    Scene s2;
    WorldStreamer st2(w.desc, w.dir.path, nullptr, deps, settings);
    for (int i = 0; i < 6 && st2.state(w.desc.cells[0].id) != CellState::Active; ++i) st2.update(s2, at(50));
    CHECK(st2.state(w.desc.cells[0].id) == CellState::Active);
    CHECK(st2.stats().dependency_timeouts == 1);
}

TEST_CASE("streamer reads cells on jobs and cancels loads no source wants") {
    register_components();
    LineWorld w("victus_world_async", 3);
    jobs::JobSystem js(2);
    Scene s;
    WorldStreamer st(w.desc, w.dir.path, &js);
    // Background reads finish whenever the pool gets to them (the suite runs tests in parallel).
    auto until_active = [&](const Uuid& id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (st.state(id) != CellState::Active && std::chrono::steady_clock::now() < deadline) {
            st.update(s, at(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return st.state(id) == CellState::Active;
    };
    CHECK(until_active(w.desc.cells[0].id));
    CHECK(until_active(w.desc.cells[1].id));

    // Start cell 2 loading, then leave at once: the load is cancelled or discarded.
    st.update(s, at(250));
    st.update(s, at(-5000));
    st.wait_idle();
    st.update(s, at(-5000));
    CHECK(st.state(w.desc.cells[2].id) == CellState::Unloaded);
    CHECK(st.stats().active == 0);
    CHECK(s.size() == 0);
}

TEST_CASE("streamer reports missing cells as failed") {
    register_components();
    LineWorld w("victus_world_missing", 2);
    std::filesystem::remove(w.dir.path / w.desc.cells[1].scene);
    Scene s;
    WorldStreamer st(w.desc, w.dir.path);
    st.update(s, at(100));
    st.update(s, at(100));
    CHECK(st.state(w.desc.cells[0].id) == CellState::Active);
    CHECK(st.state(w.desc.cells[1].id) == CellState::Failed);
    REQUIRE(st.error(w.desc.cells[1].id) != nullptr);
    CHECK(st.stats().failed == 1);
}

TEST_CASE("partition_scene splits root subtrees into grid cells") {
    register_components();
    Scene s;
    const Entity cam = s.create("camera");
    s.world().add<scene::CameraComponent>(cam, scene::CameraComponent{});
    const Entity a = add_prop(s, "a", {10, 0, 10});
    add_prop(s, "a_child", {0, 1, 0}, a);
    add_prop(s, "b", {20, 0, 30});      // same 64-cell as a
    add_prop(s, "c", {130, 0, -10});    // cell (2,-1)
    const Entity ground = add_prop(s, "ground", {0, 0, 0});
    s.world().get<Transform>(ground)->scale = {200, 1, 200}; // wider than a cell: stays persistent
    PartitionSettings ps;
    ps.cell_size = 64;
    ps.directory = "worlds/test";
    ps.name = "Test";
    auto part = partition_scene(s, ps, "worlds/test.vworld");
    REQUIRE(part.ok());
    CHECK(part->moved_entities == 4);
    REQUIRE(part->world.cells.size() == 2);
    CHECK(part->world.cells[0].name == "cell_0_0");
    CHECK(part->world.cells[1].name == "cell_2_-1");
    CHECK(part->cells[0].first == "worlds/test/cell_0_0.vscene");
    CHECK(part->cells[0].second["entities"].size() == 3);
    CHECK(part->world.load_radius == doctest::Approx(96));
    CHECK(parse_world(world_to_json(part->world)).ok());

    // The persistent scene keeps the camera and gains the WorldStreaming marker.
    Scene persistent;
    REQUIRE(persistent.load(part->persistent).ok());
    CHECK(persistent.size() == 3);
    CHECK(persistent.find_by_name("ground") != scene::kNullEntity);
    CHECK(persistent.find_by_name("camera") != scene::kNullEntity);
    const Entity marker = persistent.find_by_name("World Streaming");
    REQUIRE(marker != scene::kNullEntity);
    CHECK(persistent.world().get<WorldStreamingComponent>(marker)->world == "worlds/test.vworld");

    // Streaming the partition back reproduces the original content.
    TempDir dir("victus_world_partition");
    for (const auto& [rel, doc] : part->cells) {
        std::filesystem::create_directories((dir.path / rel).parent_path());
        REQUIRE(fs::write_text_atomic(dir.path / rel, json::stringify(doc)).ok());
    }
    WorldStreamer st(part->world, dir.path);
    std::vector<StreamingSource> both{{{20, 0, 20}, 1.0f}, {{140, 0, -20}, 1.0f}};
    st.update(persistent, both);
    st.update(persistent, both);
    CHECK(persistent.size() == 7);
    CHECK(persistent.find_by_name("a_child") != scene::kNullEntity);

    CHECK_FALSE(partition_scene(s, {.cell_size = 0}, "w.vworld").ok());
}

TEST_CASE("gather_sources uses streaming source entities, else the fallback") {
    register_components();
    Scene s;
    const math::Vec3 cam{1, 2, 3};
    s.update_transforms();
    auto none = gather_sources(s, &cam);
    REQUIRE(none.size() == 1);
    CHECK(none[0].position.z == doctest::Approx(3));
    const Entity e = s.create("player");
    s.world().get<Transform>(e)->position = {5, 0, 0};
    s.world().add<StreamingSourceComponent>(e, StreamingSourceComponent{.radius_scale = 2.0f});
    s.update_transforms();
    auto one = gather_sources(s, &cam);
    REQUIRE(one.size() == 1);
    CHECK(one[0].position.x == doctest::Approx(5));
    CHECK(one[0].radius_scale == doctest::Approx(2));
}
