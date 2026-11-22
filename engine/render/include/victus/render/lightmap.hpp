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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <optional>
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
    // Receivers with identical meshes whose transforms differ only by rotation and translation
    // share one unwrap at different places in the atlas (ADR-0135, Unity's lightmapScaleOffset):
    // their lightmap uvs differ by a constant offset, so the renderer instances them again.
    bool share_uvs{true};
    std::uint32_t samples{128};        // hemisphere rays per texel
    std::uint32_t bounces{3};          // indirect bounces after the first hit (0 = sky + direct only)
    // Also bake the bake lights' direct light on the receiver (fully baked). Off by default:
    // realtime lights stay realtime and only their bounce light is baked.
    bool include_direct{false};
    bool denoise{true};
    // Directional lightmap (ADR-0130): also bake the dominant direction of the incoming light so
    // normal maps and per-pixel normals still shade baked light (LightmapBake::direction).
    bool directional{true};
    // Baked sun shadowmask (ADR-0131): the visibility of the first shadow-casting directional
    // light is stored in the atlas alpha, so static geometry keeps sun shadows beyond the realtime
    // shadow distance at no per-frame cost. `sun_angular_radius` (radians) softens its edges.
    bool shadowmask{true};
    std::uint32_t shadowmask_samples{32};
    float sun_angular_radius{0.01f};
    std::uint32_t dilation{4};         // texels the covered area is grown into the padding
    // Baked light probes (ADR-0133): an SH9 grid over the scene traced with the same paths as the
    // lightmap, so dynamic and non-lightmapped objects receive the same baked sky and bounce light
    // as the static scenery around them. `probe_bounds` (invalid = the scene's bounds, padded)
    // limits where probes go; spacing is in metres (half of it vertically) and grows when the box
    // needs more than 32 x 16 x 32 probes. Probes inside geometry are marked invalid and never sampled.
    bool probes{true};
    float probe_spacing{2.0f};
    std::uint32_t probe_samples{256};
    bool probe_dilation{true}; // invalid probes take their valid neighbours' average (Unity APV dilation)
    math::Aabb probe_bounds{};
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
    enum class Type : std::uint8_t { Directional, Point, Spot, Area };
    Type type{Type::Directional};
    math::Vec3 position;
    math::Vec3 direction{0, -1, 0}; // direction the light travels (directional and spot)
    math::Vec3 radiance{1, 1, 1};   // colour * intensity, the shader's light radiance
    float range{10.0f};
    float cos_inner{0.94f};
    float cos_outer{0.88f};
    // Area (ADR-0194): a one-sided width x height rectangle at `position` emitting along
    // `direction`, width along `right`. Baked as a grid of shadowed sample points (4 to 8 per side, one per 12.5 cm)
    // with Lambert emission, so it casts soft shadows; same brightness as the real-time light.
    math::Vec3 right{1, 0, 0};
    float width{1.0f}, height{1.0f};
    bool cast_shadows{true};
    // Baked light (Unity's Baked mode): its direct light is baked into receivers and probes even
    // without `include_direct`; the renderer skips it while the lightmap is loaded.
    bool baked{false};
    // Spot cookie (ADR-0195): sRGB RGBA8 image projected through the cone like the real-time
    // light (top along cross(right, direction)); its colour multiplies the baked light. Null = none.
    std::shared_ptr<const Image> cookie;
};

// Linear colour a spot light's cookie passes towards `to_surface` (surface - light position),
// bilinear like pbr.frag; white without a cookie, black behind the light.
[[nodiscard]] math::Vec3 spot_cookie_tint(const LightmapLight& light, math::Vec3 to_surface) noexcept;

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

// Baked light probes (ADR-0133). Probes sit on a regular grid inside `bounds` (corners included,
// the IrradianceGrid layout of irradiance_volume.hpp: index = (y * nz + z) * nx + x). Each holds
// SH9 irradiance / pi pre-convolved with the cosine lobe (SphericalHarmonics9's convention), so
// evaluating it for a normal gives the value that multiplies kd * albedo, like a lightmap texel.
struct LightmapProbes {
    math::Aabb bounds{};
    std::array<std::uint32_t, 3> counts{}; // probes per axis, 2..32 (y 2..16); all zero = no probes
    std::vector<std::array<math::Vec3, 9>> sh;
    std::vector<std::uint8_t> valid; // 0 = inside geometry and not dilated (sampling skips it)
    // Probe sun occlusion (ADR-0136): visibility (0..1) of the shadowmask light at each probe;
    // empty when the bake has no shadowmask light. Dynamic objects take it past the realtime
    // shadow distance, as lightmapped surfaces take the shadowmask.
    std::vector<float> sun;
    [[nodiscard]] bool empty() const noexcept { return sh.empty(); }
    [[nodiscard]] std::uint32_t probe_count() const noexcept { return counts[0] * counts[1] * counts[2]; }
    [[nodiscard]] std::uint32_t valid_count() const noexcept;
    // CPU reference of the shader lookup (trilinear over valid probes, coefficients first);
    // `fallback` outside the box or where no surrounding probe is valid.
    [[nodiscard]] math::Vec3 sample(math::Vec3 p, math::Vec3 n, math::Vec3 fallback) const noexcept;
    // Trilinear sun visibility over valid probes (1 outside the box, without data or valid probes).
    [[nodiscard]] float sample_sun(math::Vec3 p) const noexcept;
};

