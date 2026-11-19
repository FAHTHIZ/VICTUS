// SPDX-License-Identifier: Apache-2.0
// Forward HDR 3D renderer scheduled through the render graph: cascaded directional shadow
// maps, optional depth pre-pass + SSAO, metallic-roughness PBR, point lights, procedural sky,
// debug lines, dual-filter bloom, auto exposure, tonemapping/grading and FXAA. Every feature
// scales through RenderSettings / QualityTier so the same content runs on phones and desktops.
#pragma once
#include "victus/render/environment_map.hpp"
#include "victus/render/gpu_context.hpp"
#include "victus/render/irradiance_volume.hpp"
#include "victus/render/render_graph.hpp"
#include "victus/render/render_path.hpp"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace victus::render {

struct Camera3D {
    math::Mat4 view;
    math::Vec3 position;
    float fov_y{math::radians(60.0f)};
    float z_near{0.1f};
    float z_far{500.0f};
    [[nodiscard]] static Camera3D look_at(math::Vec3 eye, math::Vec3 target, math::Vec3 up = {0, 1, 0});
    [[nodiscard]] static Camera3D from_transform(math::Vec3 position, math::Quat rotation);
    [[nodiscard]] math::Mat4 projection(float aspect) const noexcept;
    [[nodiscard]] math::Vec3 forward() const noexcept;
};

// How a material's colour reaches the frame. Opaque draws write depth and are instanced and
// sorted for state; Alpha (straight alpha) and Additive draws run after all opaque geometry,
// sorted back to front, test but do not write depth, and cast no shadows.
enum class MaterialBlend : std::uint8_t { Opaque, Alpha, Additive };
[[nodiscard]] std::string_view material_blend_name(MaterialBlend b) noexcept;
[[nodiscard]] std::optional<MaterialBlend> parse_material_blend(std::string_view name) noexcept;

