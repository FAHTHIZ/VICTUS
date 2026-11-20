// SPDX-License-Identifier: Apache-2.0
// Baked lighting for scenes (ADR-0125): bakes a scene's static mesh renderers into a
// `.vlightmap` file (render::bake_lightmaps) and loads one for drawing.
//
// A scene opts in with an Environment component whose `lightmap` names a project-relative
// `.vlightmap` file, and with MeshRenderers marked `lightmapped` (static lighting). Every
// visible, unskinned mesh renderer blocks and bounces light during the bake; only lightmapped
// ones receive texels. A stored entry is used only while the entity's mesh still has the
// fingerprint it was baked with (geometry and world transform), so edited or moved objects
// fall back to probe/sky ambient instead of drawing a wrong bake.
#pragma once
#include "victus/assets/database.hpp"
#include "victus/render/lightmap.hpp"
#include "victus/scene/scene.hpp"

#include <string>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::runtime {

inline constexpr std::string_view kLightmapExtension = ".vlightmap";

// CPU geometry of a built-in primitive (cube|sphere|plane|cylinder; empty when unknown) and of a
// procedural mesh, built exactly as GpuAssetCache uploads them.
[[nodiscard]] render::MeshData primitive_mesh_data(std::string_view name);
[[nodiscard]] render::MeshData procedural_mesh_data(const scene::ProceduralMesh& mesh);

// The CPU geometry a MeshRenderer draws, built exactly as GpuAssetCache builds its GPU copy
// (primitives, procedural meshes, cooked model meshes), so fingerprints match. Fails for
// renderers without geometry, missing assets and morphed or skinned meshes.
// Mean linear colour of a cooked texture (from its smallest mip; sRGB decoded). The baker multiplies
// a model material's base colour factor by its albedo texture's mean (ADR-0145). White when empty.
[[nodiscard]] math::Vec3 texture_mean_color(const assets::CookedTexture& texture);

[[nodiscard]] Result<render::MeshData> static_mesh_data(scene::Scene& scene, scene::Entity entity, assets::AssetDatabase* db);

struct SceneLightmapBake {
    render::LightmapFile file;
    render::LightmapBakeStats stats;
    std::uint32_t receivers{0}; // lightmapped mesh renderers baked
    std::uint32_t blockers{0};  // other mesh renderers that only block and bounce light
    std::uint32_t lights{0};
    std::vector<std::string> warnings; // skipped renderers and why
};

// Everything a bake reads, copied out of a scene so the bake itself can run on another thread
// while the scene keeps changing (Studio).
struct PreparedLightmapBake {
    struct Item {
        Uuid entity;
        render::MeshData mesh;
        math::Mat4 model;
        math::Vec3 albedo{0.8f, 0.8f, 0.8f};
        math::Vec3 emissive{0, 0, 0};
        bool receives{false};
    };
    std::vector<Item> items;
    std::vector<render::LightmapLight> lights;
    render::LightmapSky sky;
    render::LightmapSettings settings;
    std::vector<std::string> warnings;
};

// Gathers the scene (requires up-to-date WorldTransforms: call scene.update_transforms()). The
// sky is the Environment's analytic sky times its ambient intensity (environment maps and the
// physical sky are not sampled yet); lights are the LightComponents (radiance = colour x
// intensity). Fails when no visible mesh renderer is lightmapped.
[[nodiscard]] Result<PreparedLightmapBake> prepare_scene_lightmap(scene::Scene& scene, assets::AssetDatabase* db,
                                                                  const render::LightmapSettings& settings = {});
// Bakes a prepared scene (CPU, on `jobs` when given). Safe on any thread.
[[nodiscard]] Result<SceneLightmapBake> bake_prepared_lightmap(const PreparedLightmapBake& prepared, jobs::JobSystem* jobs = nullptr);
// prepare_scene_lightmap + bake_prepared_lightmap.
[[nodiscard]] Result<SceneLightmapBake> bake_scene_lightmap(scene::Scene& scene, assets::AssetDatabase* db,
                                                            const render::LightmapSettings& settings = {},
                                                            jobs::JobSystem* jobs = nullptr);

// What a stored bake entry is checked against: the mesh's geometry and the entity's world
// transform (quantised to 0.1 mm), so moved or edited objects stop using a bake made for them.
[[nodiscard]] std::uint64_t scene_lightmap_fingerprint(const render::MeshData& mesh, const math::Mat4& world) noexcept;

// Default bake location for a project-relative scene path: "scenes/main.vscene" ->
// "scenes/main.vlightmap".
[[nodiscard]] std::string default_lightmap_path(std::string_view scene_path);

} // namespace victus::runtime