struct LightmapBakeStats {
    std::uint32_t width{0}, height{0}; // atlas size in texels
    float texels_per_unit{0};          // density actually used
    std::uint64_t texels{0};          // covered texels
    std::uint64_t invalid_texels{0};  // covered texels inside geometry, filled by dilation
    std::uint64_t triangles{0};       // triangles in the ray tracing scene
    std::uint64_t rays{0};
    std::uint32_t probes{0}, invalid_probes{0}; // baked light probes (ADR-0133), and those inside geometry
    std::uint32_t dilated_probes{0};            // invalid probes filled from valid neighbours
    double unwrap_ms{0}, trace_ms{0}, post_ms{0}, probe_ms{0};
};

// Directional lightmap texels (ADR-0130, the encoding Unity uses): RGB = dominant incoming light
// direction * 0.5 + 0.5 in world space, its length the directionality (1 = all light from one
// direction, 0 = uniform); A = the rebalancing term 0.5 * dot(n, d) + 0.5 at the baked normal,
// always >= 0.5 on covered texels, 0 where there is no data. The shader scales the irradiance by
// (0.5 * dot(N, d) + 0.5) / A, which is exactly 1 for the normal the texel was baked with.
struct LightmapBake {
    std::vector<LightmapUnwrap> instances; // one per scene instance (empty when it does not receive)
    // RGB irradiance / pi; A the baked sun visibility (0 shadowed .. 1 lit) when
    // shadowmask_direction is set, otherwise coverage (1 covered, 0.5 dilated, 0 empty).
    ImageF atlas;
    // Direction (travel) of the light the shadowmask was baked for; zero when there is none.
    math::Vec3 shadowmask_direction{0, 0, 0};
    Image direction;                       // RGBA8, same size as atlas; empty unless settings.directional
    LightmapProbes probes;                 // empty unless settings.probes
    LightmapBakeStats stats;
};

// Bakes only the light probes of a scene (bake_lightmaps does this too when settings.probes is
// set). Fails when the scene has no geometry and no probe_bounds.
[[nodiscard]] Result<LightmapProbes> bake_light_probes(const LightmapScene& scene, const LightmapSettings& settings = {},
                                                       jobs::JobSystem* jobs = nullptr, LightmapBakeStats* stats = nullptr);

// Irradiance-scale the shader applies for a directional texel (`texel` is the RGBA8 value
// normalised to 0..1) and a shading normal; 1 when the texel has no direction.
[[nodiscard]] float lightmap_directional_scale(math::Vec4 texel, math::Vec3 normal) noexcept;

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

// When `unwrap` is `base` moved within the atlas (same vertices and triangles, every uv shifted
// by one constant within `tolerance` atlas units), returns that shift: such receivers draw from
// one mesh with a per-instance uv offset (ADR-0135). nullopt otherwise.
[[nodiscard]] std::optional<math::Vec2> lightmap_uv_offset(const LightmapUnwrap& base, const LightmapUnwrap& unwrap,
                                                           float tolerance = 1e-5f) noexcept;

// Stable fingerprint of a mesh's geometry (positions, normals, indices) so a stored bake can
// tell whether it still matches the mesh it was made for.
[[nodiscard]] std::uint64_t lightmap_mesh_fingerprint(const MeshData& mesh) noexcept;

// --- .vlightmap files -----------------------------------------------------------------------
// Little-endian binary: header, the atlas (RGBA16F), the directional atlas (RGBA8, version 2 when
// flagged), the light probes (version 3 when flagged: bounds, counts, then per probe a validity
// byte and 27 half floats), then one entry per lightmapped entity. Versions 1 and 2 still decode. Every
// count and index is validated on decode; a damaged file is an error, never a crash.
struct LightmapFileEntry {
    Uuid entity;
    std::uint64_t mesh_fingerprint{0};
    LightmapUnwrap unwrap;
};
struct LightmapFile {
    ImageF atlas;
    Image direction; // optional directional texels (version 2), same size as the atlas
    math::Vec3 shadowmask_direction{0, 0, 0}; // version 2: atlas A is a sun shadowmask for this light
    LightmapProbes probes;                    // version 3: baked light probes for dynamic objects
    std::vector<LightmapFileEntry> entries;
    [[nodiscard]] const LightmapFileEntry* find(const Uuid& entity) const noexcept;
};
inline constexpr std::uint32_t kLightmapFileVersion = 4;
[[nodiscard]] std::vector<std::uint8_t> encode_lightmap_file(const LightmapFile& file);
[[nodiscard]] Result<LightmapFile> decode_lightmap_file(std::span<const std::uint8_t> bytes);

} // namespace victus::render
