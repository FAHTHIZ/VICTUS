// SPDX-License-Identifier: Apache-2.0
// Cooked (runtime-ready) asset representations and their binary file format.
//
// A cooked file is: header (magic "VCOOK", format version, asset kind, source key, list of
// source dependencies with content hashes) followed by the kind-specific payload. The header
// alone decides whether the cache entry is up to date.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace victus::assets {

inline constexpr std::uint32_t kCookedFormatVersion = 1;

enum class AssetKind : std::uint8_t { Unknown = 0, Texture = 1, Model = 2, Scene = 3 };
[[nodiscard]] const char* to_string(AssetKind kind) noexcept;

struct CookedTexture {
    std::string name;
    bool srgb{true};
    std::vector<render::Image> mips; // mip 0 first, each RGBA8
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
};

struct CookedModel {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials; // always at least one
    std::vector<CookedTexture> textures;
    std::vector<ModelNode> nodes;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
};

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
