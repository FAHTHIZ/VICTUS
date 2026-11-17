// SPDX-License-Identifier: Apache-2.0
// Mesh optimisation and LOD generation (Slice 54, ADR-0049): optimize_mesh keeps the triangle set
// and the skin stream aligned while improving vertex cache reuse; generate_lods builds coarser
// levels with honest, increasing error bounds.
#include "doctest.h"
#include "victus/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <set>
#include <tuple>

using namespace victus;
using namespace victus::render;

namespace {

using Key = std::array<long, 9>;

long q(float v) { return std::lround(v * 10000.0f); }

// Triangle set by vertex positions, rotation-invariant (the smallest corner first, winding kept).
std::multiset<Key> triangle_set(const MeshData& m, const std::vector<std::uint32_t>& idx) {
    std::multiset<Key> out;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        std::array<std::array<long, 3>, 3> c;
        for (int k = 0; k < 3; ++k) {
            const auto& p = m.vertices[idx[t + k]].position;
            c[k] = {q(p.x), q(p.y), q(p.z)};
        }
        int first = 0;
        for (int k = 1; k < 3; ++k)
            if (c[k] < c[first]) first = k;
        Key key{};
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) key[k * 3 + a] = c[(first + k) % 3][a];
        out.insert(key);
    }
    return out;
}

// Average cache miss ratio (vertex shader invocations per triangle) for a 16-entry FIFO cache.
double acmr(const std::vector<std::uint32_t>& idx) {
    std::deque<std::uint32_t> cache;
    std::size_t misses = 0;
    for (const std::uint32_t i : idx) {
        if (std::find(cache.begin(), cache.end(), i) != cache.end()) continue;
        ++misses;
        cache.push_back(i);
        if (cache.size() > 16) cache.pop_front();
    }
    return static_cast<double>(misses) / static_cast<double>(idx.size() / 3);
}

// Shuffles triangle order deterministically (a worst case for the vertex cache).
void scramble(MeshData& m) {
    const std::size_t tris = m.indices.size() / 3;
    std::vector<std::uint32_t> out;
    out.reserve(m.indices.size());
    for (std::size_t k = 0; k < tris; ++k) {
        const std::size_t t = (k * 7919) % tris;
        out.insert(out.end(), m.indices.begin() + static_cast<std::ptrdiff_t>(t * 3),
                   m.indices.begin() + static_cast<std::ptrdiff_t>(t * 3 + 3));
    }
    m.indices = std::move(out);
}

} // namespace

TEST_CASE("mesh optimize: same triangles, aligned skin, better vertex cache reuse") {
    MeshData m = make_uv_sphere(1.0f, 48, 24);
    // Tag every vertex's skin with its own index so the remap can be checked.
    m.skin.resize(m.vertices.size());
    for (std::size_t v = 0; v < m.vertices.size(); ++v) {
        m.skin[v].joints[0] = static_cast<std::uint16_t>(v % 65536);
        m.vertices[v].color = static_cast<std::uint32_t>(v);
    }
    // One unreferenced vertex, which optimisation drops.
    m.vertices.push_back(Vertex3D{});
    m.skin.push_back(SkinVertex{});
    scramble(m);
    const auto before = triangle_set(m, m.indices);
    const double acmr_before = acmr(m.indices);
    const std::size_t vertex_count = m.vertices.size();

    optimize_mesh(m);
    CHECK(m.vertices.size() <= vertex_count - 1); // the sphere's own unused pole copies go too
    std::vector<bool> used(m.vertices.size(), false);
    for (const std::uint32_t i : m.indices) used[i] = true;
    CHECK(std::all_of(used.begin(), used.end(), [](bool u) { return u; }));
    REQUIRE(m.skinned());
    CHECK(triangle_set(m, m.indices) == before);
    for (std::size_t v = 0; v < m.vertices.size(); ++v)
        CHECK(m.skin[v].joints[0] == static_cast<std::uint16_t>(m.vertices[v].color % 65536));
    const double acmr_after = acmr(m.indices);
    MESSAGE("ACMR " << acmr_before << " -> " << acmr_after);
    CHECK(acmr_after < acmr_before * 0.6);
    CHECK(acmr_after < 1.0);
    // Vertex fetch order: vertices appear in first-use order.
    std::uint32_t highest = 0;
    bool first_use = true;
    for (const std::uint32_t i : m.indices) {
        if (i > highest + 1) first_use = false;
        highest = std::max(highest, i);
    }
    CHECK(first_use);

    MeshData empty;
    optimize_mesh(empty); // no-op
    CHECK(empty.vertices.empty());
}

