// SPDX-License-Identifier: Apache-2.0
// Starter materials (ADR-0197): five CC0 PBR texture sets (grass, rock, planks, metal, paving)
// and a CC0 HDR sky, embedded in the engine so the 3D templates start with real surfaces and
// image-based lighting instead of flat colours. Sources and licences: data/materials/LICENSE.md.
//
// The five texture sets are world-space triplanar .vmat files (uv_scale = repeats per metre), so
// they can be put on any scaled static block without stretching. Crate reuses the planks textures
// with the mesh's own UVs (one repeat per face), so the texture stays on moving bodies.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/project/project.hpp"

#include <string_view>
#include <vector>

namespace victus::project {

enum class StarterMaterial : std::uint8_t { Grass, Rock, Planks, Metal, Paving, Crate, Count };

struct StarterMaterialInfo {
    std::string_view id;            // "grass"
    std::string_view material_path; // "assets/materials/starter/grass.vmat"
    std::string_view material_uuid;
};

[[nodiscard]] const StarterMaterialInfo& starter_material(StarterMaterial m) noexcept;
// The .vmat UUID of a starter material, for MeshRendererComponent::material_asset.
[[nodiscard]] Uuid starter_material_uuid(StarterMaterial m) noexcept;

inline constexpr std::string_view kStarterSkyPath = "assets/textures/sky/partly_cloudy.hdr";
// The HDR sky's texture UUID, for EnvironmentComponent::environment_map.
[[nodiscard]] Uuid starter_sky_uuid() noexcept;

// Foliage materials (ADR-0243) for procgen::TreeComponent: vertex-coloured leaves and bark that
// sway in the wind through assets/shaders/starter/foliage_wind.vshader (a vertex function, so
// shadows move with the leaves). Material shader_params: [0] = wind direction (xyz) + bend
// strength (w), [1] = x gust speed, y leaf flutter (m), z flutter speed.
inline constexpr std::string_view kFoliageWindShaderPath = "assets/shaders/starter/foliage_wind.vshader";
[[nodiscard]] Uuid starter_leaves_uuid() noexcept;
[[nodiscard]] Uuid starter_bark_uuid() noexcept;

// Appends every starter texture, material and the sky (with their .meta files) to `files`.
void add_starter_material_files(std::vector<TemplateFile>& files);

} // namespace victus::project
