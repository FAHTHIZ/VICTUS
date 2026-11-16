// SPDX-License-Identifier: Apache-2.0
// Save games: SaveStore files, backups, migrations; the Luau save API and scene checkpoints.
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/save.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace victus;
using gameplay::SaveStore;
using json::Value;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path / "scripts");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        const auto p = path / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
};

Value obj(std::initializer_list<std::pair<const char*, Value>> kv) {
    Value v = Value::object();
    for (const auto& [k, x] : kv) v.set(k, x);
    return v;
}

std::unique_ptr<script::ScriptHost> make_host(const TempDir& d, bool saves = true, std::uint32_t version = 1) {
    script::HostOptions o;
    o.assets_root = d.path;
    if (saves) o.save_directory = d.path / "saves";
    o.save_version = version;
    auto h = script::ScriptHost::create(o);
    REQUIRE(h.ok());
    return std::move(h).value();
}

Entity scripted(Scene& s, const char* name, const char* path) {
    const Entity e = s.create(name);
    s.world().add<script::ScriptComponent>(e).script = path;
    return e;
}

} // namespace

TEST_CASE("save store: slots round trip, list newest first and validate names") {
    TempDir d("victus_save_store");
    SaveStore store(d.path / "saves", 2);
    CHECK_FALSE(store.exists("one"));
    CHECK(store.read("one").error().code == ErrorCode::NotFound);
    REQUIRE(store.write("one", obj({{"gold", 12}, {"name", "Ada"}}), "Chapter 1").ok());
    REQUIRE(store.write("two", Value(json::Array{1, 2})).ok());
    CHECK(store.exists("one"));
    auto one = store.read("one");
    REQUIRE(one.ok());
    CHECK(one->data["gold"].as_int() == 12);
    CHECK(one->data["name"].as_string() == "Ada");
    CHECK(one->label == "Chapter 1");
    CHECK(one->data_version == 2);
    CHECK(one->saved_at > 1700000000);
    CHECK_FALSE(one->recovered);
    CHECK_FALSE(one->migrated);
    const auto slots = store.list();
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].saved_at >= slots[1].saved_at);
    for (const auto& s : slots) CHECK_FALSE(s.damaged);

    for (const char* bad : {"", ".hidden", "a/b", "..", "with space", "x\\y"}) {
        CHECK_MESSAGE(!gameplay::valid_slot_name(bad), bad);
        CHECK(store.write(bad, Value(1)).error().code == ErrorCode::InvalidArgument);
    }
    CHECK(gameplay::valid_slot_name("slot-1_a.b"));
    CHECK(store.remove("two").ok());
    CHECK(store.remove("two").ok()); // already gone
    CHECK_FALSE(store.exists("two"));
    CHECK(store.list().size() == 1);
}

TEST_CASE("save store: damaged files recover from the backup; newer formats are refused") {
    TempDir d("victus_save_recover");
    SaveStore store(d.path / "saves");
    REQUIRE(store.write("slot", obj({{"level", 1}})).ok());
    REQUIRE(store.write("slot", obj({{"level", 2}})).ok()); // level 1 becomes the backup
    auto good = store.read("slot");
    REQUIRE(good.ok());
    CHECK(good->data["level"].as_int() == 2);

    // A tampered or truncated main file: the checksum fails and the backup is read.
    const auto path = store.slot_path("slot");
    auto text = fs::read_text(path);
    REQUIRE(text.ok());
    std::string tampered = *text;
    tampered.replace(tampered.find("\"level\": 2"), 10, "\"level\": 9");
    REQUIRE(fs::write_text_atomic(path, tampered).ok());
    auto rec = store.read("slot");
    REQUIRE(rec.ok());
    CHECK(rec->recovered);
    CHECK(rec->data["level"].as_int() == 1);
    REQUIRE(fs::write_text_atomic(path, text->substr(0, text->size() / 2)).ok());
    rec = store.read("slot");
    REQUIRE(rec.ok());
    CHECK(rec->recovered);
    const auto listed = store.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].damaged);

    // Writing over a damaged file keeps the last good backup (level 1), not the damaged file.
    REQUIRE(store.write("slot", obj({{"level", 3}})).ok());
    std::filesystem::remove(path);
    rec = store.read("slot");
    REQUIRE(rec.ok());
    CHECK(rec->recovered);
    CHECK(rec->data["level"].as_int() == 1);

    // A file from a future engine is refused, not misread.
    Value doc = gameplay::make_save_document(obj({{"x", 1}}), 1, 0, "");
    doc.set("version", gameplay::kSaveFormatVersion + 1);
    CHECK(gameplay::parse_save_document(json::stringify(doc)).error().code == ErrorCode::VersionMismatch);
    CHECK_FALSE(gameplay::parse_save_document("{}").ok());
    CHECK_FALSE(gameplay::parse_save_document("not json").ok());
}

