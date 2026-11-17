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
#include <span>
#include <string>
#include <vector>

namespace victus::assets {

// v2 (Slice 12): mesh skin streams, node skins, skins and animation clips.
// v3: Vertex3D gained an RGBA8 colour (52 bytes); v4: HDR textures (half-float equirect + SH9);
// v5: block-compressed texture variants (BC/ASTC, ADR-0047); v6: mesh LODs (ADR-0049).
inline constexpr std::uint32_t kCookedFormatVersion = 6;

enum class AssetKind : std::uint8_t { Unknown = 0, Texture = 1, Model = 2, Scene = 3, Script = 4, Audio = 5, Prefab = 6, Material = 7, Shader = 8, SpriteSheet = 9, Tileset = 10, Terrain = 11, BehaviorTree = 12, Sequence = 13 };
[[nodiscard]] const char* to_string(AssetKind kind) noexcept;

struct CookedTexture {
    std::string name;
    bool srgb{true};
    std::vector<render::Image> mips; // mip 0 first, each RGBA8
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
};

struct ModelMaterial {
    std::string name;
    math::Color base_color{1, 1, 1, 1};
    math::Color emissive{0, 0, 0, 1};
    float emissive_strength{1.0f};
    float metallic{1.0f};
    float roughness{1.0f};
    float normal_scale{1.0f};
    float alpha_cutoff{0.0f};
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
};

// A glTF skin: joint nodes and their inverse bind matrices (model space -> joint space at bind).
// Vertex joint indices of meshes bound to the skin index into `joints`.
struct ModelSkin {
    std::string name;
    std::vector<std::uint32_t> joints; // indices into CookedModel::nodes
    std::vector<math::Mat4> inverse_bind;
};

struct CookedModel {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials; // always at least one
    std::vector<CookedTexture> textures;
    std::vector<ModelNode> nodes;
    std::vector<ModelSkin> skins;
    std::vector<animation::Clip> animations; // channel node indices refer to `nodes`
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

} // namespace victus::assets
