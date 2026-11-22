// SPDX-License-Identifier: Apache-2.0
// Starter materials (ADR-0197): five CC0 PBR texture sets (grass, rock, planks, metal, paving)
// and a CC0 HDR sky, embedded in the engine so the 3D templates start with real surfaces and
// image-based lighting instead of flat colours. Sources and licences: data/materials/LICENSE.md.
//
// Every material is a world-space triplanar .vmat (uv_scale = repeats per metre), so it can be
// put on any scaled primitive block without stretching.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/project/project.hpp"

#include <string_view>
#include <vector>

namespace victus::project {

enum class StarterMaterial : std::uint8_t { Grass, Rock, Planks, Metal, Paving, Count };

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

// Appends every starter texture, material and the sky (with their .meta files) to `files`.
void add_starter_material_files(std::vector<TemplateFile>& files);

} // namespace victus::project
