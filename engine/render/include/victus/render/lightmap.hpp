// SPDX-License-Identifier: Apache-2.0
// Baked lighting (ADR-0122): lightmap UV unwrapping (xatlas) and a CPU path tracer that bakes
// indirect diffuse lighting into lightmap pages, plus the `.vlightmap` file that stores a bake.
//
// Units match the PBR shader: lightmap texels hold irradiance / pi (the value that multiplies
// kd * albedo directly, like the SH9 ambient), linear RGB, alpha = 1 where a texel is covered.
// The default mode ("indirect") bakes sky light and every bounce but leaves out the direct
// light of the bake lights on the receiver itself, so the realtime sun, its shadows and the
// local lights keep working on top of the lightmap without being counted twice.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::render {

struct LightmapSettings {
    float texels_per_unit{8.0f};       // lightmap resolution in world space (texels per metre)
    // Largest atlas side in texels. A scene that does not fit at `texels_per_unit` is unwrapped
    // again at a lower density (reported in LightmapBakeStats::texels_per_unit).
    std::uint32_t max_atlas_size{2048};
    std::uint32_t padding{2};          // texels between charts (bilinear filtering needs >= 1)
    std::uint32_t samples{128};        // hemisphere rays per texel
    std::uint32_t bounces{3};          // indirect bounces after the first hit (0 = sky + direct only)
    // Also bake the bake lights' direct light on the receiver (fully baked). Off by default:
    // realtime lights stay realtime and only their bounce light is baked.
    bool include_direct{false};
    bool denoise{true};
    std::uint32_t dilation{4};         // texels the covered area is grown into the padding
    std::uint64_t seed{0x5eed};
};

// One mesh instance of the bake scene. `receives` instances get lightmap UVs and texels; every
// instance (receiving or not) blocks rays and bounces light.
struct LightmapInstance {
    const MeshData* mesh{nullptr};
    math::Mat4 model{math::Mat4::identity()};
    math::Vec3 albedo{0.8f, 0.8f, 0.8f}; // linear base colour (times the texture when given)
    math::Vec3 emissive{0, 0, 0};         // linear emitted radiance
    const Image* albedo_texture{nullptr}; // optional sRGB RGBA8, sampled with the mesh UVs (wrap)
    bool receives{true};
};

struct LightmapLight {
    enum class Type : std::uint8_t { Directional, Point, Spot };
    Type type{Type::Directional};
    math::Vec3 position;
    math::Vec3 direction{0, -1, 0}; // direction the light travels (directional and spot)
    math::Vec3 radiance{1, 1, 1};   // colour * intensity, the shader's light radiance
    float range{10.0f};
    float cos_inner{0.94f};
    float cos_outer{0.88f};
    bool cast_shadows{true};
};

// Sky radiance for rays that leave the scene. Defaults to the renderer's analytic gradient
// (pbr.frag environment(dir, 0)); `radiance` overrides it (e.g. an environment map lookup).
struct LightmapSky {
    math::Vec3 sky{0.45f, 0.6f, 0.9f};
    math::Vec3 horizon{0.8f, 0.85f, 0.9f};
    math::Vec3 ground{0.25f, 0.22f, 0.2f};
    float intensity{1.0f};
    std::function<math::Vec3(math::Vec3)> radiance;
    [[nodiscard]] math::Vec3 evaluate(math::Vec3 dir) const;
};

struct LightmapScene {
    std::vector<LightmapInstance> instances;
    std::vector<LightmapLight> lights;
    LightmapSky sky;
};

// Lightmap layout of one receiving instance: a re-indexed copy of its mesh where vertex i is
// source vertex `xref[i]` with lightmap UV `uv[i]` (0..1 over the atlas).
struct LightmapUnwrap {
    std::vector<std::uint32_t> xref;
    std::vector<math::Vec2> uv;
    std::vector<std::uint32_t> indices;
    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
};

struct LightmapBakeStats {
    std::uint32_t width{0}, height{0}; // atlas size in texels
    float texels_per_unit{0};          // density actually used
    std::uint64_t texels{0};          // covered texels
    std::uint64_t invalid_texels{0};  // covered texels inside geometry, filled by dilation
    std::uint64_t triangles{0};       // triangles in the ray tracing scene
    std::uint64_t rays{0};
    double unwrap_ms{0}, trace_ms{0}, post_ms{0};
};

struct LightmapBake {
    std::vector<LightmapUnwrap> instances; // one per scene instance (empty when it does not receive)
    ImageF atlas;                          // RGB irradiance / pi, A coverage
    LightmapBakeStats stats;
};

// Unwraps every receiving instance (in world space, so texel density is uniform) into one
// atlas. Deterministic for the same input.
struct LightmapLayout {
    std::vector<LightmapUnwrap> instances; // one per scene instance (empty when it does not receive)
    std::uint32_t width{0}, height{0};
    float texels_per_unit{0};
};
[[nodiscard]] Result<LightmapLayout> unwrap_lightmaps(const LightmapScene& scene, const LightmapSettings& settings);

// Unwraps and bakes. `jobs` (optional) spreads texels over its workers; the result does not
// depend on the number of threads.
[[nodiscard]] Result<LightmapBake> bake_lightmaps(const LightmapScene& scene, const LightmapSettings& settings = {},
                                                  jobs::JobSystem* jobs = nullptr);

// Bilinear lookup of an atlas (uv 0..1, clamped), for tests and tools.
[[nodiscard]] math::Vec4 sample_lightmap(const ImageF& atlas, math::Vec2 uv) noexcept;

// Re-indexes `mesh` with an unwrap: vertices are copied through `xref`, indices replaced, LODs,
// skin and morph targets dropped (lightmapped meshes are static and draw LOD 0).
[[nodiscard]] MeshData apply_lightmap_unwrap(const MeshData& mesh, const LightmapUnwrap& unwrap);

// Stable fingerprint of a mesh's geometry (positions, normals, indices) so a stored bake can
// tell whether it still matches the mesh it was made for.
[[nodiscard]] std::uint64_t lightmap_mesh_fingerprint(const MeshData& mesh) noexcept;

// --- .vlightmap files -----------------------------------------------------------------------
// Little-endian binary: header, the atlas (RGBA16F), then one entry per lightmapped entity. Every
// count and index is validated on decode; a damaged file is an error, never a crash.
struct LightmapFileEntry {
    Uuid entity;
    std::uint64_t mesh_fingerprint{0};
    LightmapUnwrap unwrap;
};
struct LightmapFile {
    ImageF atlas;
    std::vector<LightmapFileEntry> entries;
    [[nodiscard]] const LightmapFileEntry* find(const Uuid& entity) const noexcept;
};
inline constexpr std::uint32_t kLightmapFileVersion = 1;
[[nodiscard]] std::vector<std::uint8_t> encode_lightmap_file(const LightmapFile& file);
[[nodiscard]] Result<LightmapFile> decode_lightmap_file(std::span<const std::uint8_t> bytes);

} // namespace victus::render
