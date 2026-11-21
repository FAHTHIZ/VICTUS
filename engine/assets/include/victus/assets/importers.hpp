// SPDX-License-Identifier: Apache-2.0
// Source-asset importers. Importers are pure functions from source files + settings to cooked
// data, so they can run on any worker thread and be unit tested without a GPU.
#pragma once
#include <span>
#include "victus/assets/cooked.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <string>
#include <vector>

namespace victus::assets {

struct TextureImportSettings {
    bool srgb{true};
    bool mipmaps{true};
    std::uint32_t max_size{4096}; // larger sources are downsampled by whole mip levels
    bool flip_y{false};
    // Sampling for sprites, particles and materials using the texture: "nearest" keeps pixel art
    // crisp (and stops sprite-sheet cells bleeding into each other). Not baked into the pixels.
    bool nearest{false};
    // GPU block compression variants baked next to the RGBA8 mips: "none", "auto" (BC7 + ASTC
    // 4x4), "small" (BC1 + ASTC 6x6) or a '+'-separated codec list (see texture_compression.hpp).
    std::string compression{"none"};
    bool compression_thorough{false}; // slower, higher-quality encoder searches
    // > 0 (and < 1): the texture is used with an alpha test at this cutoff, so mips rescale alpha to
    // keep the source's alpha-test coverage (render::preserve_alpha_coverage; Unity's "Mip Maps
    // Preserve Coverage"). Without it, cutout foliage and fences fade away with distance.
    float alpha_coverage_cutoff{0.0f};
    [[nodiscard]] json::Value to_json() const;
    static TextureImportSettings from_json(const json::Value& v);
};

struct ModelImportSettings {
    float scale{1.0f};
    bool generate_tangents{true};    // when the source has none
    bool import_textures{true};
    std::uint32_t max_texture_size{4096};
    std::string texture_compression{"none"}; // as TextureImportSettings::compression
    // Vertex cache / overdraw / fetch order optimisation and automatic LODs (render::generate_lods).
    bool optimize_meshes{true};
    std::uint32_t lods{3};      // coarser levels per mesh (0..7); needs optimize_meshes
    float lod_error{0.05f};     // largest simplification error, relative to the mesh size
    bool import_lights{true};   // glTF KHR_lights_punctual / FBX lights -> ModelLight
    bool import_cameras{true};  // glTF / FBX cameras -> ModelCamera
    // Multiplies light intensities on import. glTF lights are photometric (lux, candela); the
    // default 1/683 turns them back into the watt-based values Blender shows (its exporter
    // multiplies by 683 lm/W), which match VICTUS light units. Use 1 for files written unitless.
    float light_intensity_scale{1.0f / 683.0f};
    [[nodiscard]] json::Value to_json() const;
    static ModelImportSettings from_json(const json::Value& v);
};

struct ImportResult {
    std::vector<fs::Path> dependencies; // extra source files read (beyond the primary file)
    std::vector<std::string> warnings;
};

inline constexpr std::uint32_t kTextureImporterVersion = 4; // v2: HDR (.hdr) sources keep linear float data; v3: BC/ASTC variants; v4: alpha_coverage_cutoff
inline constexpr std::uint32_t kModelImporterVersion = 8; // v8: node lights and cameras; v2: skins and animations; v3: COLOR_0 vertex colours; v4: BC/ASTC textures; v5: mesh optimisation + LODs; v6: coverage-preserving base colour mips for MASK materials; v7: card LODs for leaf canopies

[[nodiscard]] Result<CookedTexture> import_texture(const fs::Path& path, const TextureImportSettings& settings);
// HDR (.hdr) sources: half-float pixels (limited to max_size x max_size/2), SH9 irradiance and a
// Reinhard-tonemapped sRGB preview in `mips`.
[[nodiscard]] CookedTexture cook_hdr_texture(const render::ImageF& image, std::string name, std::uint32_t max_size);
[[nodiscard]] CookedTexture cook_texture(const render::Image& image, std::string name, bool srgb, bool mipmaps,
                                         std::uint32_t max_size, float alpha_coverage_cutoff = 0.0f);
// Adds block-compressed variants of `texture.mips` for every codec in `setting`. HDR textures
// keep only their RGBA8 preview uncompressed (their float data is what lighting uses).
[[nodiscard]] Result<void> compress_cooked_texture(CookedTexture& texture, std::string_view setting,
                                                   CompressionEffort effort = CompressionEffort::Fast);

// Imports .gltf (with external or data-URI buffers/images) and .glb. External files must
// resolve inside `sandbox_root`; remote URIs and absolute paths are rejected.
[[nodiscard]] Result<CookedModel> import_gltf(const fs::Path& path, const ModelImportSettings& settings,
                                              const fs::Path& sandbox_root, ImportResult* result = nullptr);
// Imports a self-contained glTF from memory: a .glb, or .gltf JSON whose buffers and images are
// embedded (data URIs or the GLB binary chunk). External file references fail. `name` labels
// errors and warnings. Used for engine-embedded content such as the starter characters.
[[nodiscard]] Result<CookedModel> import_gltf_memory(std::span<const std::uint8_t> bytes, std::string_view name,
                                                     const ModelImportSettings& settings,
                                                     ImportResult* result = nullptr);

// Writes a cooked model as glTF 2.0 JSON with an embedded base64 buffer: geometry, skin
// streams, material factors (no textures), nodes, skins and animations. Round-trips through
// import_gltf (with scale 1).
[[nodiscard]] Result<std::string> export_gltf_json(const CookedModel& model);
[[nodiscard]] Status export_gltf(const CookedModel& model, const fs::Path& path);

} // namespace victus::assets