struct Material {
    math::Color base_color{1, 1, 1, 1}; // linear
    math::Color emissive{0, 0, 0, 1};   // linear rgb, multiplied by emissive_strength
    float emissive_strength{1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    float normal_scale{1.0f};
    float alpha_cutoff{0.0f}; // >0 enables alpha testing
    // Texture coordinates are transformed as uv * uv_scale + uv_offset before sampling (tiling).
    math::Vec2 uv_scale{1, 1};
    math::Vec2 uv_offset{0, 0};
    bool nearest_filter{false}; // point sampling (pixel art); default is trilinear
    rhi::TextureHandle base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;
    bool double_sided{false};
    bool unlit{false};
    bool cast_shadows{true};
    // Drawn only into shadow maps (sun cascades and local lights), never into the frame: an object
    // hidden from the camera (a tree crown cut away to show what is under it) keeps its shadow.
    bool shadow_only{false};
    // Project surface shader: a fragment shader compiled from surface_shader_source() (see
    // below); invalid = the built-in PBR shader. `shader_params` reach it as material.custom[].
    rhi::ShaderHandle surface_shader;
    std::array<math::Vec4, 4> shader_params{};
    // Vertex alpha normally multiplies opacity. As a coverage mask it instead marks where the
    // scene's coverage layer may settle (alpha >= 0.5) and opacity ignores it.
    bool vertex_alpha_coverage_mask{false};
    // How strongly the scene's coverage layer (Environment::coverage_*) settles on this material,
    // 0 (never, e.g. water or glowing embers) .. 1.
    float coverage_response{1.0f};
    MaterialBlend blend{MaterialBlend::Opaque};
};

enum class FogMode : std::uint8_t { None, Linear, Exponential, ExponentialSquared };
[[nodiscard]] std::string_view fog_mode_name(FogMode m) noexcept;
[[nodiscard]] std::optional<FogMode> parse_fog_mode(std::string_view name) noexcept;

// The built-in PBR fragment shader with a project surface function spliced in (GLSL 450 that
// defines `void surface(SurfaceInput i, inout Surface s)`; see engine/render/shaders/pbr.frag).
// `#line` directives keep compiler messages on the user's own line numbers.
[[nodiscard]] std::string surface_shader_source(std::string_view surface_code);
// The unmodified pbr.frag source.
[[nodiscard]] std::string_view pbr_fragment_source() noexcept;

struct DirectionalLight {
    math::Vec3 direction{-0.4f, -1.0f, -0.3f}; // direction the light travels
    math::Color color{1.0f, 0.96f, 0.9f, 1.0f};
    float intensity{3.0f};
    bool shadows{true};
    float shadow_distance{40.0f};
    std::uint32_t shadow_resolution{2048};
};

struct PointLight {
    math::Vec3 position;
    math::Color color{1, 1, 1, 1};
    float intensity{10.0f};
    float range{10.0f};
    bool cast_shadows{false}; // six atlas tiles (cube faces) when within RenderSettings' budget
};

// Cone light. Angles are half-angles in radians from the axis: full intensity inside
// `inner_angle`, smoothly fading to zero at `outer_angle`.
struct SpotLight {
    math::Vec3 position;
    math::Vec3 direction{0, -1, 0}; // direction light travels
    math::Color color{1, 1, 1, 1};
    float intensity{10.0f};
    float range{10.0f};
    float inner_angle{0.35f};
    float outer_angle{0.5f};
    bool cast_shadows{false}; // one atlas tile; cones wider than 85 degrees stay unshadowed
};

// Particles are camera-facing billboards drawn after opaque geometry into the HDR target, as
// soft particles (they fade where they meet scene depth). Alpha batches are drawn back to front
// and their particles sorted back to front; additive batches need no sorting.
enum class ParticleBlend : std::uint8_t { Alpha, Additive };
struct ParticleInstance {
    math::Vec4 position_size; // xyz world position, w size (full width in metres)
    math::Vec4 color;         // linear RGBA, rgb may exceed 1 for glowing particles
    math::Vec4 params;        // x rotation (radians); y flipbook frame (left to right, top to bottom);
                              // z flipbook grid = columns * 16 + rows (0: whole texture; 1..15 columns and
                              // rows); w height / width (0 or 1: square; > 1 stretches along the rotated up axis,
                              // e.g. sparks and rain along their screen velocity)
};
// One point of a camera-facing ribbon (a particle trail), in strip order. The strip is expanded
// toward the camera on the GPU: across it the texture's x runs 0..1, along it y = `v`.
struct RibbonPoint {
    math::Vec3 position; // world space, trail centre
    float width{0};      // full width in metres
    math::Color color;   // linear RGBA, rgb may exceed 1
    float v{0};          // texture y along the trail (unused without a texture)
    float reserved[3]{};
};
static_assert(sizeof(RibbonPoint) == 48);
struct ParticleMaterial {
    rhi::TextureHandle texture; // invalid => built-in soft disc
    ParticleBlend blend{ParticleBlend::Alpha};
    float softness{0.25f};      // metres over which particles fade into geometry
};

// GPU-simulated particles (ADR-0097). The caller (the particle system) owns the emission schedule
// and slot bookkeeping and hands the renderer the particles it spawned; the renderer keeps each
// emitter's state on the GPU (one 64-byte GpuParticleSpawn per slot), integrates it in compute
// shaders right before the particle pass (optionally bouncing particles off the scene depth) and
// draws every slot as a billboard (dead slots collapse to nothing). Layouts match particle_sim.comp.
struct GpuParticleSpawn {
    math::Vec4 position_age; // xyz simulation-space position, w age (spawns: seconds already lived this frame)
    math::Vec4 velocity_life;
    math::Vec4 misc; // rotation, spin (rad/s), size scale, flipbook start frame
    std::uint32_t meta[4]{}; // spawns: x = slot; GPU state: x = frame serial it was born on
};
static_assert(sizeof(GpuParticleSpawn) == 64);
struct GpuParticleSpawnRecord {
    GpuParticleSpawn particle;
    std::uint64_t index{0}; // spawn sequence number of the emitter (0, 1, 2, ...)
    double time{0};         // emitter clock at birth
};
static_assert(sizeof(GpuParticleSpawnRecord) == 80);
struct GpuParticleParams {
    math::Mat4 world{math::Mat4::identity()}; // emitter -> world (used unless world_space)
    math::Vec4 gravity_drag;                  // xyz gravity (m/s^2), w drag (1/s)
    math::Vec4 turbulence;                    // x amplitude (m/s^2), y wave number (rad/m), z field speed (rad/s), w field phase at `time`
    math::Vec4 size;                          // x size start, y size end, z intensity, w velocity stretch (s)
    math::Vec4 color_start{1, 1, 1, 1}, color_end{1, 1, 1, 0};
    math::Vec4 flipbook;                      // x columns, y rows, z frames used, w fps (0: once over the life)
    math::Vec4 collision;                     // x bounce, y friction, z thickness (m)
    bool world_space{true};
    bool collide{false};                      // bounce off the scene depth (world-space emitters only)
};
struct GpuParticleEmitter {
    std::uint64_t key{0};        // stable per emitter; selects the renderer's GPU state
    std::uint64_t instance{0};   // a new value drops the GPU state (emitter recreated)
    std::uint32_t generation{0}; // a new value drops every particle (reset, capacity change)
    std::uint32_t capacity{0};   // slots (<= SceneRenderer::kMaxGpuParticles)
    std::uint32_t alive{0};      // live particles (for stats)
    double time{0};              // emitter clock the state should be advanced to
    std::uint64_t spawned{0};    // total spawn records issued
    std::span<const GpuParticleSpawnRecord> records; // recent spawns, ascending index (must outlive render())
    GpuParticleParams params;
    ParticleMaterial material;
    math::Aabb bounds;           // conservative world bounds of every live particle (culling, ordering)
};

// Projected decals (ADR-0064): a box (the unit cube [-0.5, 0.5]^3 through `transform`) projects a
// texture along its local -Z axis onto the opaque surfaces inside it. They are drawn after opaque
// geometry and before blended geometry by reconstructing positions from scene depth, so in the
// forward pipeline they cannot change surface normals or PBR inputs:
//   Multiply: scales the lit colour by the decal colour (dirt, stains, cracks, bullet holes; exact
//             for diffuse light, since lit diffuse is proportional to albedo),
//   Alpha:    paints unlit colour over the surface (markings that should not fall into shadow),
//   Additive: adds emissive light (glowing runes, embers, neon paint).
// Texture alpha times `color.a` is the opacity. Decals also land on moving objects that enter the box.
enum class DecalBlend : std::uint8_t { Multiply, Alpha, Additive };
[[nodiscard]] std::string_view decal_blend_name(DecalBlend b) noexcept;
[[nodiscard]] std::optional<DecalBlend> parse_decal_blend(std::string_view name) noexcept;
struct Decal {
    math::Mat4 transform{math::Mat4::identity()}; // unit cube -> world (scale = box size in metres)
    rhi::TextureHandle texture;                   // invalid => white
    math::Color color{1, 1, 1, 1};                // linear; alpha scales opacity
    math::Vec4 uv{0, 0, 1, 1};                    // u0, v0 (top), u1, v1 (bottom) of the texture region
    DecalBlend blend{DecalBlend::Multiply};
    // Surfaces facing away from the projection axis fade out: 0 = project onto every surface in the
    // box, 1 = only onto surfaces facing the axis head-on. Hides stretching on perpendicular walls.
    float angle_fade{0.3f};
    float depth_fade{0.2f}; // fraction of the box depth over which the decal fades at its near/far ends
    std::int32_t sort_order{0}; // drawn in ascending order (later decals cover earlier ones)
    bool nearest_filter{false};
};

// Water surface (ADR-0095): a horizontal grid displaced by up to four Gerstner waves, shaded with
// Fresnel reflection, depth-based refraction/absorption, sun glint and foam. Drawn after opaque
// geometry and decals, before blended draws and particles; writes depth so later passes sort
// against it. Waves use theta = k * dot(direction, xz) - phase (gameplay::WaterWave).
struct WaterWave {
    math::Vec2 direction{1, 0};
    float k{0}, amplitude{0}, q{0}, phase{0};
};
struct WaterSurface {
    math::Vec3 center{};              // rest surface centre (world)
    math::Vec2 half_extent{25, 25};   // metres along X and Z
    std::array<WaterWave, 4> waves{};
    math::Color shallow_color{0.6f, 0.9f, 0.85f, 1};
    math::Color deep_color{0.01f, 0.09f, 0.13f, 1};
    float clarity{4.0f};     // metres of visibility
    float foam{0.6f};
    float refraction{0.035f};
    float roughness{0.06f};
    std::uint32_t resolution{128}; // grid cells per side (16..512)
    float time{0};                 // seconds; animates ripples and foam
};
inline constexpr std::size_t kMaxWaterSurfaces = 16;

enum class Tonemapper : std::uint8_t { None, Aces, Neutral, Reinhard };
[[nodiscard]] std::string_view tonemapper_name(Tonemapper t) noexcept;
[[nodiscard]] std::optional<Tonemapper> parse_tonemapper(std::string_view name) noexcept;

// Per-scene look (lighting environment and post-processing artistic controls).
struct ReflectionProbe {
    const EnvironmentMap* map{nullptr};
    math::Aabb box;           // world-space influence and parallax box
    math::Vec3 position{};    // capture point (inside the box)
    float intensity{1.0f};
};
inline constexpr std::size_t kMaxReflectionProbes = 64;

// Volumetric fog (ADR-0098): participating media in a camera-aligned froxel grid (Wronski 2014,
// Hillaire 2015). Every froxel takes exponential height fog plus the fog volumes covering it, is lit
// by the sun (with cascade shadows, Henyey-Greenstein phase), the ambient sky and the clustered
// local lights (with their shadows), is blended with its reprojected previous value, and the grid
// is integrated front to back; the result scatters light into and attenuates the opaque scene up
// to `distance` metres. Needs compute and RenderSettings::volumetric_fog (off on Low).
struct VolumetricFog {
    bool enabled{false};
    float density{0.02f};          // extinction per metre at and below `height`
    float height{0.0f};            // world height where the height falloff starts
    float height_falloff{0.1f};    // per metre above `height` (0 = uniform fog)
    math::Color albedo{1, 1, 1, 1}; // scattering / extinction (linear rgb)
    float anisotropy{0.6f};        // Henyey-Greenstein g: 0 isotropic .. 0.95 strongly forward
    float distance{80.0f};         // metres of view depth covered by the froxel grid (8..1000)
    float sun_intensity{1.0f};     // multipliers of the light sources scattered by the fog
    float ambient_intensity{1.0f};
    float local_light_intensity{1.0f};
    float temporal{0.9f};          // weight of the reprojected history (0 = none, deterministic)
};

// A box of extra fog for this frame: `transform` maps the unit cube [-0.5, 0.5]^3 to the world.
struct FogVolume {
    math::Mat4 transform{math::Mat4::identity()};
    float density{0.1f};            // extinction per metre added inside
    math::Color albedo{1, 1, 1, 1};
    float edge_fade{0.1f};          // fade-in width as a fraction of the box size (0 hard .. 0.5)
};
inline constexpr std::size_t kMaxFogVolumes = 8;

struct Environment {
    math::Color sky_zenith{0.18f, 0.36f, 0.75f, 1};
    math::Color sky_horizon{0.62f, 0.72f, 0.85f, 1};
    math::Color ground{0.22f, 0.2f, 0.18f, 1};
    float ambient_intensity{0.6f};
    float exposure{1.0f};          // manual exposure multiplier (ignored by auto exposure)
    float vignette{0.25f};
    bool tonemap{true};            // false: no tonemapping curve (clamp only)
    Tonemapper tonemapper{Tonemapper::Aces};
    float exposure_compensation{0.0f}; // EV stops applied after manual/auto exposure
    float saturation{1.0f};
    float contrast{1.0f};
    // Auto exposure (eye adaptation) targets middle grey at the scene's average luminance.
    bool auto_exposure{false};
    float auto_exposure_min_ev{-4.0f}; // log2 average luminance limits
    float auto_exposure_max_ev{6.0f};
    float adaptation_speed_up{3.0f};   // 1/seconds, towards brighter
    float adaptation_speed_down{1.5f}; // 1/seconds, towards darker
    // Bloom (applied only when enabled by RenderSettings).
    float bloom_intensity{0.05f};
    float bloom_threshold{1.0f};
    float bloom_knee{0.5f};
    // Screen-space ambient occlusion (applied only when enabled by RenderSettings).
    float ao_radius{0.6f};
    float ao_intensity{1.0f};
    float ao_fade_distance{60.0f}; // AO fades out between half this view distance and this
    bool draw_sky{true};
    math::Color clear_color{0.05f, 0.05f, 0.06f, 1}; // when draw_sky is false
    // Procedural sky background with its own gradient. Off: the background shows sky_zenith/sky_horizon/ground,
    // the same terms that light the scene. On: those stay the ambient and the background uses these, so a game can
    // pair a dim hemisphere ambient with a bright visible sky. Ignored when an environment map is drawn.
    bool sky_background{false};
    math::Color background_zenith{0.18f, 0.36f, 0.75f, 1};
    math::Color background_horizon{0.62f, 0.72f, 0.85f, 1};
    math::Color background_ground{0.22f, 0.2f, 0.18f, 1};
    // Distance fog on lit and unlit opaque geometry (camera distance in metres). The colour is
    // linear radiance, multiplied by ambient_intensity like the sky colours.
    FogMode fog_mode{FogMode::None};
    math::Color fog_color{0.62f, 0.72f, 0.85f, 1};
    float fog_start{50.0f};    // linear: no fog nearer than this; exponential: fog-free radius
    float fog_end{400.0f};     // linear: full fog at this distance
    float fog_density{0.004f}; // exponential modes, per metre
    float fog_max_opacity{1.0f};
    // Coverage layer (snow, dust, ash...) on upward-facing surfaces: 0 = none .. 1 = full.
    float coverage_amount{0.0f};
    math::Color coverage_color{0.86f, 0.89f, 0.95f, 1}; // linear albedo
    float coverage_roughness{0.8f};
    float coverage_slope{0.55f};     // surface normal.y where coverage starts
    float coverage_sharpness{3.0f};  // ramp steepness above the slope threshold
    // Image-based lighting. With an environment map, ambient diffuse comes from its SH9
    // irradiance and reflections from its prefiltered cubemap (both scaled by
    // environment_intensity and ambient_intensity's role is taken over); the sky background
    // shows the map. Without one, the analytic zenith/horizon/ground gradient is used. The map
    // must outlive the frames that use it.
    const EnvironmentMap* environment_map{nullptr};
    bool environment_sun_disc{false}; // draw the sun disc over the environment background (physical sky)
    // Scene captures (reflection probes): skip tonemapping/grading and write linear radiance
    // times exposure into a float target.
    bool linear_output{false};
    float environment_intensity{1.0f};
    float environment_rotation{0.0f}; // radians about +Y
    float sky_blur{0.0f};             // background blur 0 (sharp) .. 1 (fully rough mip)
    VolumetricFog volumetric_fog{};
};

// Scalability tiers. Low targets entry-level phones, Medium mid-range phones/tablets, High
// desktops/consoles/flagship phones, Ultra high-end desktops.
enum class QualityTier : std::uint8_t { Low, Medium, High, Ultra };
enum class DebugView : std::uint8_t { None, Overdraw };
[[nodiscard]] std::string_view quality_tier_name(QualityTier t) noexcept;
[[nodiscard]] std::optional<QualityTier> parse_quality_tier(std::string_view name) noexcept;

struct RenderSettings {
    QualityTier tier{QualityTier::High};
    float render_scale{1.0f};             // 3D resolution scale (0.25..2); temporally upscaled with TAA, else bilinearly
    std::uint32_t shadow_cascades{3};     // 1..4, packed into a 2x2 atlas when > 1
    std::uint32_t shadow_resolution{2048}; // per cascade; also capped by DirectionalLight
    float cascade_split_lambda{0.8f};     // 0 uniform .. 1 logarithmic splits
    bool soft_shadows{true};              // 3x3 PCF (else one hardware 2x2 PCF tap)
    bool fast_soft_shadows{false};        // with soft_shadows: a 4-tap tent (four 2x2 PCF taps) instead of 9 taps
    bool depth_prepass{false};            // forced on by SSAO
    bool ssao{true};
    bool ssao_half_resolution{true};
    std::uint32_t ssao_samples{8};        // 4..32
    bool bloom{true};
    std::uint32_t bloom_levels{5};        // 1..8 mip levels
    bool fxaa{true};                      // ignored while TAA is active
    // Spatial upscale (ADR-0066): below output resolution without temporal upscaling, tonemap and FXAA
    // run at render resolution and a final pass resamples to the output with a Catmull-Rom filter and
    // contrast-adaptive sharpening (RCAS), instead of stretching the HDR image bilinearly and running
    // FXAA on the blurred result. Off: the previous bilinear path.
    bool spatial_upscale{false};
    // Contrast-adaptive sharpening strength (0 off .. 1 strongest) of that final pass. Above 0 the pass
    // also runs at full resolution (and after TAA) as plain sharpening.
    float sharpen{0.0f};
    // Temporal anti-aliasing: sub-pixel jitter + reprojected history (camera motion from depth,
    // moving objects bounded by neighbourhood clipping). Also stabilises SSAO and shadows.
    bool taa{true};                       // High/Ultra; Low/Medium use FXAA
    float taa_blend{0.1f};                // weight of the current frame (0.04..1)
    // Temporal upscaling (ADR-0055): with TAA on and render_scale < 1 (or dynamic resolution
    // lowering it), the resolve reconstructs at output resolution from jittered samples instead of
    // upscaling the internal image bilinearly. Off: the TAA history stays at render resolution.
    bool temporal_upscale{true};
    // Object motion vectors (ADR-0054): opaque draws submitted with a motion id whose transform or
    // joints changed since the previous frame are drawn into a motion buffer the TAA resolve
    // follows, so moving and animated objects keep their history instead of ghosting. TAA only.
    bool motion_vectors{true};
    bool auto_exposure{true};             // allowed; scenes enable it through Environment
    bool instancing{true};                // merge identical mesh+material draws into instanced batches
    // Mesh levels of detail: each draw uses the coarsest level whose geometric error, projected
    // at the draw's distance, stays within `lod_error_pixels` render pixels (0.25..16).
    bool lods{true};
    float lod_error_pixels{1.0f};
    int force_lod{-1};                    // debugging: >= 0 draws that level (clamped per mesh)
    // LOD hysteresis (ADR-0077): a draw submitted with a motion id keeps last frame's level while
    // that level is between the stateless pick for `lod_error_pixels` and the pick for
    // `lod_error_pixels * (1 - lod_hysteresis)`, so an object resting on a threshold does not
    // switch every frame. Never coarser than the stateless pick: the pixel budget always holds.
    // History is per renderer and per output size; reset_history() and a different output size
    // (another view) fall back to the stateless rule. 0 disables (0..0.9).
    float lod_hysteresis{0.25f};
    // Shadow casters (directional cascades and local shadow tiles) draw this many levels coarser
    // than the level picked for the main camera (0..3, clamped per mesh). Selection stays
    // main-camera based, as in Godot and Unreal, so shadows and the lit surface keep the same
    // reference; the bias only trades shadow-pass triangles for silhouette error. Low: 1.
    std::uint32_t shadow_lod_bias{0};
    // Anisotropic filtering for material textures: at most this many taps along the texture's
    // stretched axis on surfaces seen at grazing angles (floors, roads, terrain). 1 (off), 2, 4, 8
    // or 16; other values round down. Low 2, Medium 4, High 8, Ultra 16. Nearest-filtered (pixel
    // art) materials are never anisotropic; devices without the feature fall back to trilinear.
    std::uint32_t max_anisotropy{8};
    // GPU occlusion culling: when the depth pre-pass runs, a compute pass builds a Hi-Z pyramid
    // from it and tests every opaque main-pass instance; survivors are drawn with indirect draws.
    // Conservative: the image is identical with it off. Needs compute (RenderCaps::compute).
    bool occlusion_culling{true};
    // Irradiance volumes (ADR-0056): baked probe grids replace environment diffuse light inside
    // their box. Off on Low (18 extra texture fetches per lit pixel).
    bool irradiance_volumes{true};
    // Dynamic GI (ADR-0057): volumes marked dynamic keep re-capturing their probes round-robin with
    // feedback (each refresh adds a bounce), so lighting changes propagate. High/Ultra only; read
    // by the runtime, the renderer itself only samples the volume.
    bool dynamic_gi{true};
    // Local light shadows (ADR-0052): lights with cast_shadows get perspective shadow maps in one
    // atlas of local_shadow_atlas^2 texels split into local_shadow_tile^2 tiles (at most 8x8). Spot
    // lights take one tile, point lights six; lights covering more of the screen win the tiles.
    bool local_shadows{true};
    std::uint32_t local_shadow_atlas{2048}; // 256..8192
    std::uint32_t local_shadow_tile{512};   // 64..atlas
    std::uint32_t max_point_lights{512};  // local (point + spot) lights per frame, <= SceneRenderer::kMaxLights
    // Debug views replace the final image. Overdraw: every visible opaque, alpha-tested and blended
    // draw is rasterised again without depth testing and counted per pixel, shown as a heat ramp
    // (black 0, blue 1, green 2, yellow 3, orange 4, red 5-7, white 8+); read_overdraw() returns the
    // counts. Depth complexity, not shaded fragments: the depth pre-pass and early-z reject many of
    // these layers in the normal image.
    DebugView debug_view{DebugView::None};
    // Volumetric fog (ADR-0098; scenes enable it through Environment::volumetric_fog): froxel tile
    // size in render pixels (4..64; the grid is capped at 160x90) and depth slices (16..128).
    // Low: off. Medium 16 px / 48 slices, High 16 / 64, Ultra 8 / 96.
    bool volumetric_fog{true};
    std::uint32_t volumetric_fog_tile{16};
    std::uint32_t volumetric_fog_slices{64};