TEST_CASE("save store: migrations upgrade old data in order; newer data is refused") {
    TempDir d("victus_save_migrate");
    {
        SaveStore v1(d.path / "saves", 1);
        REQUIRE(v1.write("slot", obj({{"hp", 10}})).ok());
    }
    SaveStore v3(d.path / "saves", 3);
    CHECK(v3.read("slot").error().code == ErrorCode::VersionMismatch); // no migrations registered
    v3.add_migration(1, [](Value data) -> Result<Value> {
        data.set("health", data["hp"]); // v2 renamed hp -> health
        return data;
    });
    v3.add_migration(2, [](Value data) -> Result<Value> {
        data.set("max_health", 100); // v3 added max_health
        return data;
    });
    auto up = v3.read("slot");
    REQUIRE(up.ok());
    CHECK(up->migrated);
    CHECK(up->data_version == 1);
    CHECK(up->data["health"].as_int() == 10);
    CHECK(up->data["max_health"].as_int() == 100);
    auto raw = v3.read("slot", false);
    REQUIRE(raw.ok());
    CHECK(raw->data.find("health") == nullptr);

    REQUIRE(v3.write("new", obj({{"health", 5}})).ok());
    SaveStore v2(d.path / "saves", 2);
    CHECK(v2.read("new").error().code == ErrorCode::VersionMismatch);

    SaveStore failing(d.path / "saves", 2);
    failing.add_migration(1, [](Value) -> Result<Value> { return make_error(ErrorCode::ParseError, "nope"); });
    auto f = failing.read("slot");
    REQUIRE_FALSE(f.ok());
    CHECK(f.error().message.find("nope") != std::string::npos);
}

