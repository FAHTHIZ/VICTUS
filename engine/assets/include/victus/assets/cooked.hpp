// SPDX-License-Identifier: Apache-2.0
// Cooked (runtime-ready) asset representations and their binary file format.
//
// A cooked file is: header (magic "VCOOK", format version, asset kind, source key, list of
// source dependencies with content hashes) followed by the kind-specific payload. The header
// alone decides whether the cache entry is up to date.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/core/math.hpp"
#include "victus/assets/texture_compression.hpp"
#include "victus/core/result.hpp"
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace victus::assets {

// v2 (Slice 12): mesh skin streams, node skins, skins and animation clips.
// v3: Vertex3D gained an RGBA8 colour (52 bytes); v4: HDR textures (half-float equirect + SH9);
// v5: block-compressed texture variants (BC/ASTC, ADR-0047); v6: mesh LODs (ADR-0049).
// v9: RGBA8 mip levels may be stored without pixels when a compressed variant covers them
// (mobile export profile, ADR-0149).
inline constexpr std::uint32_t kCookedFormatVersion = 15; // 7: morph targets, weight channels; 8: material occlusion; 10: node lights and cameras; 11: clear coat; 12: corrective shapes; 13: sheen; 14: anisotropy; 15: subsurface

enum class AssetKind : std::uint8_t { Unknown = 0, Texture = 1, Model = 2, Scene = 3, Script = 4, Audio = 5, Prefab = 6, Material = 7, Shader = 8, SpriteSheet = 9, Tileset = 10, Terrain = 11, BehaviorTree = 12, Sequence = 13, StringTable = 14, AnimController = 15 };
[[nodiscard]] const char* to_string(AssetKind kind) noexcept;

struct CookedTexture {
    std::string name;
    bool srgb{true};
    // Mip 0 first, each RGBA8. A level's `pixels` may be empty (width/height still set) in a
    // mobile export when `compressed` holds it: the runtime then samples the compressed variant
    // or, on a device that cannot, decodes it on the CPU (see has_rgba8 / rgba8_level).
    std::vector<render::Image> mips;
    // HDR sources (.hdr): linear radiance as RGBA16F (mips above hold a tonemapped LDR preview
    // so the texture still works anywhere an ordinary texture does), plus the SH9 irradiance of
    // the image when it is used as an equirect environment map.
    bool hdr{false};
    std::uint32_t hdr_width{0}, hdr_height{0};
    std::vector<std::uint16_t> hdr_pixels; // RGBA half floats, row-major, top row first
    std::array<math::Vec3, 9> sh{};
    // GPU block-compressed copies of `mips` (same level count and sizes) in preference order.
    // The runtime uploads the first one the device samples and otherwise uses the RGBA8 mips.
    std::vector<CompressedMips> compressed;
    [[nodiscard]] render::ImageF hdr_image() const; // decoded to RGBA32F
    // True when every RGBA8 level carries its pixels.
    [[nodiscard]] bool has_rgba8() const noexcept;
};

// RGBA8 pixels of one level: the stored mip, or a CPU decode of the first compressed variant
// when the export stripped it. Fails only for a texture that has neither.
[[nodiscard]] Result<render::Image> rgba8_level(const CookedTexture& texture, std::size_t level);


struct ModelMaterial {
    std::string name;
    math::Color base_color{1, 1, 1, 1};
    math::Color emissive{0, 0, 0, 1};
    float emissive_strength{1.0f};
    float metallic{1.0f};
    float roughness{1.0f};
    float normal_scale{1.0f};
    float alpha_cutoff{0.0f};
    // glTF occlusionTexture strength; the occlusion is packed into the red channel of
    // metallic_roughness_texture at import (ADR-0139). 0 = none.
    float occlusion_strength{0.0f};
    // KHR_materials_clearcoat factors (ADR-0203); 0 = no coat. Coat textures are not imported.
    float clearcoat{0.0f};
    float clearcoat_roughness{0.0f};
    // KHR_materials_sheen factors (ADR-0231); black = no sheen. Sheen textures are not imported.
    float sheen_color[3]{0.0f, 0.0f, 0.0f};
    float sheen_roughness{0.0f};
    // KHR_materials_anisotropy factors (ADR-0232); 0 = isotropic. The direction texture is not imported.
    float anisotropy{0.0f};
    float anisotropy_rotation{0.0f};
    // Subsurface (ADR-0233) from KHR_materials_diffuse_transmission (factor x colour); black = none.
    float subsurface_color[3]{0.0f, 0.0f, 0.0f};
    float subsurface_thickness{0.5f};
    bool double_sided{false};
    bool unlit{false};
    // Indices into CookedModel::textures, -1 when absent.
    std::int32_t base_color_texture{-1}, metallic_roughness_texture{-1}, normal_texture{-1}, emissive_texture{-1};
};

struct ModelMesh {
    std::string name;
    render::MeshData data;
    std::uint32_t material{0};
};

struct ModelNode {
    std::string name;
    std::int32_t parent{-1}; // index into nodes; parents always precede children
    math::Vec3 translation;
    math::Quat rotation;
    math::Vec3 scale{1, 1, 1};
    std::vector<std::uint32_t> meshes; // indices into CookedModel::meshes
    std::int32_t skin{-1};             // index into CookedModel::skins for skinned meshes
    std::vector<float> morph_weights;  // glTF node.weights: per-node default morph weights (else the mesh's)
    std::int32_t light{-1};            // index into CookedModel::lights
    std::int32_t camera{-1};           // index into CookedModel::cameras
};

// A punctual light (glTF KHR_lights_punctual, FBX light). Lights shine along the node's -Z axis.
struct ModelLight {
    enum class Type : std::uint8_t { Directional, Point, Spot };
    std::string name;
    Type type{Type::Point};
    math::Color color{1, 1, 1, 1}; // linear
    float intensity{1.0f};         // VICTUS units (source intensity x ModelImportSettings::light_intensity_scale)
    float range{0.0f};             // metres after import scale; 0 = unlimited in the source
    float inner_cone{0.0f};        // spot half-angles in radians
    float outer_cone{0.7853982f};
};

// A camera looking along the node's -Z axis (glTF camera, FBX camera).
struct ModelCamera {
    std::string name;
    bool orthographic{false};
    float yfov{0.8f};   // perspective vertical field of view, radians
    float ymag{1.0f};   // orthographic half height, metres
    float aspect{0.0f}; // 0 = viewport aspect
    float znear{0.1f};
    float zfar{0.0f};   // 0 = infinite
};

// A glTF skin: joint nodes and their inverse bind matrices (model space -> joint space at bind).
// Vertex joint indices of meshes bound to the skin index into `joints`.
// A corrective shape (ADR-0220): the morph target `morph` of every mesh skinned by the skin is
// weighted by how far `joint` has turned about `axis` (in the joint's rest frame) from its rest
// rotation: rising linearly from 0 at `start` to 1 at `end`, then either held at 1 (`release` ==
// `end`) or falling back to 0 at `release` (radians, signed; end < start drives the negative
// direction). In-between shapes use the falling side so neighbouring keys cross-fade. The
// renderer evaluates drivers each frame from the posed skeleton and adds them to the mesh's morph
// weights, so they follow clips, IK and Studio poses alike.
struct ModelCorrective {
    std::string morph;
    std::uint32_t joint{0}; // skin joint index
    math::Vec3 axis{1, 0, 0};
    float start{0.0f}, end{1.5707964f}, release{1.5707964f};
};

struct ModelSkin {
    std::string name;
    std::vector<std::uint32_t> joints; // indices into CookedModel::nodes
    std::vector<math::Mat4> inverse_bind;
    std::vector<ModelCorrective> correctives;
};

struct CookedModel {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials; // always at least one
    std::vector<CookedTexture> textures;
    std::vector<ModelNode> nodes;
    std::vector<ModelSkin> skins;
    std::vector<animation::Clip> animations; // channel node indices refer to `nodes`
    std::vector<ModelLight> lights;
    std::vector<ModelCamera> cameras;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
};

// Validates cross references of skins, node skins, skin streams and clips (used by the
// importer and by deserialize_model).
[[nodiscard]] Status validate_model(const CookedModel& model);
// Node graph (names, parents, rest pose) and clips of a model for the animation system.
[[nodiscard]] animation::AnimationSet make_animation_set(const CookedModel& model);

struct SourceDependency {
    std::string path; // project-relative, '/' separated
    std::uint64_t hash{0};
};

struct CookedHeader {
    std::uint32_t version{kCookedFormatVersion};
    AssetKind kind{AssetKind::Unknown};
    std::uint64_t key{0}; // hash of primary source bytes + importer id/version + settings
    std::vector<SourceDependency> dependencies;
};

[[nodiscard]] std::vector<std::uint8_t> serialize(const CookedHeader& header, const CookedTexture& texture);
[[nodiscard]] std::vector<std::uint8_t> serialize(const CookedHeader& header, const CookedModel& model);
[[nodiscard]] Result<CookedHeader> read_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<CookedTexture> deserialize_texture(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<CookedModel> deserialize_model(std::span<const std::uint8_t> bytes);
// Picks which compressed variants of each texture to keep: the first variant `keep` accepts
// (in cooked order, the order upload_cooked_texture tries them) is kept and the others are
// skipped without copying. A texture with no accepted variant keeps all of them, so it can still
// be decoded on the CPU. RGBA8 levels are always kept. A desktop GPU then never holds the ASTC
// copy of a character's textures in memory, and a phone never holds the BC7 copy.
using VariantFilter = std::function<bool(TextureCodec codec, bool srgb)>;
[[nodiscard]] Result<CookedModel> deserialize_model(std::span<const std::uint8_t> bytes, const VariantFilter& keep);
// Nodes, skins, clips and mesh morph names/weights of a cooked model, skipping texture pixels and
// mesh vertex/index data without copying them: everything make_animation_set reads. Meshes come
// back with empty geometry, textures are left empty and materials are kept.
[[nodiscard]] Result<CookedModel> deserialize_model_skeleton(std::span<const std::uint8_t> bytes);
// A cooked model holding only what deserialize_model_skeleton returns (no textures, no mesh
// geometry, material texture indices -1), for export: a game reads its animation data from this
// small pack entry instead of decompressing the whole model (`.vskel`, AssetDatabase).
[[nodiscard]] Result<std::vector<std::uint8_t>> make_skeleton_sidecar(std::span<const std::uint8_t> model_bytes);
// Everything except textures (collision meshes, validation): `textures` is left empty and every
// material texture index is -1.
[[nodiscard]] Result<CookedModel> deserialize_model_geometry(std::span<const std::uint8_t> bytes);
// One embedded texture of a cooked model, without decoding the meshes (mip re-streaming).
[[nodiscard]] Result<CookedTexture> deserialize_model_texture(std::span<const std::uint8_t> bytes, std::uint32_t index);
[[nodiscard]] Result<CookedTexture> deserialize_model_texture(std::span<const std::uint8_t> bytes, std::uint32_t index,
                                                         const VariantFilter& keep);

} // namespace victus::assets
