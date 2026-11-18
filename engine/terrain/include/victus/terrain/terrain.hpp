// SPDX-License-Identifier: Apache-2.0
// Heightmap terrain (ADR-0063). A `.vterrain` asset stores a square grid of normalized heights
// and per-sample splat weights for four material layers; a Terrain component places it in the
// scene with a world size, layer textures and collision settings.
//
// The terrain is split into chunks of kChunkQuads x kChunkQuads quads. Each chunk is one mesh
// whose coarser levels of detail (every 2nd, 4th ... sample) are index lists over the same
// vertices, with skirts hanging below every edge so neighbouring chunks drawn at different
// levels never show cracks. The renderer picks a level per chunk from its projected geometric
// error, like any other mesh with LODs; chunks are culled individually (frustum and GPU
// occlusion). Collision is a Jolt height field built from the same samples.
//
// Local space: the terrain spans x in [-size.x/2, size.x/2], z in [-size.z/2, size.z/2]; a
// sample with normalized height h sits at y = h * size.y. Sample (0, 0) is the -x/-z corner,
// rows advance along +z. The entity's translation and rotation place the terrain; its scale is
// ignored (use `size`).
//
// .vterrain (binary, little endian):
//   "VTRN"  u32 version (1)  u32 resolution  u32 flags (0)
//   u16 heights[resolution^2]      normalized height * 65535, row-major
//   u32 splat[resolution^2]        RGBA8 layer weights, layer 0 in the lowest byte
//   u64 FNV-1a 64 of every preceding byte
#pragma once

#include "victus/core/hash.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::terrain {

inline constexpr std::string_view kTerrainExtension = ".vterrain";
inline constexpr std::uint32_t kTerrainVersion = 1;
inline constexpr std::uint32_t kChunkQuads = 32;      // quads per chunk side
inline constexpr std::uint32_t kMinResolution = 33;   // samples per side: 2^n + 1, 33 .. 4097
inline constexpr std::uint32_t kMaxResolution = 4097;
inline constexpr std::uint32_t kLayerCount = 4;

[[nodiscard]] bool valid_resolution(std::uint32_t resolution) noexcept;

// ---- data ----------------------------------------------------------------------------------

struct Heightfield {
    std::uint32_t resolution{0};
    std::vector<float> heights;          // normalized 0..1, resolution^2, row-major (z rows)
    std::vector<std::uint32_t> splat;    // RGBA8 weights per sample (layer 0 = lowest byte)
    // Bumped on every edit, overall and for each chunk whose geometry the edit touched (runtime
    // only). Every TerrainSystem compares them with what it built, so several scenes (edit and
    // play copies) can share one heightfield and all follow its edits.
    std::uint64_t revision{0};
    std::vector<std::uint64_t> chunk_revisions; // chunks_per_side^2, row-major
    [[nodiscard]] bool empty() const noexcept { return resolution == 0; }
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t z) const noexcept {
        return static_cast<std::size_t>(z) * resolution + x;
    }
    [[nodiscard]] float height(std::int32_t x, std::int32_t z) const noexcept; // clamped to the grid
    // Bilinear height at normalized coordinates (u along x, v along z, 0..1, clamped).
    [[nodiscard]] float sample(float u, float v) const noexcept;
    // Bilinear layer weights at normalized coordinates, normalized to sum to 1.
    [[nodiscard]] math::Vec4 weights(float u, float v) const noexcept;
    [[nodiscard]] std::uint32_t chunks_per_side() const noexcept { return resolution > 1 ? (resolution - 1) / kChunkQuads : 0; }
    // Records an edit of the inclusive sample rectangle: bumps the revision and that of every
    // chunk whose vertices (or normals, one sample further) the rectangle reaches.
    void touch(std::int32_t x0, std::int32_t z0, std::int32_t x1, std::int32_t z1) noexcept;
    void touch_all() noexcept;
};

// Packs four weights (0..1, normalized) into a splat sample and back.
[[nodiscard]] std::uint32_t pack_weights(math::Vec4 w) noexcept;
[[nodiscard]] math::Vec4 unpack_weights(std::uint32_t packed) noexcept;

// A flat heightfield at `height` painted entirely with layer 0. Fails for invalid resolutions.
[[nodiscard]] Result<Heightfield> make_flat(std::uint32_t resolution, float height = 0.0f);