    [[nodiscard]] static RenderSettings preset(QualityTier tier) noexcept;
    // Clamps every field into its supported range.
    [[nodiscard]] RenderSettings sanitized() const noexcept;
};

// Per-pixel layer counts of the last frame rendered with DebugView::Overdraw (render resolution).
struct OverdrawStats {
    std::uint32_t width{0}, height{0};
    std::uint32_t covered_pixels{0}; // pixels with at least one layer
    std::uint64_t layers{0};         // rasterised fragments over all pixels (saturates at 255 per pixel)
    std::uint32_t max_layers{0};
    // Mean layers per covered pixel (1.0 = no overdraw); 0 when nothing was drawn.
    [[nodiscard]] double average() const noexcept {
        return covered_pixels ? static_cast<double>(layers) / static_cast<double>(covered_pixels) : 0.0;
    }
};

struct SceneStats {
    std::uint32_t submitted{0}, visible{0}, culled{0}, shadow_casters{0}, lines{0};
    std::uint32_t anisotropy{1}; // anisotropic filtering level applied to material textures this frame
    std::uint32_t point_lights{0};      // local lights used (point + spot)
    std::uint32_t spot_lights{0};
    std::uint32_t light_indices{0};     // clustered light index list length
    std::uint32_t max_cluster_lights{0}; // most lights in one cluster
    std::uint32_t shadow_cascades{0}, shadow_draws{0}, prepass_draws{0};
    std::uint32_t render_width{0}, render_height{0};
    bool taa{false};                   // TAA resolved this frame
    bool taa_history{false};           // ... with a valid history (false on the first frame / after a reset)
    bool temporal_upscale{false};      // ... reconstructed at output resolution from a lower render resolution
    bool spatial_upscale{false};       // ADR-0066: resampled to the output by the Catmull-Rom + RCAS pass
    bool sharpened{false};             // the upscale/sharpen pass ran this frame
    std::uint32_t irradiance_probes{0}; // baked probes of the irradiance volume used this frame (0 = none)
    math::Vec2 jitter{};               // sub-pixel jitter in render pixels
    // Draw calls issued per pass after instancing (a batch = one draw_indexed with N instances).
    std::uint32_t main_batches{0}, shadow_batches{0}, prepass_batches{0};
    std::uint32_t transparent_draws{0};
    // Shading cost, separate from geometry (H2): visible opaque draws with an alpha test (they
    // cannot use early depth rejection as well), visible draws running a project surface shader,
    // and the triangles of blended draws (each blended layer shades every covered pixel again).
    std::uint32_t alpha_tested_draws{0}, surface_shader_draws{0};
    std::uint64_t transparent_triangles{0};
    std::uint32_t decals{0};           // decals drawn (after frustum culling)
    std::uint32_t water_surfaces{0};   // water surfaces drawn (after frustum culling)
    std::uint32_t reflection_probes{0}, probe_draws{0}; // probes used / visible draws lit by a probe
    std::uint32_t instances_uploaded{0};
    std::uint32_t skinned_draws{0};    // skinned submissions this frame
    std::uint32_t joints_uploaded{0};  // joint matrices in the per-frame palette
    std::uint32_t particle_batches{0}; // visible particle batches (one draw each)
    std::uint32_t particles{0};        // particles drawn
    std::uint32_t ribbon_vertices{0};  // trail triangle-list vertices drawn (six per segment)
    // GPU particles: emitters simulated / live particles of the drawn ones / spawns uploaded.
    std::uint32_t gpu_particle_emitters{0}, gpu_particles{0}, gpu_particle_spawns{0};
    std::uint32_t lod_draws{0};        // visible draws using a coarser level than LOD 0
    std::uint32_t lod_held{0};         // draws kept finer than the stateless pick by lod_hysteresis
    std::uint64_t triangles{0};        // triangles in the main and transparent passes (all instances)
    std::uint64_t shadow_triangles{0}; // triangles drawn into directional cascades and local shadow tiles
    // GPU occlusion culling this frame: instances tested on the GPU / main batches drawn
    // indirectly (survivor counts stay on the GPU; see SceneRenderer::read_occlusion_survivors).
    bool occlusion{false};
    // Local light shadows: shadowed lights, atlas tiles rendered, caster draws over all tiles.
    std::uint32_t local_shadow_lights{0}, local_shadow_tiles{0}, local_shadow_draws{0};
    std::uint32_t occlusion_tested{0}, occlusion_batches{0};
    // Object motion vectors: moving instances drawn into the motion buffer / their batches.
    std::uint32_t motion_draws{0}, motion_batches{0};
    // Volumetric fog this frame: grid size (0 when off) and fog volumes used.
    bool volumetric_fog{false};
    std::uint32_t fog_froxels_x{0}, fog_froxels_y{0}, fog_slices{0}, fog_volumes{0};
};

struct SceneRendererOptions {
    // Store the HDR scene colour, bloom chain and TAA history as R11G11B10_FLOAT (4 bytes/pixel) instead of
    // RGBA16_FLOAT (8): half the framebuffer bandwidth on tile-based mobile GPUs. No alpha and no negative
    // values, neither of which the HDR chain uses. Falls back to RGBA16_FLOAT when unsupported.
    bool compact_hdr{false};
};

class SceneRenderer {
public:
    static constexpr std::uint32_t kMaxLights = 4096;
    static constexpr std::uint32_t kMaxPointLights = kMaxLights; // former name
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>> create(GpuContext& ctx, const SceneRendererOptions& options = {});
    [[nodiscard]] rhi::Format hdr_format() const noexcept { return hdr_format_; }
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void set_settings(const RenderSettings& settings) noexcept { settings_ = settings.sanitized(); }
    [[nodiscard]] const RenderSettings& settings() const noexcept { return settings_; }
    // Probe captures: linear scene radiance (no exposure, tonemapping, grading or FXAA).
    void set_capture_mode(bool on) noexcept { capture_mode_ = on; }
    [[nodiscard]] bool capture_mode() const noexcept { return capture_mode_; }
    // Seconds since the previous frame (drives auto exposure adaptation). Default 1/60.
    void set_frame_time(float seconds) noexcept { frame_time_ = seconds; }
    // Time seen by surface shaders (SurfaceInput.time). Default: seconds since the process
    // started rendering, so animated shaders also move in the editor's edit mode.
    void set_time(double seconds) noexcept { time_ = seconds; }
    // Forces auto exposure to jump to the current scene luminance on the next frame.
    void reset_adaptation() noexcept { adaptation_valid_ = false; }
    // Discards the TAA history (camera cuts, teleports, scene loads) so the next frame does not
    // blend with unrelated content.
    // Also discards the LOD hysteresis history.
    void reset_history() noexcept {
        history_valid_ = false;
        lod_history_valid_ = false;
        fog_history_valid_ = false;
    }

