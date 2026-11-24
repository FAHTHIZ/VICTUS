// SPDX-License-Identifier: Apache-2.0
#include "victus/ecs/world.hpp"

#include "doctest.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <set>
#include <string>

using namespace victus;
using namespace victus::ecs;

namespace {
struct Position {
    float x{0}, y{0};
};
struct Velocity {
    float x{0}, y{0};
};
struct Tag {};
struct Named {
    std::string name;
};
struct Tracked {
    static inline int live = 0;
    std::unique_ptr<int> payload;
    Tracked() : payload(std::make_unique<int>(7)) { ++live; }
    Tracked(Tracked&& o) noexcept : payload(std::move(o.payload)) { ++live; }
    Tracked& operator=(Tracked&& o) noexcept {
        payload = std::move(o.payload);
        return *this;
    }
    ~Tracked() { --live; }
};
struct alignas(64) Aligned { // exactly 64 bytes: no padding (MSVC C4324)
    float v[16]{};
};
} // namespace

TEST_CASE("ecs: create, destroy and generational handles") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(w.alive(a));
    CHECK(w.alive(b));
    CHECK(w.alive_count() == 2);
    w.destroy(a);
    CHECK_FALSE(w.alive(a));
    const Entity c = w.create(); // reuses a's slot with a new generation
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK_FALSE(w.alive(a));
    CHECK(w.get<Position>(a) == nullptr);
    w.destroy(a); // stale destroy is a no-op
    CHECK(w.alive(c));
    CHECK(w.alive_count() == 2);
    CHECK_FALSE(w.alive(kNullEntity));
}

TEST_CASE("ecs: add/get/remove moves between archetypes and preserves data") {
    World w;
    const Entity e = w.create();
    w.add<Position>(e, 1.0f, 2.0f);
    w.add<Named>(e, "hero");
    REQUIRE(w.get<Position>(e));
    CHECK(w.get<Position>(e)->y == 2.0f);
    w.add<Velocity>(e, 3.0f, 4.0f);
    CHECK(w.get<Position>(e)->x == 1.0f);
    CHECK(w.get<Named>(e)->name == "hero");
    CHECK(w.get<Velocity>(e)->y == 4.0f);
    w.remove<Position>(e);
    CHECK_FALSE(w.has<Position>(e));
    CHECK(w.get<Named>(e)->name == "hero");
    CHECK(w.get<Velocity>(e)->x == 3.0f);
    // add on existing replaces value
    w.add<Velocity>(e, 9.0f, 9.0f);
    CHECK(w.get<Velocity>(e)->x == 9.0f);
    CHECK(w.components_of(e).size() == 2);
}

TEST_CASE("ecs: swap-remove keeps other entities' rows correct") {
    World w;
    std::vector<Entity> es;
    for (int i = 0; i < 100; ++i) {
        const Entity e = w.create();
        w.add<Position>(e, static_cast<float>(i), 0.0f);
        es.push_back(e);
    }
    for (int i = 0; i < 100; i += 3) w.destroy(es[static_cast<std::size_t>(i)]);
    for (int i = 0; i < 100; ++i) {
        const Entity e = es[static_cast<std::size_t>(i)];
        if (i % 3 == 0) {
            CHECK_FALSE(w.alive(e));
        } else {
            REQUIRE(w.get<Position>(e));
            CHECK(w.get<Position>(e)->x == static_cast<float>(i));
        }
    }
    CHECK(w.count<Position>() == 66);
}