[[nodiscard]] std::vector<std::uint8_t> serialize(const Heightfield& hf);
// Validates the header, size and checksum; untrusted input is safe to pass.
[[nodiscard]] Result<Heightfield> deserialize(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<Heightfield> load_file(const std::filesystem::path& path);
[[nodiscard]] Status save_file(const std::filesystem::path& path, const Heightfield& hf); // atomic

// Deterministic fractal (fBm value noise) hills, written over the whole field.
struct GenerateSettings {
    std::uint64_t seed{1};
    std::uint32_t octaves{5};
    float frequency{3.0f};     // features across the terrain at the first octave
    float persistence{0.5f};   // amplitude ratio between octaves
    float base{0.15f};         // normalized height added everywhere
    float amplitude{0.6f};     // normalized height range of the noise
    float ridge{0.0f};         // 0 = rolling hills .. 1 = sharp ridges
};
void generate(Heightfield& hf, const GenerateSettings& settings);

// Paints the splat map from height and slope: layer 0 on flat ground, layer 1 on steep slopes,
// layer 2 below `low` (normalized height), layer 3 above `high`.
struct AutoPaintSettings {
    float slope_start{25.0f}, slope_end{40.0f}; // degrees: layer 1 fades in between these
    float low{0.12f};
    float high{0.7f};
    float blend{0.05f};                         // normalized height band of the low/high fades
};
void auto_paint(Heightfield& hf, math::Vec3 size, const AutoPaintSettings& settings = {});

// ---- component -----------------------------------------------------------------------------

struct TerrainComponent {
    Uuid data;                         // .vterrain asset
    math::Vec3 size{256, 40, 256};     // x/z extent and the height of a normalized 1.0 (m)
    // Material layers blended by the splat map. A layer without a texture uses its tint only;
    // tiling is the number of texture repeats across the whole terrain.
    Uuid layer0_texture, layer1_texture, layer2_texture, layer3_texture;
    math::Color layer0_tint{0.30f, 0.45f, 0.18f, 1}; // grass
    math::Color layer1_tint{0.42f, 0.40f, 0.38f, 1}; // rock
    math::Color layer2_tint{0.76f, 0.70f, 0.50f, 1}; // sand
    math::Color layer3_tint{0.95f, 0.95f, 0.97f, 1}; // snow
    float layer0_tiling{64}, layer1_tiling{48}, layer2_tiling{64}, layer3_tiling{48};
    float roughness{0.9f};
    bool cast_shadows{true};
    bool collision{true};
    float friction{0.8f};
    std::uint32_t collision_layer{0};
    std::uint32_t collision_mask{0xFFFFu};

    [[nodiscard]] const Uuid& layer_texture(std::uint32_t i) const noexcept;
    [[nodiscard]] const math::Color& layer_tint(std::uint32_t i) const noexcept;
    [[nodiscard]] float layer_tiling(std::uint32_t i) const noexcept;
};

// Registers "Terrain" and "Foliage". Idempotent.
void register_components();

// ---- geometry ------------------------------------------------------------------------------

// Builds chunk (cx, cz) of the heightfield as a published procedural mesh with LODs and skirts
// (positions in terrain local space, uvs normalized over the whole terrain, splat weights in
// the vertex colours, see terrain_surface_code()).
[[nodiscard]] Result<std::shared_ptr<const scene::ProceduralMesh>> build_chunk(const Heightfield& hf, math::Vec3 size,
                                                                               std::uint32_t cx, std::uint32_t cz);
// Smooth normal at a sample from central differences (local space).
[[nodiscard]] math::Vec3 sample_normal(const Heightfield& hf, math::Vec3 size, std::int32_t x, std::int32_t z) noexcept;
// Local-space height at local (x, z), or nullopt outside the terrain.
[[nodiscard]] std::optional<float> local_height(const Heightfield& hf, math::Vec3 size, float x, float z) noexcept;

// Surface shader (pbr.frag `surface()` function) that blends the four layers: material texture
// slots base colour, metal-rough, normal and emissive carry the layer 0..3 textures, shader
// parameter i is (layer tint rgb, tiling or 0 for an untextured layer), vertex colours carry
// the splat weights.
[[nodiscard]] std::string_view terrain_surface_code() noexcept;

// ---- runtime -------------------------------------------------------------------------------

using TerrainProvider = std::function<std::shared_ptr<Heightfield>(const Uuid& data)>;

// Written by TerrainSystem on every Terrain entity (not saved).
struct TerrainRuntime {
    std::shared_ptr<Heightfield> data;
    Uuid resolved;
    math::Vec3 built_size{0, 0, 0};
    const Heightfield* built_data{nullptr};
    std::uint64_t collision_revision{~0ull};
    std::shared_ptr<std::vector<float>> collision_heights; // shared with the collider; edited in place
    std::uint32_t chunks_per_side{0};
    std::vector<std::shared_ptr<const scene::ProceduralMesh>> chunks; // row-major (z rows)
    std::vector<std::uint64_t> chunk_built;                           // chunk revision each was built at
};

struct TerrainStats {
    std::uint32_t terrains{0}, missing_data{0}, chunk_rebuilds{0}, collision_rebuilds{0}, chunks{0};
    std::uint32_t collision_updates{0}; // edits passed to the collider as a dirty rectangle (no rebuild)
    std::uint32_t foliage{0}, foliage_instances{0}, foliage_rebuilds{0}; // foliage entities, live instances, chunk scatters
};

// Resolves terrain data, keeps chunk meshes in step with edits (only dirty chunks are rebuilt)
// and maintains the static height-field collider (physics::StaticHeightfieldComponent).
class TerrainSystem {
public:
    explicit TerrainSystem(TerrainProvider provider = {});
    void add_data(const Uuid& id, std::shared_ptr<Heightfield> data); // takes precedence
    void clear_cache();  // after .vterrain files changed on disk
    [[nodiscard]] std::shared_ptr<Heightfield> data(const Uuid& id);
    void update(scene::Scene& scene);
    [[nodiscard]] const TerrainStats& stats() const noexcept { return stats_; }

private:
    void update_foliage(scene::Scene& scene);
    TerrainProvider provider_;
    std::unordered_map<Uuid, std::shared_ptr<Heightfield>, UuidHash> added_, cache_;
    TerrainStats stats_;
};

// Runs update() in PreUpdate (before physics) and PostUpdate (after scripts and tools edited).
void add_terrain_systems(gameplay::Simulation& sim, TerrainSystem& system);

// World-space height of the terrain on `entity` below/above world (x, z), using the entity's
// world transform; nullopt when the entity has no resolved terrain or the point is outside.
[[nodiscard]] std::optional<float> height_at(scene::Scene& scene, scene::Entity entity, float x, float z);
// The first terrain (in entity order) covering world (x, z): its world-space height.
[[nodiscard]] std::optional<float> height_at(scene::Scene& scene, float x, float z);

// Everything gameplay usually wants about the ground under world (x, z): the first terrain (in
// entity order) covering it, its world height, smooth world normal and blended layer weights
// (e.g. for footstep sounds or spawning rules).
struct TerrainSample {
    scene::Entity entity;
    float height{0};
    math::Vec3 normal{0, 1, 0};
    math::Vec4 weights{1, 0, 0, 0};
};
[[nodiscard]] std::optional<TerrainSample> sample_at(scene::Scene& scene, scene::Entity entity, float x, float z);
[[nodiscard]] std::optional<TerrainSample> sample_at(scene::Scene& scene, float x, float z);
// Translation and rotation of a world matrix with the scale removed (how terrains are placed).
[[nodiscard]] math::Mat4 rigid_matrix(const math::Mat4& world) noexcept;

// ---- foliage -------------------------------------------------------------------------------
// Scatters instances of the entity's MeshRenderer (mesh, material, tint, shadows) over the
// Terrain on the same entity or its parent. Placement is deterministic: a jittered grid with
// `density` points per square metre, each kept with a probability equal to the splat weight of
// `layer` (so painting a layer paints its foliage) and only where the slope and normalized
// height are in range. Instances live in terrain local space per terrain chunk and are rebuilt
// only for chunks whose terrain revision changed. The Foliage entity's own transform is ignored
// and the MeshRenderer is drawn once per instance within `draw_distance` of the camera.
inline constexpr std::uint32_t kMaxFoliageInstances = 1u << 20;
struct FoliageComponent {
    std::int32_t layer{0};          // splat layer 0..3 that drives density; -1 = everywhere
    float min_weight{0.3f};         // no instances where the layer weight is below this
    float density{0.5f};            // candidate points per square metre
    float min_scale{0.6f}, max_scale{1.0f};
    float align_to_normal{0.0f};    // 0 = upright .. 1 = follow the surface normal
    bool random_yaw{true};
    float max_slope{35.0f};         // degrees
    float min_height{0.0f}, max_height{1.0f}; // normalized terrain height range
    float offset{0.0f};             // metres along the instance's up (negative sinks it)
    float draw_distance{150.0f};    // metres from the camera
    std::uint32_t seed{1};
    std::uint32_t max_instances{100000}; // the grid is coarsened to stay below this
};
// Written by TerrainSystem on Foliage entities (not saved).
struct FoliageRuntime {
    scene::Entity terrain;
    const Heightfield* built_data{nullptr};
    math::Vec3 built_size{0, 0, 0};
    std::uint64_t settings{0};                 // hash of the component the chunks were built with
    float spacing{0};                          // grid spacing in metres
    std::vector<std::vector<math::Mat4>> chunks; // terrain local instance matrices per chunk
    std::vector<math::Aabb> chunk_bounds;      // local bounds of the instance origins
    std::vector<std::uint64_t> chunk_built;    // terrain chunk revision each was built at
    std::uint32_t instances{0};
};
// Builds the instances of chunk (cx, cz) for a foliage layer (exposed for tests and tools).
[[nodiscard]] std::vector<math::Mat4> scatter_chunk(const Heightfield& hf, math::Vec3 size, const FoliageComponent& f,
                                                    float spacing, std::uint32_t cx, std::uint32_t cz);
// Grid spacing for a foliage layer on a terrain of `size` (density, capped by max_instances).
[[nodiscard]] float foliage_spacing(math::Vec3 size, const FoliageComponent& f) noexcept;
// Smooth local-space normal of the bilinear surface at local (x, z) (clamped to the terrain).
[[nodiscard]] math::Vec3 local_normal(const Heightfield& hf, math::Vec3 size, float x, float z) noexcept;

// ---- editing -------------------------------------------------------------------------------

// Inclusive sample rectangle.
struct SampleRect {
    std::int32_t x0{0}, z0{0}, x1{-1}, z1{-1};
    [[nodiscard]] bool empty() const noexcept { return x1 < x0 || z1 < z0; }
    [[nodiscard]] std::size_t count() const noexcept {
        return empty() ? 0 : static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1);
    }
    void merge(const SampleRect& o) noexcept;
    friend bool operator==(const SampleRect&, const SampleRect&) = default;
};

