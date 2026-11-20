// SPDX-License-Identifier: Apache-2.0
// Project-level lighting bakes (ADR-0125, ADR-0134): bakes one scene of a project into its
// `.vlightmap` file and points the scene's Environment at it. Shared by `victus bake-lighting`,
// `victus new` and Studio's New Project (templates ship baked, like Unity's and Unreal's starter
// content).
#pragma once
#include "victus/core/result.hpp"
#include "victus/render/lightmap.hpp"
#include "victus/runtime/scene_lightmap.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <string>

namespace victus::jobs {
class JobSystem;
}
namespace victus::project {
class Project;
}

namespace victus::player {

// True when nothing can move the mesh at run time: no dynamic or kinematic body, character,
// nav agent, animator, script, rotator, oscillator, moving platform or sequence player on the
// entity or any parent. What `--mark-static` marks as Static Lighting.
[[nodiscard]] bool is_static_for_lighting(scene::Scene& scene, scene::Entity entity);
// Marks every visible mesh renderer for which is_static_for_lighting holds as lightmapped;
// returns how many changed.
std::uint32_t mark_static_lighting(scene::Scene& scene);

struct BakeLightingOptions {
    std::string scene;             // project-relative; empty = the startup scene
    std::string out;               // project-relative .vlightmap; empty = the Environment's, else next to the scene
    render::LightmapSettings settings{};
    bool mark_static{false};       // mark_static_lighting first
    bool dry_run{false};           // bake and report, write nothing
    jobs::JobSystem* jobs{nullptr};
};

struct BakeLightingReport {
    runtime::SceneLightmapBake bake;
    std::string scene;               // project-relative scene that was baked
    std::string out;                 // project-relative lightmap written (or that would be)
    std::uint32_t marked{0};         // renderers newly marked Static Lighting
    bool added_environment{false};   // the scene had no Environment; one was added
    bool scene_updated{false};       // the scene file was rewritten
    double seconds{0};
};

// Imports the project's assets, bakes and (unless dry_run) writes the lightmap and the scene.
[[nodiscard]] Result<BakeLightingReport> bake_project_lighting(const project::Project& project, const BakeLightingOptions& options);

// After creating a project from a template: bakes the startup scene when it has lightmapped
// renderers (the 3D templates), with template settings (128 samples, 3 bounces, 8 texels/m,
// probes). Returns false without touching anything when there is nothing to bake.
[[nodiscard]] Result<bool> bake_template_lighting(const project::Project& project, jobs::JobSystem* jobs = nullptr);

} // namespace victus::player