    void begin(const Camera3D& camera, const DirectionalLight& sun, const Environment& env);
    void add_point_light(const PointLight& light);
    void add_spot_light(const SpotLight& light);
    // `motion_id` (non-zero, unique per object and stable across frames, e.g. an entity id) lets
    // the renderer compare the draw with the previous frame and write object motion vectors for
    // TAA; 0 = no tracking (the object is treated as static, camera motion only).
    void submit(const GpuMesh& mesh, const Material& material, const math::Mat4& model, std::uint64_t motion_id = 0);
    // Many static copies of one mesh+material (foliage, scattered props): equivalent to calling
    // submit() once per model with motion_id 0, but the material is hashed once and the draw list
    // grows once. The copies are still culled, LOD-selected and shadowed per instance and merge
    // into instanced batches like any other identical draws.
    void submit_instances(const GpuMesh& mesh, const Material& material, std::span<const math::Mat4> models);
    // Skinned draw (GPU skinning). `joints` are skin matrices in the mesh's model space
    // (inverse(model) * joint_world * inverse_bind); `world_bounds` must contain the posed mesh.
    // Falls back to submit() when the mesh has no skin stream or no joints are given. Skinned
    // instances of one mesh+material are still batched into one instanced draw.
    void submit_skinned(const GpuMesh& mesh, const Material& material, const math::Mat4& model,
                        std::span<const math::Mat4> joints, const math::Aabb& world_bounds,
                        std::uint64_t motion_id = 0);
    // One batch of billboards sharing a material (e.g. one emitter); `bounds` (world space,
    // including particle sizes) is used for frustum culling and batch ordering.
    void submit_particles(std::span<const ParticleInstance> particles, const ParticleMaterial& material,
                          const math::Aabb& bounds);
    // A GPU-simulated emitter for this frame (see GpuParticleEmitter). Every submitted emitter is
    // simulated (also when off screen, so its state keeps up with the CPU schedule); visible ones
    // are drawn with the other particle batches. Not simulated in capture mode.
    static constexpr std::uint32_t kMaxGpuParticles = 1u << 20;
    void submit_gpu_particles(const GpuParticleEmitter& emitter);
    // GPU state of an emitter after the last render (synchronous readback; tests and tooling).
    [[nodiscard]] Result<std::vector<GpuParticleSpawn>> read_gpu_particles(std::uint64_t key);
    // Ribbon strips sharing a material (e.g. one emitter's trails): `strips` holds the point count
    // of each consecutive strip in `points`; strips with fewer than two points are skipped.
    // Drawn in the particle pass (soft depth fade), ordered back to front with particle batches.
    void submit_ribbons(std::span<const RibbonPoint> points, std::span<const std::uint32_t> strips,
                        const ParticleMaterial& material, const math::Aabb& bounds);
    // Local reflection probe: draws whose bounds centre lies inside `box` use the probe's cube
    // (box-projected) for reflections and its SH for diffuse light instead of the scene
    // environment; the smallest containing box wins. The map must outlive the frame.
    void add_reflection_probe(const ReflectionProbe& probe);
    // Projected decal for this frame (see Decal). At most kMaxDecals per frame (nearest first).
    static constexpr std::uint32_t kMaxDecals = 1024;
    void add_decal(const Decal& decal);
    // Water surface for this frame (see WaterSurface). At most kMaxWaterSurfaces (nearest first).
    void add_water(const WaterSurface& water);
    // Fog volume for this frame (see FogVolume), used while the environment's volumetric fog is on.
    // At most kMaxFogVolumes (nearest first).
    void add_fog_volume(const FogVolume& volume);
    // Irradiance volume (ADR-0056) for this frame: lit surfaces inside its box take diffuse
    // ambient light from its probes (offset `normal_offset` metres along the normal) instead of the
    // environment or reflection probe SH. One volume per frame; cleared by begin(). The volume must
    // outlive the frame.
    void set_irradiance_volume(const IrradianceVolume* volume, float intensity = 1.0f, float normal_offset = 0.0f);
    void line(math::Vec3 a, math::Vec3 b, math::Color color);
    void box(const math::Aabb& box, math::Color color);
    void grid(float half_size, float spacing, math::Color color);