TEST_CASE("ecs: non-trivial component lifetimes are balanced") {
    Tracked::live = 0;
    {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 300; ++i) { // forces column growth (moves)
            const Entity e = w.create();
            w.add<Tracked>(e);
            if (i % 2) w.add<Position>(e);
            es.push_back(e);
        }
        CHECK(Tracked::live == 300);
        for (int i = 0; i < 300; i += 5) w.destroy(es[static_cast<std::size_t>(i)]);
        CHECK(Tracked::live == 240);
        for (const Entity e : es) {
            if (w.alive(e)) {
                REQUIRE(w.get<Tracked>(e)->payload);
                CHECK(*w.get<Tracked>(e)->payload == 7);
            }
        }
        w.remove<Tracked>(es[1]);
        CHECK(Tracked::live == 239);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("ecs: queries match archetypes incrementally and support exclusion") {
    World w;
    for (int i = 0; i < 10; ++i) {
        const Entity e = w.create();
        w.add<Position>(e, static_cast<float>(i), 0.0f);
        if (i % 2 == 0) w.add<Velocity>(e, 1.0f, 0.0f);
        if (i % 5 == 0) w.add<Tag>(e);
    }
    int moving = 0;
    w.each<Position, Velocity>([&](Position& p, Velocity& v) {
        p.x += v.x;
        ++moving;
    });
    CHECK(moving == 5);
    int still = 0;
    w.each_excluding<Position>(World::Exclude<Velocity>{}, [&](Entity, Position&) { ++still; });
    CHECK(still == 5);
    // New archetype created after the query was cached must be found.
    const Entity late = w.create();
    w.add<Position>(late);
    w.add<Velocity>(late);
    w.add<Named>(late, "late");
    CHECK(w.count<Position, Velocity>() == 6);
    std::set<std::uint32_t> seen;
    w.each<Velocity>([&](Entity e, Velocity&) { seen.insert(e.index); });
    CHECK(seen.size() == 6);
}

TEST_CASE("ecs: deterministic iteration order") {
    auto build = [] {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 50; ++i) {
            const Entity e = w.create();
            w.add<Position>(e, static_cast<float>(i), 0.0f);
            if (i % 3 == 0) w.add<Velocity>(e);
            es.push_back(e);
        }
        for (int i = 0; i < 50; i += 7) w.destroy(es[static_cast<std::size_t>(i)]);
        std::vector<float> order;
        w.each<Position>([&](Position& p) { order.push_back(p.x); });
        return order;
    };
    CHECK(build() == build());
}

TEST_CASE("ecs: deferred commands apply after iteration") {
    World w;
    for (int i = 0; i < 20; ++i) w.add<Position>(w.create(), static_cast<float>(i), 0.0f);
    w.each<Position>([&](Entity e, Position& p) {
        if (static_cast<int>(p.x) % 2 == 0) w.commands().destroy(e);
        else w.commands().add<Tag>(e, Tag{});
    });
    w.commands().spawn([](World& world, Entity e) { world.add<Named>(e, "spawned"); });
    CHECK(w.alive_count() == 20);
    w.flush();
    CHECK(w.alive_count() == 11);
    CHECK(w.count<Position, Tag>() == 10);
    CHECK(w.count<Named>() == 1);
}