TEST_CASE("script: save API writes, reads, lists, migrates and keeps entity references") {
    TempDir d("victus_save_script");
    script::register_components();
    {
        auto v1 = make_host(d, true, 1);
        Scene s;
        const Entity hero = s.create("Hero");
        (void)hero;
        auto r = v1->eval(R"lua(
            assert(save.available())
            assert(save.version() == 1)
            assert(not save.exists("slot1"))
            local data, err = save.read("slot1")
            assert(data == nil and string.find(err, "slot1"), err)
            assert(save.write("slot1", { coins = 3, pos = vector.create(1, 2, 3), hero = scene.find("Hero") }, "First"))
            assert(save.write("profile", { volume = 0.5 }))
            local ok, werr = save.write("bad/name", {})
            assert(not ok and string.find(werr, "invalid"), werr)
            local back, info = save.read("slot1")
            assert(back.coins == 3 and back.pos == vector.create(1, 2, 3), "data")
            assert(back.hero == scene.find("Hero"), "entity reference")
            assert(info.label == "First" and info.version == 1 and not info.recovered and not info.migrated)
            local slots = save.list()
            assert(#slots == 2, "two slots")
            assert(save.delete("profile"))
            assert(#save.list() == 1)
            return true
        )lua", &s);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        CHECK(v1->stats().errors == 0);
    }
    // The game moves to save version 2 and upgrades old saves with a Luau migration.
    auto v2 = make_host(d, true, 2);
    Scene s2;
    auto r = v2->eval(R"lua(
        save.migrate(1, function(data)
            data.gold = data.coins * 10
            data.coins = nil
            return data
        end)
        local data, info = save.read("slot1")
        assert(data, info)
        assert(info.migrated and info.version == 1, "migrated from 1")
        assert(data.gold == 30 and data.coins == nil, "migrated data")
        assert(data.hero == nil, "entity from another scene is nil")
        return data.gold
    )lua", &s2);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->as_int() == 30);

    // Without a save directory every call fails softly.
    auto none = make_host(d, false);
    auto n = none->eval(R"lua(
        assert(not save.available())
        local ok, err = save.write("x", 1)
        assert(ok == nil and string.find(err, "not available"))
        assert(#save.list() == 0 and not save.exists("slot1"))
        local c, cerr = save.checkpoint("x")
        assert(c == nil and cerr)
        return true
    )lua", &s2);
    REQUIRE_MESSAGE(n.ok(), (n.ok() ? "" : n.error().message));
    CHECK(none->saves() == nullptr);
}

TEST_CASE("script: checkpoints restore the scene and every script's state") {
    TempDir d("victus_save_checkpoint");
    script::register_components();
    // A counter that moves, spawns a marker at frame 3, saves at 5 and loads at 10.
    d.write("scripts/counter.luau", R"lua(
local C = {}
function C:start()
    self.count = 0
    self.starts = (self.starts or 0) + 1
    self.loaded = nil
end
function C:save_state()
    return { count = self.count, marker = self.marker }
end
function C:load_state(state)
    self.count = state.count
    self.marker = state.marker
    self.loaded = true
end
function C:update(dt)
    self.count += 1
    self.entity.position = vector.create(self.count, 0, 0)
    self.entity.name = "Counter " .. self.count
    if self.count == 3 then self.marker = scene.spawn("Marker") end
    if self.count == 5 and not self.loaded then assert(save.checkpoint("cp", "five")) end
    if self.count == 7 and self.marker then self.marker:destroy(); self.marker = nil; scene.spawn("Extra") end
    if self.count == 10 and not self.loaded then assert(save.restore("cp")) end
end
return C
)lua");
    Scene s;
    scripted(s, "Counter", "scripts/counter.luau");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 60.0f);
    script::add_script_systems(sim, *host);
    for (int i = 0; i < 10; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(host->stats().errors == 0);
    // The restore applied at the end of frame 10: the scene is the frame-5 scene.
    CHECK(s.find_by_name("Extra").is_null());
    CHECK_FALSE(s.find_by_name("Marker").is_null());
    const Entity counter = s.find_by_name("Counter 5");
    REQUIRE_FALSE(counter.is_null());
    CHECK(s.world().get<Transform>(counter)->position.x == doctest::Approx(5.0f));
    CHECK(host->stats().instances == 0); // restarted on the next update
    sim.tick(s, 1.0f / 60.0f);
    // start ran again, then load_state restored count 5 and the marker reference; update -> 6.
    const Entity after = s.find_by_name("Counter 6");
    REQUIRE_FALSE(after.is_null());
    auto r = host->eval(R"lua(
        local c = scene.find("Counter 6")
        assert(c, "counter")
        local m = scene.find("Marker")
        assert(m and scene.find_id(m.id) == m, "find_id")
        assert(scene.find_id("not-a-uuid") == nil)
        return true
    )lua", &s);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    for (int i = 0; i < 4; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(host->stats().errors == 0);
    CHECK_FALSE(s.find_by_name("Counter 10").is_null()); // loaded: no second restore
    CHECK(s.find_by_name("Marker").is_null());           // the restored reference was destroyed at 7
    CHECK_FALSE(s.find_by_name("Extra").is_null());

    // The C++ API: checkpoint and restore outside update.
    REQUIRE(host->checkpoint(s, "cpp", "from C++").ok());
    for (int i = 0; i < 3; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(s.find_by_name("Counter 13").is_null());
    REQUIRE(host->restore(s, "cpp").ok());
    CHECK_FALSE(s.find_by_name("Counter 10").is_null());
    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(s.find_by_name("Counter 11").is_null());
    auto list = host->saves()->list();
    CHECK(list.size() == 2);

    // Restoring something that is not a checkpoint (or is missing) fails and keeps the scene.
    REQUIRE(host->saves()->write("plain", obj({{"a", 1}})).ok());
    CHECK_FALSE(host->restore(s, "plain").ok());
    CHECK_FALSE(host->restore(s, "missing").ok());
    CHECK_FALSE(s.find_by_name("Counter 11").is_null());
    CHECK(host->stats().errors == 0);
}

TEST_CASE("script: restoring a checkpoint rebuilds physics bodies") {
    TempDir d("victus_save_physics");
    script::register_components();
    d.write("scripts/noop.luau", "local M = {}\nreturn M\n");
    Scene s;
    const Entity ground = s.create("Ground");
    s.world().get<Transform>(ground)->scale = {20, 1, 20};
    s.world().add<physics::ColliderComponent>(ground);
    const Entity box = s.create("Box");
    s.world().get<Transform>(box)->position = {0, 10, 0};
    s.world().add<physics::RigidBodyComponent>(box);
    s.world().add<physics::ColliderComponent>(box);
    scripted(s, "Logic", "scripts/noop.luau");
    auto host = make_host(d);
    auto world = physics::PhysicsWorld::create();
    REQUIRE(world.ok());
    gameplay::Simulation sim(1.0f / 60.0f);
    script::add_script_systems(sim, *host);
    physics::add_physics_systems(sim, **world);
    for (int i = 0; i < 20; ++i) sim.tick(s, 1.0f / 60.0f);
    const float y_saved = s.world().get<Transform>(s.find_by_name("Box"))->position.y;
    CHECK(y_saved < 10.0f);
    REQUIRE(host->checkpoint(s, "fall").ok()); // default (empty) label
    for (int i = 0; i < 120; ++i) sim.tick(s, 1.0f / 60.0f);
    const float y_landed = s.world().get<Transform>(s.find_by_name("Box"))->position.y;
    CHECK(y_landed < 1.6f); // resting on the ground
    REQUIRE(host->restore(s, "fall").ok());
    const Entity restored = s.find_by_name("Box");
    REQUIRE_FALSE(restored.is_null());
    CHECK(s.world().get<Transform>(restored)->position.y == doctest::Approx(y_saved));
    for (int i = 0; i < 120; ++i) sim.tick(s, 1.0f / 60.0f);
    // The new entity got a body: it falls again and lands (bodies restart at rest).
    CHECK(s.world().get<Transform>(restored)->position.y == doctest::Approx(y_landed).epsilon(0.05));
    CHECK(host->stats().errors == 0);
}