    // Renders into `target` (any colour-target format; sRGB encoding is applied for UNORM targets).
    [[nodiscard]] Status render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width,
                                std::uint32_t height, rhi::Format target_format);

    [[nodiscard]] const SceneStats& stats() const noexcept { return stats_; }
    // Which rendering features ran in the last render(), their output quality and why the others
    // did not (ADR-0103).
    [[nodiscard]] const RenderPathReport& path_report() const noexcept { return path_; }
    // Debug/test readback (waits for the GPU): instances that passed the last frame's occlusion
    // test, summed over its culled batches. 0 when the last frame did not cull.
    [[nodiscard]] Result<std::uint32_t> read_occlusion_survivors();
    // Debug/test readback (waits for the GPU) of the last DebugView::Overdraw frame; zeros when no
    // frame used it.
    [[nodiscard]] Result<OverdrawStats> read_overdraw();
    [[nodiscard]] const CompiledGraphInfo& graph_info() const noexcept { return graph_info_; }
    // Cascade 0 matrix (identity when shadows are off).
    [[nodiscard]] const math::Mat4& light_view_projection() const noexcept { return cascade_vp_[0]; }
    [[nodiscard]] const std::array<math::Mat4, 4>& cascade_view_projections() const noexcept { return cascade_vp_; }
    [[nodiscard]] const std::array<float, 4>& cascade_splits() const noexcept { return cascade_splits_; }

private:
    explicit SceneRenderer(GpuContext& ctx); // defined in the .cpp, where FrameScratch is complete
    struct Draw {
        const GpuMesh* mesh;
        Material material;
        math::Mat4 model;
        float distance;
        std::uint64_t batch_key; // equal keys => same mesh and identical material
        std::uint32_t palette{0}; // first joint matrix in palette_ (skinned only)
        bool skinned{false};
        std::uint8_t lod{0};      // level of detail picked for this frame
        std::uint8_t shadow_lod{0}; // level used by the shadow passes (lod + shadow_lod_bias)
        math::Aabb world_bounds;  // skinned only (static draws use mesh bounds * model)
        int probe{-1};            // reflection probe index (-1: scene environment)
        std::uint64_t motion_id{0};
        std::uint32_t joint_count{0}; // skinned only
    };
    // Previous-frame state of a tracked object (motion vectors).
    struct MotionState {
        math::Mat4 model;
        std::uint32_t joint_offset{0}, joint_count{0}; // into motion_joints_
    };
    // A run of consecutive instances in the per-frame instance buffer sharing one mesh/material.
    struct Batch {
        const Draw* first;
        std::uint32_t instance_offset, instance_count;
        std::uint8_t lod{0}; // level drawn by every instance of the batch
    };
    struct ParticleBatch {
        ParticleMaterial material;
        std::uint32_t offset, count;
        math::Aabb bounds;
        float distance{0};
        bool ribbon{false}; // triangle-list ribbon vertices rather than billboard instances
        rhi::BufferHandle buffer{}; // GPU particles: their instance buffer (offset 0, count slots)
    };
    struct GpuParticleState {
        rhi::BufferHandle state, instances, spawns;
        std::uint32_t capacity{0}, spawn_capacity{0};
        std::uint64_t instance{0}, next_index{0};
        std::uint32_t generation{0}, serial{0};
        double time{0};
        std::uint64_t last_frame{0};
    };
    // GPU particle compute pipeline, created on first use; false (logged once) when unavailable.
    [[nodiscard]] bool ensure_gpu_particle_pipeline();
    void destroy_gpu_particle_state(GpuParticleState& s);
    struct LineVertex {
        math::Vec3 pos;
        std::uint32_t color;
    };
    [[nodiscard]] Result<rhi::PipelineHandle> post_pipeline(const char* fragment, rhi::Format format,
                                                            rhi::BlendMode blend = rhi::BlendMode::Opaque);
    void compute_cascades(float aspect, std::uint32_t resolution);
    // Pipeline for a project surface shader (created on first use); the built-in one on failure.
    [[nodiscard]] rhi::PipelineHandle surface_pipeline(rhi::ShaderHandle fragment, bool skinned, bool double_sided, MaterialBlend blend);
    [[nodiscard]] rhi::PipelineHandle blend_pipeline(MaterialBlend blend, bool skinned, bool double_sided);
    [[nodiscard]] Status ensure_exposure_textures();
    [[nodiscard]] Status ensure_history(std::uint32_t index, std::uint32_t width, std::uint32_t height);
    // Creates the occlusion compute pipelines on first use; false (logged once) when unavailable.
    [[nodiscard]] bool ensure_occlusion_pipelines();
    // Creates the motion vector pipelines on first use; false (logged once) when unavailable.
    [[nodiscard]] bool ensure_motion_pipelines();
    [[nodiscard]] Status ensure_buffer(rhi::BufferHandle& buffer, std::uint32_t& capacity, std::uint32_t bytes,
                                       std::uint32_t usage, const char* name);

