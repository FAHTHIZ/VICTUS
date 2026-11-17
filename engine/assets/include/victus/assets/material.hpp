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
//     "filter": "linear" | "nearest", "unlit": false, "double_sided": false,
//     "shader": "<uuid of a .vshader surface shader>" | null,
//     "shader_params": [[x, y, z, w], ...]   // up to 4, read in the shader as si.params[i]
//     "vertex_alpha": "opacity" | "coverage_mask", "coverage_response": 1,
//     "blend": "opaque" | "alpha" | "additive"   // blended materials draw after opaque, far to near
//   }
//
// Material instances name a parent and contain only the fields they override; everything else
// comes from the parent (which may itself be an instance):
//
//   { "victus_material": 1, "parent": "<uuid of a .vmat>", "base_color": [0.8, 0.1, 0.1, 1] }
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
inline constexpr std::string_view kShaderExtension = ".vshader";
inline constexpr std::size_t kMaterialShaderParams = 4;
inline constexpr int kMaterialVersion = 1;

enum class TextureFilter : std::uint8_t { Linear = 0, Nearest = 1 };
enum class MaterialBlendMode : std::uint8_t { Opaque = 0, Alpha = 1, Additive = 2 };

// Every field a material instance can override (bit index in MaterialAsset::overrides).
enum class MaterialField : std::uint8_t {
    BaseColor, BaseColorTexture, Metallic, Roughness, MetallicRoughnessTexture, NormalTexture, NormalScale, Emissive,
    EmissiveStrength, EmissiveTexture, AlphaCutoff, UvScale, UvOffset, Filter, Unlit, DoubleSided, Shader, ShaderParams,
    VertexAlpha, CoverageResponse, Blend, Count
};
inline constexpr std::size_t kMaterialFieldCount = static_cast<std::size_t>(MaterialField::Count);
[[nodiscard]] constexpr std::uint32_t field_bit(MaterialField f) noexcept { return 1u << static_cast<unsigned>(f); }
inline constexpr std::uint32_t kAllMaterialFields = (1u << kMaterialFieldCount) - 1u;
// JSON key of a field ("base_color", ...).
[[nodiscard]] const char* field_key(MaterialField f) noexcept;
inline constexpr std::size_t kMaxMaterialDepth = 16; // parent chain length limit

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
    bool vertex_alpha_coverage_mask{false}; // vertex alpha marks where the coverage layer may settle
    float coverage_response{1.0f};          // 0..1 response to the scene's snow/dust coverage layer
    MaterialBlendMode blend{MaterialBlendMode::Opaque};
    Uuid base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;
    Uuid shader; // optional surface shader (.vshader); nil = built-in PBR surface
    std::array<math::Vec4, 4> shader_params{};
    // Instances: the parent material (nil = a standalone material) and which fields this file
    // sets. from_json() records the keys present; to_json() writes only those for instances.
    Uuid parent;
    std::uint32_t overrides{kAllMaterialFields};

    [[nodiscard]] bool is_instance() const noexcept { return !parent.is_nil(); }
    [[nodiscard]] bool overrides_field(MaterialField f) const noexcept { return (overrides & field_bit(f)) != 0; }
    // Copies one field's value from `src`.
    void copy_field(const MaterialAsset& src, MaterialField f);

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

// Applies `instance`'s overridden fields on top of `base` (a resolved parent). The result keeps
// the instance's parent and override mask so editors can save it back unchanged.
[[nodiscard]] MaterialAsset apply_instance(const MaterialAsset& base, const MaterialAsset& instance);

[[nodiscard]] Result<MaterialAsset> load_material_file(const fs::Path& path);
[[nodiscard]] Status save_material_file(const fs::Path& path, const MaterialAsset& material);

} // namespace victus::assets
