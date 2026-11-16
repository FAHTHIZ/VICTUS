// SPDX-License-Identifier: Apache-2.0
// Material assets (.vmat): a small JSON document describing a PBR surface — colours, scalar
// parameters, texture references (by asset UUID, so renaming a texture never breaks a material),
// UV tiling/offset and sampling. Materials are used directly from source (nothing to cook) and are
// shared by every MeshRenderer that references them.
//
//   {
//     "victus_material": 1,
//     "base_color": [1, 1, 1, 1], "base_color_texture": "<uuid>",
//     "metallic": 0, "roughness": 0.5, "metallic_roughness_texture": null,
//     "normal_texture": null, "normal_scale": 1,
//     "emissive": [0, 0, 0], "emissive_strength": 1, "emissive_texture": null,
//     "alpha_cutoff": 0, "uv_scale": [1, 1], "uv_offset": [0, 0],
//     "filter": "linear" | "nearest", "unlit": false, "double_sided": false
//   }
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"
#include "victus/core/math.hpp"

#include <array>
#include <string_view>

namespace victus::assets {

inline constexpr std::string_view kMaterialExtension = ".vmat";
inline constexpr int kMaterialVersion = 1;

enum class TextureFilter : std::uint8_t { Linear = 0, Nearest = 1 };

struct MaterialAsset {
    math::Color base_color{1, 1, 1, 1}; // linear
    float metallic{0.0f};
    float roughness{0.5f};
    float normal_scale{1.0f};
    math::Color emissive{0, 0, 0, 1}; // linear rgb
    float emissive_strength{1.0f};
    float alpha_cutoff{0.0f}; // >0 enables alpha testing
    math::Vec2 uv_scale{1, 1};
    math::Vec2 uv_offset{0, 0};
    TextureFilter filter{TextureFilter::Linear};
    bool unlit{false};
    bool double_sided{false};
    Uuid base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;

    // Texture slots in a fixed order (base colour, metallic-roughness, normal, emissive), for
    // tools that iterate references (export validation, the material editor).
    [[nodiscard]] std::array<const Uuid*, 4> textures() const noexcept {
        return {&base_color_texture, &metallic_roughness_texture, &normal_texture, &emissive_texture};
    }
    [[nodiscard]] std::array<Uuid*, 4> textures() noexcept {
        return {&base_color_texture, &metallic_roughness_texture, &normal_texture, &emissive_texture};
    }
    [[nodiscard]] json::Value to_json() const;
    // Strict about types (a string where a number belongs is an error), lenient about missing
    // keys (they keep their defaults), so hand-written materials can stay short.
    [[nodiscard]] static Result<MaterialAsset> from_json(const json::Value& v);

    friend bool operator==(const MaterialAsset&, const MaterialAsset&) = default;
};

inline constexpr const char* kMaterialTextureSlotNames[4] = {"Base Color", "Metallic/Roughness", "Normal", "Emissive"};

[[nodiscard]] Result<MaterialAsset> load_material_file(const fs::Path& path);
[[nodiscard]] Status save_material_file(const fs::Path& path, const MaterialAsset& material);

} // namespace victus::assets