    GpuContext& ctx_;
    TransientTexturePool pool_;
    rhi::PipelineHandle opaque_cull_, opaque_double_, shadow_pipeline_, shadow_alpha_, sky_pipeline_, line_pipeline_;
    rhi::PipelineHandle prepass_cull_, prepass_double_;
    rhi::PipelineHandle particle_alpha_, particle_additive_, ribbon_alpha_, ribbon_additive_;
    std::array<rhi::PipelineHandle, 3> decal_pipelines_{}; // by DecalBlend
    rhi::PipelineHandle water_pipeline_, water_depth_pipeline_;
    rhi::PipelineHandle skin_cull_, skin_double_, skin_prepass_cull_, skin_prepass_double_, skin_shadow_;
    // DebugView::Overdraw: [skinned][double_sided] counters and the persistent RGBA8 count target.
    rhi::PipelineHandle overdraw_[2][2]{};
    rhi::TextureHandle overdraw_tex_{};
    std::uint32_t overdraw_w_{0}, overdraw_h_{0};
    bool overdraw_valid_{false};
    std::unordered_map<std::string, rhi::PipelineHandle> post_; // "<shader>/<format>/<blend>"
    // Built-in PBR pipeline descriptions, reused for surface shader variants.
    rhi::PipelineDesc opaque_desc_, skinned_desc_;
    // Surface shader pipelines by shader handle: [skinned * 2 + double_sided]; all invalid
    // when creation failed (the built-in pipeline is used and the failure logged once).
    std::unordered_map<std::uint64_t, std::array<rhi::PipelineHandle, 12>> surface_pipelines_;
    std::unordered_set<std::uint64_t> surface_failed_; // shaders whose pipeline failed (logged once)
    std::array<rhi::PipelineHandle, 8> blend_pipelines_{}; // [blend-1][skinned][double_sided], created on first use
    std::optional<double> time_;
    RenderPathFacts facts_;
    RenderPathReport path_;
    rhi::TextureHandle no_shadow_;
    rhi::TextureHandle exposure_[2];
    std::uint32_t exposure_index_{0};
    rhi::TextureHandle history_[2];
    std::uint32_t history_w_[2]{}, history_h_[2]{};
    std::uint32_t history_index_{0};
    bool history_valid_{false};
    math::Mat4 prev_view_proj_{math::Mat4::identity()};
    std::uint64_t frame_index_{0};
    bool adaptation_valid_{false};
    float frame_time_{1.0f / 60.0f};
    RenderSettings settings_;
    rhi::BufferHandle line_buffer_;
    std::uint32_t line_capacity_{0};
    rhi::BufferHandle instance_buffer_;
    std::uint32_t instance_capacity_{0}; // in instances
    rhi::BufferHandle palette_buffer_;
    std::uint32_t palette_capacity_{0}; // in matrices
    rhi::ComputePipelineHandle hiz_depth_, hiz_reduce_, occlusion_cull_;
    bool occlusion_failed_{false};
    rhi::BufferHandle hiz_buffer_, occl_bounds_, occl_args_, occl_instances_; // capacities in bytes
    std::uint32_t hiz_capacity_{0}, occl_bounds_capacity_{0}, occl_args_capacity_{0}, occl_instances_capacity_{0};
    std::uint32_t occl_batches_{0}; // batches culled by the last frame (for read_occlusion_survivors)
    std::vector<math::Mat4> palette_;
    rhi::PipelineHandle motion_cull_, motion_double_, skin_motion_cull_, skin_motion_double_;
    bool motion_failed_{false};
    rhi::BufferHandle motion_buffer_;
    std::uint32_t motion_capacity_{0}; // in instances
    std::unordered_map<std::uint64_t, MotionState> motion_prev_, motion_next_;
    // LOD hysteresis: level drawn last frame per (motion id, mesh), for the output size below.
    std::unordered_map<std::uint64_t, std::uint8_t> lod_prev_, lod_next_;
    std::uint32_t lod_history_w_{0}, lod_history_h_{0};
    bool lod_history_valid_{false};
    std::vector<math::Mat4> motion_joints_, motion_joints_next_;
    rhi::Format depth_format_{rhi::Format::D32_FLOAT};
    rhi::Format hdr_format_{rhi::Format::RGBA16_FLOAT};