TEST_CASE("ecs: parallel iteration touches every row exactly once") {
    jobs::JobSystem js(2);
    World w;
    for (int i = 0; i < 20000; ++i) {
        const Entity e = w.create();
        w.add<Position>(e);
        w.add<Velocity>(e, 1.0f, 2.0f);
        if (i % 4 == 0) w.add<Tag>(e);
    }
    std::atomic<int> rows{0};
    w.par_each<Position, Velocity>(js, 256, [&](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        rows.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(rows.load() == 20000);
    bool all = true;
    w.each<Position>([&](const Position& p) { all = all && p.x == 1.0f && p.y == 2.0f; });
    CHECK(all);
}

TEST_CASE("ecs: over-aligned components, clone, clear, resources, type-erased access") {
    World w;
    const Entity e = w.create();
    for (int i = 0; i < 40; ++i) w.add<Aligned>(w.create());
    auto& a = w.add<Aligned>(e);
    CHECK(reinterpret_cast<std::uintptr_t>(&a) % 64 == 0);
    w.add<Named>(e, "orig");
    w.add<Position>(e, 5.0f, 6.0f);
    const Entity c = w.clone(e);
    CHECK(w.get<Named>(c)->name == "orig");
    CHECK(w.get<Position>(c)->y == 6.0f);
    w.get<Named>(c)->name = "copy";
    CHECK(w.get<Named>(e)->name == "orig");

    const ComponentId pid = component_id<Position>();
    auto* raw = static_cast<Position*>(w.get_raw(e, pid));
    REQUIRE(raw);
    CHECK(raw->x == 5.0f);
    const Entity f = w.create();
    auto* added = static_cast<Position*>(w.add_raw(f, pid));
    CHECK(added->x == 0.0f);

    struct Gravity {
        float g{-9.81f};
    };
    w.set_resource<Gravity>();
    REQUIRE(w.resource<Gravity>());
    CHECK(w.resource<Gravity>()->g == doctest::Approx(-9.81f));

    ComponentRegistry::instance().set_name(pid, "TestPosition");
    CHECK(ComponentRegistry::instance().find("TestPosition") == pid);

    w.clear();
    CHECK(w.alive_count() == 0);
    CHECK_FALSE(w.alive(e));
    CHECK(w.count<Position>() == 0);
    const Entity g = w.create();
    CHECK(w.alive(g));
}

TEST_CASE("ecs: runtime-defined component") {
    ComponentInfo info;
    info.name = "Dynamic16";
    info.size = 16;
    info.align = 4;
    info.construct = [](void* p) { std::memset(p, 0xAB, 16); };
    info.destroy = [](void*) {};
    info.move_construct = [](void* d, void* s) { std::memcpy(d, s, 16); };
    info.copy_construct = [](void* d, const void* s) { std::memcpy(d, s, 16); };
    const ComponentId id = ComponentRegistry::instance().register_component(info);
    World w;
    const Entity e = w.create();
    auto* bytes = static_cast<std::uint8_t*>(w.add_raw(e, id));
    CHECK(bytes[15] == 0xAB);
    w.add<Position>(e, 1.0f, 1.0f); // move to new archetype keeps the dynamic payload
    CHECK(static_cast<std::uint8_t*>(w.get_raw(e, id))[0] == 0xAB);
    QueryDesc d;
    d.include.set(id);
    std::size_t rows = 0;
    for (const Archetype* a : w.match(d)) rows += a->size();
    CHECK(w.match(d).size() == 2); // {Dynamic16} (now empty) and {Dynamic16, Position}
    CHECK(rows == 1);
}

TEST_CASE("ecs: each_archetype visits non-empty matching archetypes with optional columns") {
    World w;
    for (int i = 0; i < 3; ++i) w.add<Position>(w.create()).x = static_cast<float>(i);
    for (int i = 0; i < 2; ++i) {
        const Entity e = w.create();
        w.add<Position>(e).x = 10.0f + static_cast<float>(i);
        w.add<Velocity>(e).x = 1.0f;
    }
    const Entity empty = w.create();
    w.add<Position>(empty);
    w.add<Tag>(empty);
    w.destroy(empty); // leaves an empty Position+Tag archetype behind
    std::size_t archetypes = 0, rows = 0, with_velocity = 0;
    float sum = 0.0f;
    w.each_archetype<Position>([&](Archetype& a) {
        ++archetypes;
        CHECK(w.iterating());
        CHECK(a.size() > 0);
        const Position* p = a.column_data<Position>();
        const Velocity* v = a.column_data<Velocity>();
        REQUIRE(p != nullptr);
        for (std::size_t r = 0; r < a.size(); ++r) {
            sum += p[r].x;
            if (v) with_velocity += v[r].x > 0.0f ? 1u : 0u;
            ++rows;
        }
    });
    CHECK(archetypes == 2);
    CHECK(rows == 5);
    CHECK(with_velocity == 2);
    CHECK(sum == doctest::Approx(0.0f + 1.0f + 2.0f + 10.0f + 11.0f));
    CHECK_FALSE(w.iterating());
}
