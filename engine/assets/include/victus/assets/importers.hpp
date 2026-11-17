// SPDX-License-Identifier: Apache-2.0
// Source-asset importers. Importers are pure functions from source files + settings to cooked
// data, so they can run on any worker thread and be unit tested without a GPU.
#pragma once
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
    [[nodiscard]] json::Value to_json() const;
    static TextureImportSettings from_json(const json::Value& v);
};

struct ModelImportSettings {
    float scale{1.0f};
    bool generate_tangents{true};    // when the source has none
    bool import_textures{true};
    std::uint32_t max_texture_size{4096};
    [[nodiscard]] json::Value to_json() const;
    static ModelImportSettings from_json(const json::Value& v);
};

struct ImportResult {
    std::vector<fs::Path> dependencies; // extra source files read (beyond the primary file)
    std::vector<std::string> warnings;
};

inline constexpr std::uint32_t kTextureImporterVersion = 1;
inline constexpr std::uint32_t kModelImporterVersion = 3; // v2: skins and animations; v3: COLOR_0 vertex colours

[[nodiscard]] Result<CookedTexture> import_texture(const fs::Path& path, const TextureImportSettings& settings);
[[nodiscard]] CookedTexture cook_texture(const render::Image& image, std::string name, bool srgb, bool mipmaps,
                                         std::uint32_t max_size);

// Imports .gltf (with external or data-URI buffers/images) and .glb. External files must
// resolve inside `sandbox_root`; remote URIs and absolute paths are rejected.
[[nodiscard]] Result<CookedModel> import_gltf(const fs::Path& path, const ModelImportSettings& settings,
                                              const fs::Path& sandbox_root, ImportResult* result = nullptr);

// Writes a cooked model as glTF 2.0 JSON with an embedded base64 buffer: geometry, skin
// streams, material factors (no textures), nodes, skins and animations. Round-trips through
// import_gltf (with scale 1).
[[nodiscard]] Result<std::string> export_gltf_json(const CookedModel& model);
[[nodiscard]] Status export_gltf(const CookedModel& model, const fs::Path& path);

} // namespace victus::assets