TEST_CASE("mesh lods: coarser levels with increasing, bounded error") {
    const float radius = 1.0f;
    MeshData m = make_uv_sphere(radius, 64, 32);
    const std::size_t full = m.triangle_count();
    LodSettings s;
    s.max_lods = 4;
    const std::uint32_t built = generate_lods(m, s);
    REQUIRE(built >= 3);
    CHECK(built == m.lods.size());
    std::size_t previous = full;
    float previous_error = 0.0f;
    for (const MeshLod& lod : m.lods) {
        REQUIRE(lod.indices.size() % 3 == 0);
        const std::size_t tris = lod.indices.size() / 3;
        MESSAGE("LOD " << tris << " triangles, error " << lod.error);
        CHECK(tris < previous);
        CHECK(static_cast<double>(tris) <= 0.9 * static_cast<double>(previous));
        CHECK(tris >= s.min_triangles);
        CHECK(lod.error >= previous_error);
        CHECK(std::isfinite(lod.error));
        // Error is in mesh units and bounded by max_error relative to the extent (diameter 2).
        CHECK(lod.error <= s.max_error * 2.0f * radius + 1e-4f);
        for (const std::uint32_t i : lod.indices) REQUIRE(i < m.vertices.size());
        // Simplification collapses onto existing vertices, so every triangle centroid stays
        // within the reported error of the sphere surface (chords sit inside it).
        float worst = 0.0f;
        for (std::size_t t = 0; t < lod.indices.size(); t += 3) {
            math::Vec3 c{0, 0, 0};
            for (int k = 0; k < 3; ++k) c = c + m.vertices[lod.indices[t + k]].position;
            c = c * (1.0f / 3.0f);
            worst = std::max(worst, radius - math::length(c));
        }
        CHECK(worst <= std::max(lod.error * 2.0f, 1e-3f));
        previous = tris;
        previous_error = lod.error;
    }
    CHECK(m.lods.back().indices.size() / 3 <= full / 4);
    CHECK(m.triangle_count() == full); // LOD 0 untouched

    // Optimising afterwards remaps the levels with the vertices.
    const auto coarse = triangle_set(m, m.lods.back().indices);
    optimize_mesh(m);
    CHECK(triangle_set(m, m.lods.back().indices) == coarse);
}

TEST_CASE("mesh lods: settings, tiny meshes and append") {
    MeshData sphere = make_uv_sphere();
    LodSettings none;
    none.max_lods = 0;
    CHECK(generate_lods(sphere, none) == 0);
    CHECK(sphere.lods.empty());
    LodSettings one;
    one.max_lods = 1;
    CHECK(generate_lods(sphere, one) == 1);
    LodSettings many;
    many.max_lods = 100; // clamped to kMaxMeshLods - 1
    many.ratio = 0.9f;
    many.min_triangles = 1;
    CHECK(generate_lods(sphere, many) <= kMaxMeshLods - 1);

    // A cube is already minimal: no level clears the minimum triangle count.
    MeshData cube = make_cube();
    CHECK(generate_lods(cube) == 0);

    // Regenerating replaces old levels; append drops them (they no longer describe the mesh).
    REQUIRE(generate_lods(sphere) > 0);
    sphere.append(make_cube(), math::Mat4::translation({3, 0, 0}));
    CHECK(sphere.lods.empty());

    // Locked borders keep an open grid's outline exactly.
    MeshData grid = make_plane({4, 4}, 32);
    LodSettings locked;
    locked.lock_borders = true;
    locked.max_error = 0.2f;
    generate_lods(grid, locked);
    for (const MeshLod& lod : grid.lods) {
        float min_x = 1e9f, max_x = -1e9f;
        for (const std::uint32_t i : lod.indices) {
            min_x = std::min(min_x, grid.vertices[i].position.x);
            max_x = std::max(max_x, grid.vertices[i].position.x);
        }
        CHECK(min_x == doctest::Approx(-2.0f));
        CHECK(max_x == doctest::Approx(2.0f));
    }
}