enum class BrushMode : std::uint8_t { Raise, Lower, Smooth, Flatten, Paint };
[[nodiscard]] std::string_view brush_mode_name(BrushMode mode) noexcept;

struct Brush {
    BrushMode mode{BrushMode::Raise};
    float radius{6.0f};     // metres (terrain local space)
    float strength{0.5f};   // 0..1
    float falloff{0.5f};    // fraction of the radius that fades out (0 = hard edge)
    std::uint32_t layer{0}; // Paint
    float target{0.5f};     // Flatten: normalized height
};

// Brush influence (0..1) at `distance` metres from its centre.
[[nodiscard]] float brush_weight(const Brush& brush, float distance) noexcept;

// One dab centred at terrain-local (x, z). `amount` scales the effect (seconds of brushing when
// dragging: Raise/Lower move the centre by strength * amount * radius / 2 metres). Heights stay
// in 0..1. Returns the changed samples, already touch()ed; empty when nothing changed.
SampleRect apply_brush(Heightfield& hf, math::Vec3 size, const Brush& brush, float x, float z, float amount);

// The samples of a rectangle (clamped to the field), for undo and redo.
struct HeightfieldRegion {
    SampleRect rect;
    std::vector<float> heights;
    std::vector<std::uint32_t> splat;
};
[[nodiscard]] HeightfieldRegion copy_region(const Heightfield& hf, SampleRect rect);
// Writes a region back and touch()es it. Regions that do not fit the field are ignored.
void paste_region(Heightfield& hf, const HeightfieldRegion& region);

// Ray against a heightfield in terrain local space: the first surface point within
// `max_distance` along the (normalized) direction.
[[nodiscard]] std::optional<math::Vec3> raycast_local(const Heightfield& hf, math::Vec3 size, math::Vec3 origin,
                                                     math::Vec3 direction, float max_distance) noexcept;

struct TerrainHit {
    scene::Entity entity;
    math::Vec3 point;    // world space
    math::Vec3 local;    // terrain local space
    math::Vec3 normal;   // world space
    float distance{0};
};
// World-space ray against one resolved terrain (needs WorldTransform and TerrainRuntime), or
// against every terrain (closest hit).
[[nodiscard]] std::optional<TerrainHit> raycast(scene::Scene& scene, scene::Entity entity, math::Vec3 origin,
                                                math::Vec3 direction, float max_distance);
[[nodiscard]] std::optional<TerrainHit> raycast(scene::Scene& scene, math::Vec3 origin, math::Vec3 direction,
                                                float max_distance);

} // namespace victus::terrain
