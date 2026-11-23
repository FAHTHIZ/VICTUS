// SPDX-License-Identifier: Apache-2.0
// Procedural trees (ADR-0243): a seeded broadleaf or conifer built from vertex-coloured meshes,
// a trunk with branches and a crown, with no textures or alpha testing, so a few dozen trees
// cost a few draws and no overdraw. The TreeComponent turns them into scenery: the trunk and
// crown are transient child meshes, regenerated whenever the settings change (edit mode and
// play), and drawn with the component's materials. The starter leaves and bark materials
// (project/starter_materials.hpp) carry a wind vertex function, so trees sway in every pass
// (camera, shadows) with no per-frame CPU cost.
//
// Conventions: Y up, metres, the tree's base at the owner's origin.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/hash.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>

namespace victus::procgen {

enum class TreeKind : std::uint8_t { Broadleaf, Conifer };

struct TreeSettings {
    std::uint32_t seed{1};
    TreeKind kind{TreeKind::Broadleaf};
    float height{7.0f};       // m, base to crown top
    float crown_radius{2.6f}; // m, widest horizontal radius of the crown
    math::Color leaf_color{0.30f, 0.46f, 0.18f, 1}; // sRGB, varied per clump
    math::Color bark_color{0.36f, 0.27f, 0.2f, 1};  // sRGB
};

struct TreeMeshes {
    scene::ProceduralMesh trunk; // trunk and branches
    scene::ProceduralMesh crown; // leaf masses
};
// Deterministic: the same settings give the same meshes.
[[nodiscard]] TreeMeshes build_tree_meshes(const TreeSettings& settings);

struct TreeComponent {
    std::uint32_t seed{1};
    TreeKind kind{TreeKind::Broadleaf};
    float height{7.0f};
    float crown_radius{2.6f};
    math::Color leaf_color{0.30f, 0.46f, 0.18f, 1};
    math::Color bark_color{0.36f, 0.27f, 0.2f, 1};
    Uuid leaf_material; // nil = plain vertex colours
    Uuid bark_material;
    bool cast_shadows{true};
    std::uint64_t built{0}; // runtime: hash of the settings the children were built from
    [[nodiscard]] TreeSettings settings() const;
};

// Registers TreeComponent with the scene reflection (procgen::register_components calls it).
void register_tree_components();

// (Re)builds the meshes of every TreeComponent whose settings changed since its last build.
// Returns the number of trees built. Cheap when nothing changed (one hash per tree).
std::uint32_t build_trees(scene::Scene& scene);

} // namespace victus::procgen
