// SPDX-License-Identifier: Apache-2.0
// Procedural trees (ADR-0243): meshes and the TreeComponent scene build.
#include "victus/procgen/tree.hpp"

#include "victus/procgen/city.hpp"
#include "victus/scene/scene.hpp"

#include <doctest.h>

#include <algorithm>

using namespace victus;
using namespace victus::procgen;
using math::Vec3;

namespace {

// Share of triangles whose winding (counter-clockwise front faces) agrees with their vertex normals.
float outward_share(const scene::ProceduralMesh& m) {
    std::size_t ok = 0;
    const std::size_t tris = m.indices.size() / 3;
    for (std::size_t t = 0; t < tris; ++t) {
        const auto a = m.indices[t * 3], b = m.indices[t * 3 + 1], c = m.indices[t * 3 + 2];
        const Vec3 face = math::cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
        const Vec3 n = m.normals[a] + m.normals[b] + m.normals[c];
        if (math::dot(face, n) > 0.0f) ++ok;
    }
    return tris ? static_cast<float>(ok) / static_cast<float>(tris) : 0.0f;
}

float max_y(const scene::ProceduralMesh& m) {
    float y = -1e9f;
    for (const Vec3& p : m.positions) y = std::max(y, p.y);
    return y;
}

float max_radius(const scene::ProceduralMesh& m) {
    float r = 0.0f;
    for (const Vec3& p : m.positions) r = std::max(r, std::sqrt(p.x * p.x + p.z * p.z));
    return r;
}

} // namespace

TEST_CASE("trees: broadleaf and conifer meshes are deterministic, outward-facing and within budget (ADR-0243)") {
    for (const TreeKind kind : {TreeKind::Broadleaf, TreeKind::Conifer}) {
        CAPTURE(static_cast<int>(kind));
        TreeSettings st;
        st.kind = kind;
        st.seed = 7;
        st.height = 8.0f;
        st.crown_radius = 2.5f;
        const TreeMeshes a = build_tree_meshes(st);
        const TreeMeshes b = build_tree_meshes(st);
        REQUIRE(!a.trunk.indices.empty());
        REQUIRE(!a.crown.indices.empty());
        CHECK(a.crown.positions == b.crown.positions);
        CHECK(a.trunk.indices == b.trunk.indices);
        st.seed = 8;
        CHECK(build_tree_meshes(st).crown.positions != a.crown.positions);
        for (const auto* m : {&a.trunk, &a.crown}) {
            CHECK(m->normals.size() == m->positions.size());
            CHECK(m->colors.size() == m->positions.size());
            CHECK(outward_share(*m) > 0.97f); // back-face culling keeps the outside
        }
        // Scale: the crown reaches about the requested height and radius.
        const float top = std::max(max_y(a.crown), max_y(a.trunk));
        CHECK(top > 0.85f * 8.0f);
        CHECK(top < 1.15f * 8.0f);
        CHECK(max_radius(a.crown) > 0.8f * 2.5f);
        CHECK(max_radius(a.crown) < 1.6f * 2.5f);
        // Cheap enough for dozens of trees: a few thousand triangles each.
        const std::size_t tris = (a.trunk.indices.size() + a.crown.indices.size()) / 3;
        MESSAGE("triangles: " << tris);
        CHECK(tris < 4000);
    }
}

TEST_CASE("trees: TreeComponent builds transient trunk and crown meshes and rebuilds on change (ADR-0243)") {
    procgen::register_components();
    scene::Scene s;
    const scene::Entity tree = s.create("Tree");
    auto& tc = s.world().add<TreeComponent>(tree);
    Uuid leaves, bark;
    REQUIRE(Uuid::parse("a3e1f0c2-6b7d-4e58-9c14-2f8b5d7e0a32", leaves));
    REQUIRE(Uuid::parse("a3e1f0c2-6b7d-4e58-9c14-2f8b5d7e0a33", bark));
    tc.leaf_material = leaves;
    tc.bark_material = bark;
    tc.cast_shadows = false;

    const auto parts = [&] {
        std::vector<scene::Entity> out;
        for (const scene::Entity c : s.children(tree))
            if (s.world().get<scene::TransientComponent>(c) && s.world().get<scene::ProceduralMeshComponent>(c)) out.push_back(c);
        return out;
    };
    CHECK(build_trees(s) == 1);
    auto kids = parts();
    REQUIRE(kids.size() == 2);
    bool saw_leaves = false, saw_bark = false;
    for (const scene::Entity k : kids) {
        const auto* mr = s.world().get<scene::MeshRendererComponent>(k);
        REQUIRE(mr);
        CHECK_FALSE(mr->cast_shadows);
        saw_leaves |= mr->material_asset == leaves;
        saw_bark |= mr->material_asset == bark;
        CHECK(s.world().get<scene::ProceduralMeshComponent>(k)->mesh);
    }
    CHECK(saw_leaves);
    CHECK(saw_bark);
    CHECK(build_trees(s) == 0); // unchanged: no rebuild

    s.world().get<TreeComponent>(tree)->kind = TreeKind::Conifer;
    CHECK(build_trees(s) == 1);
    CHECK(parts().size() == 2); // old meshes replaced, not duplicated

    // Saved scenes keep the component but not the generated meshes; loading rebuilds them.
    const auto saved = s.save();
    scene::Scene copy;
    REQUIRE(copy.load(saved).ok());
    scene::Entity loaded{};
    copy.world().each<TreeComponent>([&](scene::Entity e, TreeComponent& c) {
        loaded = e;
        CHECK(c.kind == TreeKind::Conifer);
        CHECK(c.leaf_material == leaves);
    });
    REQUIRE(loaded != scene::Entity{});
    CHECK(copy.children(loaded).empty());
    CHECK(build_trees(copy) == 1);
    CHECK(copy.children(loaded).size() == 2);
}
