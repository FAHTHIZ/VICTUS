// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/memory.hpp"

#include <cstdint>
#include <set>

using namespace victus::memory;

TEST_CASE("arena: alignment, growth and reset") {
    LinearArena arena(1024);
    auto* a = static_cast<std::uint8_t*>(arena.allocate(3, 1));
    auto* b = arena.allocate(16, 16);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    CHECK(a != nullptr);
    void* big = arena.allocate(5000, 8);
    CHECK(big != nullptr);
    CHECK(arena.chunk_count() == 2);
    CHECK(arena.bytes_used() == 3 + 16 + 5000);
    arena.reset();
    CHECK(arena.chunk_count() == 1);
    CHECK(arena.bytes_used() == 0);
    int* ints = arena.allocate_array<int>(100);
    for (int i = 0; i < 100; ++i) ints[i] = i;
    CHECK(ints[99] == 99);
}

TEST_CASE("pool: stable addresses, reuse and live count") {
    struct Obj {
        int v;
        explicit Obj(int x) : v(x) {}
    };
    Pool<Obj, 8> pool;
    std::set<Obj*> ptrs;
    std::vector<Obj*> objs;
    for (int i = 0; i < 20; ++i) {
        objs.push_back(pool.create(i));
        ptrs.insert(objs.back());
    }
    CHECK(ptrs.size() == 20);
    CHECK(pool.live() == 20);
    CHECK(pool.capacity() == 24);
    for (int i = 0; i < 20; ++i) CHECK(objs[static_cast<std::size_t>(i)]->v == i);
    Obj* freed = objs[5];
    pool.destroy(freed);
    CHECK(pool.live() == 19);
    Obj* reused = pool.create(99);
    CHECK(reused == freed);
    CHECK(reused->v == 99);
}