    Camera3D camera_;
    DirectionalLight sun_;
    Environment env_;
    std::vector<PointLight> points_;
    std::vector<SpotLight> spots_;
    rhi::BufferHandle light_buffer_, cluster_buffer_;
    std::uint32_t light_capacity_{0}, cluster_capacity_{0}; // bytes
    std::vector<Draw> draws_;
    std::vector<ReflectionProbe> probes_;
    std::uint32_t probe_requests_{0}, probe_unbaked_{0}, probe_over_budget_{0}; // render-path report (ADR-0105)
    std::vector<Decal> decals_;
    std::vector<WaterSurface> waters_;
    const IrradianceVolume* volume_{nullptr};
    float volume_intensity_{1.0f}, volume_normal_offset_{0.0f};
    bool capture_mode_{false};
    std::vector<LineVertex> lines_;
    std::vector<ParticleInstance> particles_, particle_upload_;
    std::vector<ParticleBatch> particle_batches_;
    // Per-frame draw lists, batches and upload staging, kept between frames so render() reuses their
    // capacity instead of allocating (and, for the large ones, mapping fresh pages) every frame.
    struct FrameScratch;
    std::unique_ptr<FrameScratch> scratch_;
    rhi::BufferHandle particle_buffer_;
    std::uint32_t particle_capacity_{0}; // in particles
    std::vector<GpuParticleEmitter> gpu_emitters_; // this frame's submissions
    std::unordered_map<std::uint64_t, GpuParticleState> gpu_particle_states_;
    rhi::ComputePipelineHandle particle_sim_{};
    bool particle_sim_failed_{false};
    std::uint64_t gpu_particle_frame_{0}; // render() calls (state garbage collection)
    // Volumetric fog: compute pipelines (created on first use), injected froxels (ping-pong:
    // this frame / history) and the integrated grid, plus what the history was rendered with.
    [[nodiscard]] bool ensure_fog_pipelines();
    std::vector<FogVolume> fog_volumes_;
    rhi::ComputePipelineHandle fog_inject_{}, fog_integrate_{};
    bool fog_failed_{false};
    rhi::BufferHandle fog_froxels_[2]{}, fog_integrated_{};
    std::uint32_t fog_froxel_capacity_[2]{}, fog_integrated_capacity_{0};
    std::uint32_t fog_index_{0};
    std::array<std::uint32_t, 3> fog_dims_{};
    float fog_far_{0.0f};
    bool fog_history_valid_{false};
    math::Mat4 fog_prev_view_proj_{math::Mat4::identity()};
    math::Vec3 fog_prev_position_{}, fog_prev_forward_{0, 0, -1};
    std::uint64_t fog_frame_{0};
    std::array<math::Mat4, 4> cascade_vp_{math::Mat4::identity(), math::Mat4::identity(), math::Mat4::identity(),
                                          math::Mat4::identity()};
    std::array<float, 4> cascade_splits_{};
    std::array<float, 4> cascade_texel_{};
    std::uint32_t cascade_count_{0};
    SceneStats stats_;
    CompiledGraphInfo graph_info_;
};

} // namespace victus::render
