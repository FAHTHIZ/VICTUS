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
//     "occlusion_strength": 0..1   // AO from metallic_roughness_texture's red channel (ORM); 0 = off
//     "responsive": 0..1           // blended only: how much TAA trusts the current frame under it (ADR-0187)
//     "triplanar": false           // world-space triplanar mapping; uv_scale = repeats per metre (ADR-0197)
//     "triplanar_sharpness": 4     // blend exponent between the three projections (1 .. 64)
//     "clearcoat": 0..1            // glossy dielectric layer over the base (ADR-0203); 0 = none
//     "clearcoat_roughness": 0.03  // the coat's own roughness
//     "sheen_color": [r, g, b]     // cloth/velvet sheen, linear (ADR-0231); black = none
//     "sheen_roughness": 0.3       // how wide the sheen rim spreads
//     "anisotropy": 0.0            // brushed metal / hair highlight stretch 0..1 (ADR-0232)
//     "anisotropy_rotation": 0.0   // radians from the mesh tangent
//     "subsurface_color": [r, g, b] // skin / wax / leaf scattering colour, linear (ADR-0233); black = none
//     "subsurface_thickness": 0.5  // 1 = opaque body, 0 = thin (light passes through)
//     "anti_tiling": 0             // 0..1 macro variation; > 0.5 adds a distant large-scale blend (ADR-0240)
//     "max_vertex_offset": 0       // metres a shader vertex() may move vertices; grows culling bounds (ADR-0245)
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
    VertexAlpha, CoverageResponse, Blend, OcclusionStrength, ShaderTextures, Responsive, Triplanar, TriplanarSharpness,
    Clearcoat, ClearcoatRoughness, SheenColor, SheenRoughness, Anisotropy, AnisotropyRotation, SubsurfaceColor,
    SubsurfaceThickness, AntiTiling, MaxVertexOffset, Count
};
inline constexpr std::size_t kMaterialFieldCount = static_cast<std::size_t>(MaterialField::Count);
[[nodiscard]] constexpr std::uint64_t field_bit(MaterialField f) noexcept { return std::uint64_t{1} << static_cast<unsigned>(f); }
static_assert(kMaterialFieldCount < 64, "MaterialAsset::overrides holds one bit per field");
inline constexpr std::uint64_t kAllMaterialFields = (std::uint64_t{1} << kMaterialFieldCount) - 1u;
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
    // World-space triplanar mapping (ADR-0197): textures are projected along the world axes and
    // blended by the normal instead of using mesh uvs; uv_scale then counts repeats per metre.
    bool triplanar{false};
    float triplanar_sharpness{4.0f};
    // Clear coat (ADR-0203): a glossy dielectric layer over the base with its own roughness
    // (lacquer, car paint, glazed ceramic). 0 = none.
    float clearcoat{0.0f};
    float clearcoat_roughness{0.03f};
    // Sheen (ADR-0231): the soft rim of cloth, velvet, felt and peach fuzz. Black = none.
    math::Vec3 sheen_color{0, 0, 0};
    float sheen_roughness{0.3f};
    // Anisotropy (ADR-0232): stretches the highlight along the tangent rotated by
    // anisotropy_rotation (radians): brushed metal, hair, satin. 0 = isotropic.
    float anisotropy{0.0f};
    float anisotropy_rotation{0.0f};
    // Subsurface scattering (ADR-0233): skin, wax, marble, leaves. Black = none.
    math::Vec3 subsurface_color{0, 0, 0};
    float subsurface_thickness{0.5f};
    // Anti-tiling (ADR-0240), 0..1: world-space macro variation of the albedo that breaks visible
    // repeats; above 0.5 distant pixels also blend in a 4x larger sample of the textures. 0 = off.
    float anti_tiling{0.0f};
    // How far (metres) the shader's vertex() function may move a vertex from the mesh (ADR-0245).
    // Culling bounds grow by this much, so displaced geometry is not culled while still on screen
    // (Unreal's Max World Position Offset Displacement). 0 = the mesh bounds; capped at 100 m.
    float max_vertex_offset{0.0f};
    TextureFilter filter{TextureFilter::Linear};
    bool unlit{false};
    bool double_sided{false};
    bool vertex_alpha_coverage_mask{false}; // vertex alpha marks where the coverage layer may settle
    float coverage_response{1.0f};          // 0..1 response to the scene's snow/dust coverage layer
    // Ambient occlusion read from the red channel of metallic_roughness_texture (glTF's packed
    // "ORM" layout: R occlusion, G roughness, B metallic), 0 = ignored .. 1 = full (ADR-0139).
    float occlusion_strength{0.0f};
    MaterialBlendMode blend{MaterialBlendMode::Opaque};
    // Blended materials have no motion vectors, so TAA and temporal upscaling blend a moving glass
    // pane or hologram with a history that does not move with it (ghosting). Responsive marks
    // where the surface covers a pixel in the TAA reactive mask: 0 = resolve from history as
    // before .. 1 = trust the current frame (up to RenderSettings::reactive_max). Ignored for
    // opaque materials (ADR-0187; FSR2/Arm ASR "reactive", Unreal "responsive AA").
    float responsive{0.0f};
    Uuid base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;
    Uuid shader; // optional surface shader (.vshader); nil = built-in PBR surface
    std::array<math::Vec4, 4> shader_params{};
    // Slots given explicitly (the "shader_params" length). A slot that is neither given nor
    // non-zero takes the default its shader declares (`uniform float wind = 0.5;`, ADR-0171).
    // A hint only: equality ignores it.
    struct ParamsSet {
        std::uint8_t count{0};
        friend constexpr bool operator==(ParamsSet, ParamsSet) noexcept { return true; }
    } shader_params_set;
    [[nodiscard]] bool shader_param_set(std::size_t slot) const noexcept {
        return slot < shader_params_set.count || (slot < shader_params.size() && shader_params[slot] != math::Vec4{});
    }
    // Textures for the shader's `uniform sampler2D` declarations, by declaration order; nil = the
    // declared default (white, black or flat normal).
    std::array<Uuid, 4> shader_textures{};
    // Instances: the parent material (nil = a standalone material) and which fields this file
    // sets. from_json() records the keys present; to_json() writes only those for instances.
    Uuid parent;
    std::uint64_t overrides{kAllMaterialFields};

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
    // Every referenced texture: the four slots above, then the shader textures (dependency walks).
    [[nodiscard]] std::array<const Uuid*, 8> all_textures() const noexcept {
        return {&base_color_texture, &metallic_roughness_texture, &normal_texture, &emissive_texture,
                &shader_textures[0], &shader_textures[1], &shader_textures[2], &shader_textures[3]};
    }
    [[nodiscard]] json::Value to_json() const;
    // Strict about types (a string where a number belongs is an error), lenient about missing
    // keys (they keep their defaults), so hand-written materials can stay short.
    [[nodiscard]] static Result<MaterialAsset> from_json(const json::Value& v);

    friend bool operator==(const MaterialAsset&, const MaterialAsset&) = default;
};

inline constexpr const char* kMaterialTextureSlotNames[4] = {"Base Color", "Metallic/Roughness", "Normal", "Emissive"};
// Names for all_textures() entries.
inline constexpr const char* kMaterialAllTextureNames[8] = {"Base Color", "Metallic/Roughness", "Normal", "Emissive",
                                                            "Shader texture 0", "Shader texture 1", "Shader texture 2", "Shader texture 3"};

// Applies `instance`'s overridden fields on top of `base` (a resolved parent). The result keeps
// the instance's parent and override mask so editors can save it back unchanged.
[[nodiscard]] MaterialAsset apply_instance(const MaterialAsset& base, const MaterialAsset& instance);

[[nodiscard]] Result<MaterialAsset> load_material_file(const fs::Path& path);
[[nodiscard]] Status save_material_file(const fs::Path& path, const MaterialAsset& material);

} // namespace victus::assets
