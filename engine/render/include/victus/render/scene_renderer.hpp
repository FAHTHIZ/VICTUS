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

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
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

// Vertex shaders compiled from a surface shader's `vertex()` function (surface_vertex_source(),
// ADR-0170), one per pipeline family. Owned by whoever compiled them (runtime::GpuAssetCache);
// a Material points at them while they are alive.
enum class SurfaceVertexVariant : std::uint8_t { Static, Skinned, Lightmapped, Shadow, ShadowSkinned };
inline constexpr std::size_t kSurfaceVertexVariants = 5;
struct SurfaceVertexStage {
    std::array<rhi::ShaderHandle, kSurfaceVertexVariants> shaders{}; // indexed by SurfaceVertexVariant
    [[nodiscard]] rhi::ShaderHandle get(SurfaceVertexVariant v) const noexcept { return shaders[static_cast<std::size_t>(v)]; }
};

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
    // World-space triplanar mapping (ADR-0197): textures are projected along the world axes and
    // blended by the surface normal instead of using the mesh's uvs, and uv_scale becomes repeats
    // per metre. Suits scaled blocks, rocks and level geometry without good uvs. The projection
    // is in world space, so a moving object's texture slides over it.
    bool triplanar{false};
    float triplanar_sharpness{4.0f}; // blend exponent between projections (1 soft .. 64 hard)
    // Clear coat (ADR-0203, KHR_materials_clearcoat): a glossy dielectric layer over the base
    // (lacquer, car paint, glazed ceramic, varnish). 0 = none; it has its own roughness and
    // follows the vertex normal, so a normal-mapped base keeps a smooth reflection on top.
    float clearcoat{0.0f};
    float clearcoat_roughness{0.03f};
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
    // Project vertex function (ADR-0170): displaces vertices in every pass (forward, shadows) with
    // the built-in fragment shader or `surface_shader`. Not owned; null = the built-in vertex stage.
    const SurfaceVertexStage* vertex_stage{nullptr};
    // Textures declared by the surface shader (`uniform sampler2D name;`, ADR-0171), by slot;
    // bound to both stages of `surface_shader` / `vertex_stage`. `shader_texture_count` must equal
    // the number the shader declares (the asset cache fills missing ones with the declared
    // default), and invalid handles bind white.
    std::array<rhi::TextureHandle, 4> shader_textures{};
    std::uint8_t shader_texture_count{0};
    // Vertex alpha normally multiplies opacity. As a coverage mask it instead marks where the
    // scene's coverage layer may settle (alpha >= 0.5) and opacity ignores it.
    bool vertex_alpha_coverage_mask{false};
    // How strongly the scene's coverage layer (Environment::coverage_*) settles on this material,
    // 0 (never, e.g. water or glowing embers) .. 1.
    float coverage_response{1.0f};
    // Ambient occlusion from metallic_roughness_texture's red channel (glTF ORM packing), 0 = off.
    float occlusion_strength{0.0f};
    MaterialBlend blend{MaterialBlend::Opaque};
    // Blended only (ADR-0187): where this surface covers a pixel, the TAA reactive mask is raised
    // by responsive * coverage, so the resolve trusts the current frame there (blended draws have
    // no motion vectors and would otherwise ghost when they move). 0 = off, the default.
    float responsive{0.0f};
};

enum class FogMode : std::uint8_t { None, Linear, Exponential, ExponentialSquared };
[[nodiscard]] std::string_view fog_mode_name(FogMode m) noexcept;
[[nodiscard]] std::optional<FogMode> parse_fog_mode(std::string_view name) noexcept;

// Named declarations at file scope of a surface shader (ADR-0171), Godot-style:
//   uniform float wind : hint_range(0, 2) = 0.5;   // -> shader_params slot 0, .x
//   uniform vec3 tint : source_color = vec3(1, 0.5, 0);
//   uniform sampler2D noise : hint_default_black;  // -> shader texture slot 0
// Parameters take the four shader_params slots in order (float .x, vec2 .xy, vec3 .xyz, vec4);
// textures take up to four texture slots (one on GLES). Both work in surface() and vertex().
inline constexpr std::size_t kMaxSurfaceParams = 4, kMaxSurfaceTextures = 4, kMaxSurfaceTexturesGles = 1;
enum class SurfaceTextureDefault : std::uint8_t { White, Black, Normal };
struct SurfaceParamDecl {
    std::string name;
    std::uint8_t components{1}; // 1..4
    std::uint8_t slot{0};
    math::Vec4 value{};          // declared default
    bool color{false};           // source_color
    bool has_range{false};
    float min{0.0f}, max{1.0f}, step{0.0f};
    std::uint32_t line{0};
};
struct SurfaceTextureDecl {
    std::string name;
    std::uint8_t slot{0};
    SurfaceTextureDefault fallback{SurfaceTextureDefault::White};
    std::uint32_t line{0};
};
struct SurfaceDeclarations {
    std::vector<SurfaceParamDecl> params;
    std::vector<SurfaceTextureDecl> textures;
    std::string error; // empty = valid
    std::uint32_t error_line{0};
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
[[nodiscard]] SurfaceDeclarations parse_surface_declarations(std::string_view code);

// The built-in PBR fragment shader with a project surface function spliced in (GLSL 450 that
// defines `void surface(SurfaceInput i, inout Surface s)`; see engine/render/shaders/pbr.frag).
// `#line` directives keep compiler messages on the user's own line numbers.
[[nodiscard]] std::string surface_shader_source(std::string_view surface_code);
// Whether surface shader code defines `void surface(...)` / `void vertex(...)` (comments ignored).
[[nodiscard]] bool surface_code_has_function(std::string_view code, std::string_view name);
// `code` with the definition of function `name` blanked out (newlines kept, so compiler line
// numbers stay the user's); unchanged when it is not defined.
[[nodiscard]] std::string surface_code_without_function(std::string_view code, std::string_view name);
// The surface vertex template (engine/render/shaders/templates/surface_vertex.glsl) for
// `variant` with `surface_code`'s vertex function spliced in (its surface function removed).
[[nodiscard]] std::string surface_vertex_source(std::string_view surface_code, SurfaceVertexVariant variant);
// The unmodified pbr.frag source.
[[nodiscard]] std::string_view pbr_fragment_source() noexcept;

struct DirectionalLight {
    math::Vec3 direction{-0.4f, -1.0f, -0.3f}; // direction the light travels
    math::Color color{1.0f, 0.96f, 0.9f, 1.0f};
    float intensity{3.0f};
    bool shadows{true};
    float shadow_distance{40.0f};
    std::uint32_t shadow_resolution{2048};
    // Light cookie (ADR-0201): key of an image registered with SceneRenderer::set_cookie, 0 = none.
    // The image tiles across the plane facing the light, `cookie_size` metres per repeat, its
    // left-to-right along `cookie_right`; its colour multiplies the sunlight (canopy dapples,
    // window frames, caustics). `cookie_offset` scrolls it (metres along right / up).
    std::uint64_t cookie{0};
    float cookie_size{10.0f};
    math::Vec3 cookie_right{1, 0, 0};
    float cookie_offset_x{0.0f};
    float cookie_offset_y{0.0f};
};

struct PointLight {
    math::Vec3 position;
    math::Color color{1, 1, 1, 1};
    float intensity{10.0f};
    float range{10.0f};
    // Radius of the emitting sphere in metres (0 = a point). Larger bulbs give wider, softer
    // highlights with the same total energy (representative point, Karis 2013).
    float radius{0.0f};
    bool cast_shadows{false}; // six atlas tiles (cube faces) when within RenderSettings' budget
    // Light cookie (ADR-0201): key of an image registered with SceneRenderer::set_cookie, 0 = none.
    // The image wraps around the light as a latitude-longitude panorama: its centre column faces
    // `forward`, its left-to-right turns towards `right`, its top row points along
    // up = cross(right, forward). Its colour multiplies the light (lamp shades, projector cages).
    std::uint64_t cookie{0};
    math::Vec3 forward{0, 0, -1};
    math::Vec3 right{1, 0, 0};
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
    // Light cookie (ADR-0195): key of an image registered with SceneRenderer::set_cookie, 0 = none.
    // The image is projected through the cone, its top along `up` = cross(right, direction), its
    // left-to-right along `right`; its colour multiplies the light (black blocks it).
    std::uint64_t cookie{0};
    math::Vec3 right{1, 0, 0};
};

// One-sided rectangular area light (ADR-0194), like a window, screen or softbox. It emits along
// `direction` from a `width` x `height` rectangle centred on `position` whose width runs along
// `right`. Diffuse light is the exact form factor of the rectangle with horizon clipping
// approximated by a sphere; specular uses the representative point with energy normalisation.
// `intensity` matches a point light seen straight on, so swapping one for the other keeps the
// brightness; `range` is measured from the rectangle. Not shadowed in real time (bake it).
struct AreaLight {
    math::Vec3 position;
    math::Vec3 direction{0, -1, 0}; // direction light travels (the lit side)
    math::Vec3 right{1, 0, 0};      // width axis, made perpendicular to `direction`
    math::Color color{1, 1, 1, 1};
    float intensity{10.0f};
    float range{10.0f};
    float width{1.0f}, height{1.0f};
};
// Packs an area light's frame into GpuLight form: an angle around `direction` relative to the
// branchless orthonormal basis of Duff et al. 2017 (the shader rebuilds `right` from it).
[[nodiscard]] float area_light_basis_angle(math::Vec3 direction, math::Vec3 right) noexcept;
[[nodiscard]] math::Vec3 area_light_right(math::Vec3 direction, float angle) noexcept;

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
    // Trails (ADR-0110): each slot keeps a ring of `trail_points` positions recorded on the GPU and
    // is drawn as a camera-facing ribbon (ribbon.vert) behind its particle. 0 = no trails.
    std::uint32_t trail_points{0};            // 2..64 requested; reduced to fit kMaxGpuTrailSegments
    math::Vec4 trail;                         // x lifetime (s), y min distance (m), z head width (x size), w tail width (fraction)
    float trail_alpha_end{0};                 // tail alpha as a fraction of the particle's
    bool trail_billboards{true};              // also draw the particles themselves
    // Over-life curves (ADR-0209): with color_keys > 0 the colour is the piecewise-linear gradient
    // through color_key[i] at color_time[i] (replacing color_start/end); with size_keys > 0 the size
    // is size.x x the curve through (size_time[i], size_value[i]) (replacing size.x -> size.y).
    std::uint32_t color_keys{0}, size_keys{0}; // <= 8 each, times ascending
    std::array<math::Vec4, 8> color_key{};
    std::array<float, 8> color_time{}, size_time{}, size_value{};
    // Motion multipliers over life (ADR-0211; particles::speed_at / spin_at): 0 keys = 1.
    std::uint32_t speed_keys{0}, spin_keys{0};
    std::array<float, 8> speed_time{}, speed_value{}, spin_time{}, spin_value{};
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
    // Heterogeneous media (ADR-0186): procedural 3D noise breaks height fog and fog volumes into
    // drifting patches, keeping the mean density.
    float noise{0.0f};             // 0 uniform .. 1 patches with clear gaps
    float noise_scale{6.0f};       // metres per noise feature (0.25..1000)
    math::Vec3 wind{0.6f, 0.0f, 0.25f}; // metres per second the noise drifts
    // Self-shadowing (ADR-0186): the fog between a froxel and the sun dims the sun light it
    // scatters (RenderSettings::volumetric_fog_shadow_steps samples towards the sun).
    float self_shadow{0.0f};       // 0 off .. 1 physical optical depth (up to 4 exaggerates)
    float self_shadow_distance{16.0f}; // metres marched towards the sun (1..200)
};

// A box of extra fog for this frame: `transform` maps the unit cube [-0.5, 0.5]^3 to the world.
struct FogVolume {
    math::Mat4 transform{math::Mat4::identity()};
    float density{0.1f};            // extinction per metre added inside
    math::Color albedo{1, 1, 1, 1};
    float edge_fade{0.1f};          // fade-in width as a fraction of the box size (0 hard .. 0.5)
};
inline constexpr std::size_t kMaxFogVolumes = 8;

// Volumetric clouds (ADR-0111): a horizontal layer between `bottom` and `top` (world heights,
// metres; flat, no planet curvature) with procedural density, ray marched at reduced resolution
// and composited over the sky and any geometry behind the layer. Lit by the directional light
// and the sky ambient. Needs RenderSettings::volumetric_clouds (off on Low).
struct VolumetricClouds {
    bool enabled{false};
    float coverage{0.5f};          // 0 clear .. 1 overcast
    float density{0.03f};          // extinction per metre inside a fully dense cloud
    float bottom{1500.0f};         // layer base height (m)
    float top{3500.0f};            // layer top height (m), > bottom
    float shape_size{4000.0f};     // feature size of the cloud shapes (m)
    float detail_size{600.0f};     // feature size of the eroding detail noise (m)
    float detail{0.35f};           // edge erosion strength 0..1
    math::Vec3 wind{10.0f, 0.0f, 4.0f}; // m/s; shapes drift by wind * SceneRenderer time
    float anisotropy{0.6f};        // forward scattering (silver lining towards the sun) 0..0.95
    float sun_intensity{1.0f};     // light multipliers
    float ambient_intensity{1.0f};
    float distance{30000.0f};      // metres marched along a view ray (fades out over the last 40%)
    // Cloud shadows on lit opaque and blended surfaces below the layer (ADR-0112): 0 none .. 1
    // full transmittance of the sun through the clouds. Drawn whenever the clouds are.
    float shadow_strength{1.0f};
};

// Rain or snow falling in a box that follows the camera (ADR-0113). Drops are placed and moved
// procedurally on the GPU from their index (no simulation state), stay fixed in the world while
// the camera moves, are tested against the scene depth and fade towards the box edges and the
// camera. Drawn after particles; RenderSettings::max_precipitation drops at intensity 1.
enum class PrecipitationType : std::uint8_t { None, Rain, Snow };
struct Precipitation {
    PrecipitationType type{PrecipitationType::None};
    float intensity{1.0f};          // 0..1, fraction of RenderSettings::max_precipitation drawn
    math::Vec3 wind{0.0f, 0.0f, 0.0f}; // m/s added to the fall velocity (slanted rain, drifting snow)
    float radius{15.0f};            // half size of the box around the camera (m), 2..100
    float fall_speed{0.0f};         // m/s; 0 = 9 for rain, 1.2 for snow
    float size{0.0f};               // drop width / flake diameter (m); 0 = 0.006 rain, 0.025 snow
    float streak{0.03f};            // rain streak length in seconds of motion
    math::Color color{1, 1, 1, 1};  // tint (linear); lit by the sky ambient and the sun
    float opacity{0.0f};            // 0 = 0.35 rain, 0.9 snow
    float brightness{1.0f};         // light multiplier
    float ripples{1.0f};            // rain ripples on puddles and wet flat ground (ADR-0117), 0..1 x intensity
    float splashes{1.0f};           // rain splashes where drops land (ADR-0118; needs the occlusion map), 0..1
};

struct Environment {
    math::Color sky_zenith{0.18f, 0.36f, 0.75f, 1};
    math::Color sky_horizon{0.62f, 0.72f, 0.85f, 1};
    math::Color ground{0.22f, 0.2f, 0.18f, 1};
    float ambient_intensity{0.6f};
    float exposure{1.0f};          // manual exposure multiplier (ignored by auto exposure)
    float vignette{0.25f};
    // Camera-lens imperfections (ADR-0146), all off by default: barrel distortion (-1..1, > 0 barrel),
    // lateral chromatic aberration (0..1) and luminance-weighted animated sensor grain (0..1).
    float lens_distortion{0.0f};
    float chromatic_aberration{0.0f};
    float film_grain{0.0f};
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
    // Sun contact shadows (ADR-0183): screen-space ray length in metres (0 = off) and the assumed
    // thickness of depth-buffer occluders. They fade out with the AO fade distance.
    float contact_shadow_length{0.0f};
    float contact_shadow_thickness{0.25f};
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
    VolumetricClouds clouds{};
    Precipitation precipitation{};
    // Wet surfaces (ADR-0113): 0 dry .. 1 soaked. Lit opaque and blended surfaces darken where
    // they are rough and porous and get smoother (Lagarde 2013); metals only get smoother.
    float wetness{0.0f};
    // Puddles (ADR-0117): 0 none .. 1 every flat, upward face under water. Patches of standing
    // water about `puddle_size` metres across, fully wet and mirror smooth; rain ripples them.
    float puddles{0.0f};
    float puddle_size{3.0f};
    // Baked lighting (ADR-0122): the scene's lightmap atlas (RGBA16F irradiance / pi, see
    // lightmap.hpp). Meshes created with GpuContext::create_lightmapped_mesh sample it in place of
    // the probe/sky ambient; everything else is unaffected. Must outlive the frames that use it.
    rhi::TextureHandle lightmap{};
    // Directional lightmap (ADR-0130, create_lightmap_direction_texture): lets normal maps shade
    // the baked light. Optional; without it baked light ignores the per-pixel normal.
    rhi::TextureHandle lightmap_direction{};
    // Baked sun shadowmask (ADR-0131): the direction of the sun the lightmap's alpha was baked
    // for (LightmapBake::shadowmask_direction), zero = none. Lightmapped pixels beyond the
    // realtime shadow distance take the sun's shadow from it; ignored while the sun points
    // elsewhere (more than ~1 degree off) or casts no shadows.
    math::Vec3 lightmap_shadowmask_direction{0, 0, 0};
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
    // Screen-space sun contact shadows (ADR-0183), marched in the SSAO pass when the scene's
    // Environment sets a contact_shadow_length; forces the depth prepass like SSAO.
    bool contact_shadows{true};
    std::uint32_t contact_shadow_steps{12}; // 4..32 depth taps per pixel
    // Screen-space reflections (ADR-0196): water reflects the opaque scene by marching the
    // reflected ray through the depth buffer, falling back to the environment where the ray
    // leaves the screen. Off on Low; 16 / 32 / 48 steps on Medium / High / Ultra.
    bool screen_space_reflections{true};
    std::uint32_t ssr_steps{32};    // 4..64 ray steps per pixel
    float ssr_max_distance{60.0f};  // metres along the reflected ray (1..500)
    float ssr_thickness{0.3f};      // metres a surface is assumed to extend behind its depth (0.01..10)
    // ADR-0199: opaque and blended PBR surfaces smoother than this also trace screen-space
    // reflections, against last frame's TAA history (so only with TAA, High and Ultra), with half
    // of ssr_steps (at least 8). 0 turns opaque reflections off; water keeps its own trace.
    float ssr_max_roughness{0.5f};
    bool bloom{true};
    std::uint32_t bloom_levels{5};        // 1..8 mip levels
    bool fxaa{true};                      // ignored while TAA is active
    // Multisample anti-aliasing of the opaque forward pass (ADR-0152): 1 (off), 2, 4 or 8 samples,
    // used only without TAA and where the GPU supports the HDR and depth formats at that count.
    // The pass renders into multisampled colour and depth that are resolved into the scene colour
    // at its end (on tiled mobile GPUs the samples stay in tile memory); a depth pre-pass then
    // provides the single-sample depth that decals, water, particles and rain sample. Low: 4.
    std::uint32_t msaa_samples{1};
    // With MSAA, alpha-tested materials turn their cut-out alpha into sample coverage, sharpened
    // to about one pixel, instead of discarding whole pixels (anti-aliased leaf and grass edges).
    bool alpha_to_coverage{true};
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
    // With `taa` off: temporal upscaling takes over from MSAA/FXAA while the render scale (set
    // directly or lowered by dynamic resolution) is below this, and hands back once it is 0.05
    // above it (ADR-0188; Low uses 0.65, measured by tools/render/measure_low_aa.py). 0 = never.
    // Needs `temporal_upscale`.
    float taa_below_scale{0.0f};
    float taa_blend{0.1f};                // weight of the current frame (0.04..1)
    // Temporal upscaling (ADR-0055): with TAA on and render_scale < 1 (or dynamic resolution
    // lowering it), the resolve reconstructs at output resolution from jittered samples instead of
    // upscaling the internal image bilinearly. Off: the TAA history stays at render resolution.
    bool temporal_upscale{true};
    // Texture mip bias while temporally upscaling (ADR-0141): material textures are sampled with
    // log2(render / output size) + upscale_mip_bias, so the jittered low-resolution samples carry
    // output-resolution texture detail into the history. 0 = render-resolution mips (blurry),
    // FSR2/Arm ASR add -1 more (measured noisier with no FLIP gain here, so 0). Clamped to -2..0.
    float upscale_mip_bias{0.0f};
    // Object motion vectors (ADR-0054): opaque draws submitted with a motion id whose transform or
    // joints changed since the previous frame are drawn into a motion buffer the TAA resolve
    // follows, so moving and animated objects keep their history instead of ghosting. TAA only.
    bool motion_vectors{true};
    // Reactive mask (ADR-0143, FSR2/Arm ASR "reactive"): particles, water and precipitation also
    // write an R8 mask of how much of each pixel they cover; the TAA and temporal upscaling resolves
    // trust the current frame there (up to `reactive_max` of its weight), so effects without motion
    // vectors stop smearing and trailing. TAA only; costs one R8 target while such effects are drawn.
    bool reactive_mask{true};
    float reactive_max{0.5f}; // current-frame weight at a fully reactive pixel (0..1, ADR-0143)
    // Depth-based disocclusion (ADR-0147, FSR2 "depth clip" in gather form): the resolves keep a
    // linear depth history and trust the current frame where the reprojected surface was hidden
    // behind something nearer last frame, so uncovered background whose colours match the occluder
    // does not ghost. 0 off .. 1 full; costs one RG16F target per history (shared with the locks).
    float taa_disocclusion{1.0f};
    // Thin-feature locks (ADR-0148, FSR2's locks): pixels whose luminance is a ridge in their 3x3
    // neighbourhood (wires, fences, railings narrower than a pixel) keep their history unclipped for
    // one jitter period, so sub-pixel features stop breaking into flickering dashes. 0 off .. 1 full.
    float taa_thin_features{1.0f};
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
    // LOD cross-fade (ADR-0184): when a tracked draw (motion id) changes level, the old and new
    // levels are drawn together for this many frames with complementary screen-door dithering, so
    // the switch dissolves instead of popping (Unity's LOD Cross Fade, Unreal's dithered LOD
    // transition). Opaque per-frame draws only; shadows switch at once. 0 disables (0..64).
    std::uint32_t lod_fade_frames{8};
    // Shadow casters (directional cascades and local shadow tiles) draw this many levels coarser
    // than the level picked for the main camera (0..3, clamped per mesh). The main-camera pick
    // plus this bias is the finest level any shadow pass draws, so shadows and the lit surface
    // keep the same reference; directional cascades may go coarser still (shadow_lod_texels).
    // The bias only trades shadow-pass triangles for silhouette error. Low: 1.
    std::uint32_t shadow_lod_bias{0};
    // Alpha-tested draws (leaf and needle cards, grass, ferns) use this many levels coarser than
    // the main-camera pick (0..3, clamped per mesh); shadows add shadow_lod_bias on top. Card
    // canopies (ADR-0151) then thin out to fewer, larger cards nearer the camera, trading leaf
    // density for triangles and alpha-tested overdraw. Low: 1.
    std::uint32_t foliage_lod_bias{0};
    // Scattered foliage (ADR-0150): each FoliageComponent draws instances within
    // `draw_distance * foliage_distance_scale` (0.25..1), and only those within
    // `foliage_shadow_fraction` (0..1) of that range cast shadows; the rest draw lit but
    // shadowless, so the shadow pass skips the far grass that a 1024 shadow map cannot resolve.
    // Instance counts fall with the square of the scale. Low: 0.7 / 0.5, Medium: 0.85 / 0.75.
    float foliage_distance_scale{1.0f};
    float foliage_shadow_fraction{1.0f};
    // Per-cascade shadow LOD (ADR-0132): each directional cascade draws a caster no finer than
    // the coarsest level whose geometric error stays within this many of the cascade's shadow
    // texels, and never finer than the level above. A 1-texel error is inside the 1-texel
    // normal offset and the 3x3 PCF footprint, so the shadow keeps its shape while the wide
    // cascades drop to coarse levels. 0 disables (0..8). Local light tiles keep shadow_lod.
    float shadow_lod_texels{1.0f};
    // Directional cascades skip casters whose shadow cannot fall on a pixel the cascade shades:
    // the caster's light-space box is tested against the light-space bounds of the cascade's
    // view-depth slice (plus its transition band and filter footprint), not just the cascade's
    // bounding-sphere box. Lossless; off only to compare against the unculled image.
    bool shadow_receiver_culling{true};
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
    // their box. Off on Low (runtime cubemap capture, 18 extra texture fetches per lit pixel).
    bool irradiance_volumes{true};
    // Probes baked with the lightmap (ADR-0133, set_irradiance_volume(..., baked = true)) light
    // dynamic objects even where irradiance_volumes is off: they cost no capture, and lightmapped
    // pixels never sample them (ADR-0138).
    bool baked_probes{true};
    // Spherical-harmonic bands sampled from probe volumes: 2 = L2 (9 coefficients, 18 fetches),
    // 1 = L1 (4 coefficients, 8 fetches; Low). Unity's URP offers the same choice.
    std::uint32_t probe_sh_bands{2};
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
    // draw, particle billboard and ribbon (ADR-0212) is rasterised again without depth testing and
    // counted per pixel, shown as a heat ramp
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
    // Samples towards the sun for VolumetricFog::self_shadow (ADR-0186; 0 disables, max 16).
    // Medium 2, High 4, Ultra 6.
    std::uint32_t volumetric_fog_shadow_steps{4};
    // Volumetric clouds (ADR-0111; scenes enable them through Environment::clouds): the cloud
    // target is the render resolution divided by `cloud_downsample` (1..8), marched with
    // `cloud_steps` samples per ray (8..128) and `cloud_light_steps` towards the sun (1..16).
    // Low: off. Medium 4 / 32 / 4, High 2 / 48 / 6, Ultra 2 / 64 / 8.
    bool volumetric_clouds{true};
    std::uint32_t cloud_downsample{2};
    std::uint32_t cloud_steps{48};
    std::uint32_t cloud_light_steps{6};
    // Rain and snow (ADR-0113; scenes enable them through Environment::precipitation): drops
    // drawn at intensity 1 (0..SceneRenderer::kMaxPrecipitation). Low 2048, Medium 8192, High
    // 16384, Ultra 32768.
    bool precipitation{true};
    std::uint32_t max_precipitation{16384};
    // Rain occlusion map size (ADR-0114; 0 = off, else 64..4096): a depth map along the fall
    // direction that stops drops and wetness under roofs. Low 256, Medium 512, High and Ultra 1024.
    std::uint32_t precipitation_occlusion{1024};
    // Effect budgets per frame (ADR-0109), nearest first. Decals: Low 128, Medium 256, High 512,
    // Ultra 1024 (SceneRenderer::kMaxDecals). Water surfaces: Low 4, Medium 8, High and Ultra 16
    // (kMaxWaterSurfaces). CPU particles (billboards plus ribbon vertices): Low 16384, Medium
    // 65536, High 262144, Ultra 1048576; batches are kept whole from the nearest, and only the
    // nearest batch is cut when it alone is over. GPU emitters are budgeted by max_particles.
    std::uint32_t max_decals{512};
    std::uint32_t max_water_surfaces{16};
    std::uint32_t max_particles{262144};

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
    // The share drawn by particle billboards and ribbons (ADR-0212): pixels they touch, their
    // layers (included in `layers`) and the most particle layers in one pixel.
    std::uint32_t particle_pixels{0};
    std::uint64_t particle_layers{0};
    std::uint32_t particle_max_layers{0};
    // Mean layers per covered pixel (1.0 = no overdraw); 0 when nothing was drawn.
    [[nodiscard]] double average() const noexcept {
        return covered_pixels ? static_cast<double>(layers) / static_cast<double>(covered_pixels) : 0.0;
    }
    // Mean particle layers per pixel particles touch; 0 without particles.
    [[nodiscard]] double particle_average() const noexcept {
        return particle_pixels ? static_cast<double>(particle_layers) / static_cast<double>(particle_pixels) : 0.0;
    }
    // Particle fragments per render pixel: 1.0 = the effects filled the screen once.
    [[nodiscard]] double particle_fill() const noexcept {
        const std::uint64_t n = std::uint64_t{width} * height;
        return n ? static_cast<double>(particle_layers) / static_cast<double>(n) : 0.0;
    }
};

struct SceneStats {
    std::uint32_t submitted{0}, visible{0}, culled{0}, shadow_casters{0}, lines{0};
    // Caster/cascade pairs inside a cascade's box that receiver culling skipped (their shadow
    // cannot reach a pixel that samples that cascade; RenderSettings::shadow_receiver_culling).
    std::uint32_t shadow_receiver_culled{0};
    std::uint32_t anisotropy{1}; // anisotropic filtering level applied to material textures this frame
    std::uint32_t point_lights{0};      // local lights used (point + spot)
    std::uint32_t spot_lights{0};
    std::uint32_t area_lights{0};
    std::uint32_t cookie_lights{0};  // spot and point lights drawn with a cookie
    std::uint32_t missing_cookies{0}; // lights (sun included) whose cookie key was never registered (drawn without)
    bool sun_cookie{false};            // the directional light is drawn with a cookie (ADR-0201)
    std::uint32_t light_indices{0};     // clustered light index list length
    std::uint32_t max_cluster_lights{0}; // most lights in one cluster
    std::uint32_t shadow_cascades{0}, shadow_draws{0}, prepass_draws{0};
    std::uint32_t render_width{0}, render_height{0};
    bool taa{false};                   // TAA resolved this frame
    bool taa_by_scale{false};          // ... because the render scale fell below taa_below_scale (ADR-0188)
    bool opaque_ssr{false};            // ADR-0199: opaque surfaces traced screen-space reflections
    bool taa_history{false};           // ... with a valid history (false on the first frame / after a reset)
    bool taa_disocclusion{false};      // ... with the depth-history disocclusion test (ADR-0147)
    bool taa_thin_features{false};     // ... with thin-feature locks (ADR-0148)
    bool temporal_upscale{false};      // ... reconstructed at output resolution from a lower render resolution
    bool spatial_upscale{false};       // ADR-0066: resampled to the output by the Catmull-Rom + RCAS pass
    bool sharpened{false};             // the upscale/sharpen pass ran this frame
    std::uint32_t msaa_samples{1};     // ADR-0152: samples per pixel of the opaque forward pass this frame
    std::uint32_t irradiance_probes{0}; // baked probes of the irradiance volume used this frame (0 = none)
    math::Vec2 jitter{};               // sub-pixel jitter in render pixels
    float texture_mip_bias{0.0f};      // material texture LOD bias this frame (ADR-0141; < 0 when upscaling)
    bool reactive_mask{false};         // ADR-0143: transparent effects wrote the TAA reactive mask this frame
    std::uint32_t reactive_mesh_draws{0}; // ADR-0187: responsive blended mesh batches drawn into the mask
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
    // Retained instance groups (ADR-0162): clusters submitted / clusters passing the main-view
    // frustum test / instances those clusters draw in the main pass.
    std::uint32_t group_clusters{0}, group_clusters_visible{0}, group_instances_drawn{0};
    std::uint32_t skinned_draws{0};    // skinned submissions this frame
    std::uint32_t joints_uploaded{0};  // joint matrices in the per-frame palette
    std::uint32_t particle_batches{0}; // visible particle batches (one draw each)
    std::uint32_t particles{0};        // particles drawn
    std::uint32_t ribbon_vertices{0};  // trail triangle-list vertices drawn (six per segment)
    // GPU particles: emitters simulated / live particles of the drawn ones / spawns uploaded.
    std::uint32_t gpu_particle_emitters{0}, gpu_particles{0}, gpu_particle_spawns{0};
    // Alpha-blended GPU emitters drawn back to front this frame, and the compute passes it took.
    std::uint32_t gpu_particle_sorted_emitters{0}, gpu_particle_sort_passes{0};
    // GPU emitters drawn with trails (ADR-0110), their ribbon vertices (two per trail point slot,
    // including collapsed unused ones) and trailed emitters whose points were reduced to fit.
    std::uint32_t gpu_particle_trail_emitters{0}, gpu_particle_trail_vertices{0}, gpu_particle_trails_reduced{0};
    std::uint32_t lod_draws{0};        // visible draws using a coarser level than LOD 0
    std::uint32_t lod_held{0};         // draws kept finer than the stateless pick by lod_hysteresis
    std::uint32_t lod_fading{0};       // draws cross-fading between two levels (each drawn twice)
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
    // Volumetric clouds this frame (ADR-0111): cloud target size and samples per ray (0 when off).
    bool volumetric_clouds{false};
    std::uint32_t cloud_width{0}, cloud_height{0}, cloud_steps{0};
    bool cloud_shadows{false};         // the sun light of lit surfaces is shadowed by the clouds (ADR-0112)
    std::uint32_t precipitation_drops{0}; // rain drops / snow flakes drawn (ADR-0113)
    bool surface_wetness{false};       // lit surfaces were shaded wet
    std::uint32_t precipitation_occlusion{0}; // rain occlusion map size (0 when off, ADR-0114)
    std::uint32_t precipitation_occluders{0}; // opaque draws rendered into it
    bool surface_puddles{false};       // flat surfaces may hold puddles (ADR-0117)
    bool rain_ripples{false};          // rain ripples animated on wet flat surfaces
    std::uint32_t precipitation_splashes{0}; // rain splash crowns drawn (ADR-0118)
};

struct SceneRendererOptions {
    // Store the HDR scene colour, bloom chain and TAA history as R11G11B10_FLOAT (4 bytes/pixel) instead of
    // RGBA16_FLOAT (8): half the framebuffer bandwidth on tile-based mobile GPUs. No alpha and no negative
    // values, neither of which the HDR chain uses. Falls back to RGBA16_FLOAT when unsupported.
    bool compact_hdr{false};
    // Explicit FP16 BRDF maths in the built-in PBR shader (pbr.frag#fp16, ADR-0137) where the device
    // has shaderFloat16; ignored elsewhere. Surface shaders stay FP32.
    bool half_precision{false};
};

class SceneRenderer {
public:
    static constexpr std::uint32_t kMaxLights = 4096;
    static constexpr std::uint32_t kMaxPointLights = kMaxLights; // former name
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>> create(GpuContext& ctx, const SceneRendererOptions& options = {});
    [[nodiscard]] rhi::Format hdr_format() const noexcept { return hdr_format_; }
    // True when the built-in PBR shader runs its BRDF in FP16 (ADR-0137).
    [[nodiscard]] bool half_precision() const noexcept { return half_precision_; }
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
    void add_area_light(const AreaLight& light);
    void add_spot_light(const SpotLight& light);
    // Light cookies (ADR-0195). The image is resampled to kCookieSize x kCookieSize RGBA8 (sRGB) and
    // kept on the GPU until removed; `key` is any non-zero id (the runtime uses the texture UUID).
    // Registering the same key again replaces the image. Fails when kMaxCookies are registered.
    static constexpr std::uint32_t kCookieSize = 256;
    static constexpr std::uint32_t kMaxCookies = 64;
    // `version` is the caller's revision of the image (e.g. bumped on re-import); cookie_version()
    // returns it, so callers re-register only when it changed.
    Status set_cookie(std::uint64_t key, const Image& image, std::uint64_t version = 0);
    void remove_cookie(std::uint64_t key);
    [[nodiscard]] bool has_cookie(std::uint64_t key) const noexcept { return cookie_slots_.contains(key); }
    [[nodiscard]] std::optional<std::uint64_t> cookie_version(std::uint64_t key) const noexcept {
        const auto it = cookie_slots_.find(key);
        return it == cookie_slots_.end() ? std::nullopt : std::optional<std::uint64_t>(it->second.version);
    }
    [[nodiscard]] std::size_t cookie_count() const noexcept { return cookie_slots_.size(); }
    // `motion_id` (non-zero, unique per object and stable across frames, e.g. an entity id) lets
    // the renderer compare the draw with the previous frame and write object motion vectors for
    // TAA; 0 = no tracking (the object is treated as static, camera motion only).
    // `lightmap_uv_offset` shifts a lightmapped mesh's atlas uvs for this instance, so congruent
    // static copies baked into different atlas places share one mesh and batch (ADR-0135).
    void submit(const GpuMesh& mesh, const Material& material, const math::Mat4& model, std::uint64_t motion_id = 0,
                math::Vec2 lightmap_uv_offset = {});
    // Many static copies of one mesh+material (foliage, scattered props): equivalent to calling
    // submit() once per model with motion_id 0, but the material is hashed once and the draw list
    // grows once. The copies are still culled, LOD-selected and shadowed per instance and merge
    // into instanced batches like any other identical draws.
    void submit_instances(const GpuMesh& mesh, const Material& material, std::span<const math::Mat4> models);
    // Retained instance groups (ADR-0162): many static copies of one mesh+material registered once.
    // The copies are ordered along a Morton curve and split into clusters of `cluster_size`;
    // their per-instance data lives in a GPU buffer written once (on the next render after
    // creation). Each frame submit_instance_group() costs one cull, LOD pick and probe pick per
    // cluster instead of per instance, and nothing is re-uploaded. Trade-offs: a visible cluster
    // draws all its copies (no per-instance frustum culling inside it), every copy of a cluster
    // uses the cluster's LOD (picked from the cluster bounds and its largest-scale copy), and
    // group draws are not GPU occlusion culled and write no object motion vectors (static).
    // The mesh must outlive the group. Groups are owned by this renderer and die with it.
    struct InstanceGroupDesc {
        const GpuMesh* mesh{nullptr};
        Material material;
        std::span<const math::Mat4> models;
        std::span<const math::Vec2> lightmap_offsets; // empty, or one per model (ADR-0135)
        std::uint32_t cluster_size{64};               // clamped to [1, 4096]
    };
    Result<std::uint32_t> create_instance_group(const InstanceGroupDesc& desc); // id > 0
    void destroy_instance_group(std::uint32_t id);
    // Per-submission distance limits, tested per cluster against the nearest point of its bounds:
    // clusters farther than `max_distance` are skipped, those farther than `shadow_distance` draw
    // lit but cast no shadow (ADR-0164). Returns the instances submitted (0 for unknown ids).
    struct InstanceGroupDraw {
        float max_distance{std::numeric_limits<float>::infinity()};
        float shadow_distance{std::numeric_limits<float>::infinity()};
    };
    std::uint32_t submit_instance_group(std::uint32_t id, const InstanceGroupDraw& draw);
    std::uint32_t submit_instance_group(std::uint32_t id); // no distance limits
    // Keyed cache over instance groups for callers that rebuild their copies rarely (foliage,
    // scattered props): returns the group for `key`, rebuilding it with `build` only when the
    // key is new or `revision`, the mesh or the material changed. Groups not requested for
    // 120 begin() calls are destroyed. Returns 0 when building fails or yields no copies.
    std::uint32_t cached_instance_group(std::uint64_t key, std::uint64_t revision, const GpuMesh& mesh,
                                        const Material& material,
                                        const std::function<void(std::vector<math::Mat4>&)>& build,
                                        std::uint32_t cluster_size = 64);
    std::uint32_t cached_instance_groups() const noexcept { return static_cast<std::uint32_t>(group_cache_.size()); }
    std::uint32_t instance_group_clusters(std::uint32_t id) const noexcept;
    // Skinned draw (GPU skinning). `joints` are skin matrices in the mesh's model space
    // (inverse(model) * joint_world * inverse_bind); `world_bounds` must contain the posed mesh.
    // Falls back to submit() when the mesh has no skin stream or no joints are given. Skinned
    // instances of one mesh+material are still batched into one instanced draw.
    void submit_skinned(const GpuMesh& mesh, const Material& material, const math::Mat4& model,
                        std::span<const math::Mat4> joints, const math::Aabb& world_bounds,
                        std::uint64_t motion_id = 0);
    // Skinned draw from a palette that already lives on the GPU (crowds, ADR-0200): `palette` is a
    // storage buffer of Mat4 skin matrices (BufferUsageStorageRead) owned by the caller and kept
    // alive until the frame is submitted; the mesh reads `joint_count` matrices from `first_joint`.
    // Nothing is uploaded per draw. Draws with the same mesh, material and palette buffer batch.
    void submit_skinned(const GpuMesh& mesh, const Material& material, const math::Mat4& model, rhi::BufferHandle palette,
                        std::uint32_t first_joint, std::uint32_t joint_count, const math::Aabb& world_bounds,
                        std::uint64_t motion_id = 0);
    // One batch of billboards sharing a material (e.g. one emitter); `bounds` (world space,
    // including particle sizes) is used for frustum culling and batch ordering.
    void submit_particles(std::span<const ParticleInstance> particles, const ParticleMaterial& material,
                          const math::Aabb& bounds);
    // A GPU-simulated emitter for this frame (see GpuParticleEmitter). Every submitted emitter is
    // simulated (also when off screen, so its state keeps up with the CPU schedule); visible ones
    // are drawn with the other particle batches. Not simulated in capture mode.
    static constexpr std::uint32_t kMaxGpuParticles = 1u << 20;
    // Trail segments one GPU emitter may keep (slots x points per slot); larger emitters get fewer
    // points per particle, and none when fewer than two fit (ADR-0110).
    static constexpr std::uint32_t kMaxGpuTrailSegments = 1u << 18;
    // Trail points per slot the renderer uses for `capacity` slots asking for `requested` (0 = off).
    [[nodiscard]] static std::uint32_t gpu_trail_points(std::uint32_t capacity, std::uint32_t requested) noexcept;
    void submit_gpu_particles(const GpuParticleEmitter& emitter);
    // GPU state of an emitter after the last render (synchronous readback; tests and tooling).
    [[nodiscard]] Result<std::vector<GpuParticleSpawn>> read_gpu_particles(std::uint64_t key);
    // The instances of an emitter in the order the last render drew them: back to front for
    // alpha-blended emitters (ADR-0107), slot order otherwise (synchronous readback; tests).
    [[nodiscard]] Result<std::vector<ParticleInstance>> read_gpu_particle_draw(std::uint64_t key);
    // The ribbon vertices of a trailed GPU emitter after the last render: per slot, (points + 1)
    // pairs of edge vertices from tail to head (position + signed half width, colour, tangent + v);
    // unused pairs collapse to zero width (synchronous readback; tests). Empty without trails.
    [[nodiscard]] Result<std::vector<ParticleInstance>> read_gpu_particle_trails(std::uint64_t key);
    // Trail points per slot of an emitter's GPU state (0 = none).
    [[nodiscard]] std::uint32_t gpu_particle_trail_points(std::uint64_t key) const;
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
    // Most rain drops / snow flakes drawn per frame (RenderSettings::max_precipitation).
    static constexpr std::uint32_t kMaxPrecipitation = 65536;
    // Highest roof above the camera that the rain occlusion map sees (m).
    static constexpr float kRainOcclusionHeight = 200.0f;
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
    // `baked`: the volume holds the lightmap's probes, drawn under RenderSettings::baked_probes
    // rather than irradiance_volumes (ADR-0138).
    void set_irradiance_volume(const IrradianceVolume* volume, float intensity = 1.0f, float normal_offset = 0.0f,
                               bool baked = false);
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
        std::uint32_t palette{0}; // first joint matrix in palette_ (skinned only), or in palette_src
        bool skinned{false};
        rhi::BufferHandle palette_src{}; // external baked palette (ADR-0200); null = this frame's palette_
        std::uint8_t lod{0};      // level of detail picked for this frame
        float lod_fade{0.0f};     // cross-fade (ADR-0184): > 0 keeps dither < f, < 0 keeps dither >= -f, 0 solid
        std::uint8_t shadow_lod{0}; // level used by the shadow passes (lod + shadow_lod_bias)
        std::array<std::uint8_t, 4> cascade_lod{}; // level per directional cascade (>= shadow_lod)
        math::Aabb world_bounds;  // skinned only (static draws use mesh bounds * model)
        int probe{-1};            // reflection probe index (-1: scene environment)
        std::uint64_t motion_id{0};
        std::uint32_t joint_count{0}; // skinned only
        math::Vec2 lightmap_offset{};  // lightmapped meshes: per-instance atlas uv shift (ADR-0135)
        std::int32_t group{-1};   // retained instance group slot (ADR-0162); world_bounds holds the cluster bounds
        std::uint32_t cluster_first{0}, cluster_count{0}; // its instances in the group buffer
        bool has_bounds() const noexcept { return skinned || group >= 0; }
    };
    // Storage buffer a skinned draw's palette offsets index: its baked palette or this frame's.
    [[nodiscard]] rhi::BufferHandle palette_of(const Draw& d) const noexcept { return d.palette_src ? d.palette_src : palette_buffer_; }
    // Previous-frame state of a tracked object (motion vectors).
    struct MotionState {
        math::Mat4 model;
        std::uint32_t joint_offset{0}, joint_count{0}; // into motion_joints_, or into palette_src when set
        rhi::BufferHandle palette_src{};
    };
    // A run of consecutive instances in the per-frame instance buffer sharing one mesh/material.
    struct Batch {
        const Draw* first;
        std::uint32_t instance_offset, instance_count;
        std::uint8_t lod{0}; // level drawn by every instance of the batch
        rhi::BufferHandle buffer{}; // retained group buffer (ADR-0162); empty = the per-frame instance buffer
    };
    struct InstanceCluster {
        math::Aabb bounds;
        math::Mat4 representative; // largest-scale copy (LOD and shadow LOD scale)
        std::uint32_t first, count;
    };
    struct InstanceGroup {
        const GpuMesh* mesh{nullptr};
        Material material;
        std::uint64_t key{0};
        std::vector<InstanceCluster> clusters;
        std::vector<std::uint8_t> pending; // instance data not yet uploaded
        rhi::BufferHandle buffer;
        std::uint32_t generation{0};
        bool live{false};
        std::uint64_t cache_key{0}, revision{0}, last_used{0}; // cached groups only
        std::uint32_t cluster_size{0};
        bool cached{false};
    };
    struct ParticleBatch {
        ParticleMaterial material;
        std::uint32_t offset, count;
        math::Aabb bounds;
        float distance{0};
        bool ribbon{false}; // triangle-list ribbon vertices rather than billboard instances
        rhi::BufferHandle buffer{}; // GPU particles: their instance buffer (offset 0, count slots)
        rhi::BufferHandle index{};  // GPU trails: index buffer of the ribbon (count indices)
    };
    struct GpuParticleState {
        rhi::BufferHandle state, instances, spawns;
        rhi::BufferHandle pairs, sorted; // depth sort (alpha emitters): (key, slot) pairs, draw order
        std::uint32_t sort_size{0};      // pairs allocated (power of two >= 256)
        bool sorted_last{false};         // the last render drew `sorted`
        rhi::BufferHandle trail, ribbon, ribbon_index; // trails (ADR-0110): point rings, edge vertices, indices
        std::uint32_t trail_points{0};   // points per slot of the allocated trail buffers
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
    Result<rhi::PipelineHandle> precipitation_pipeline(bool splash = false, bool reactive = false);
    // ADR-0143: particle/ribbon/water pipelines with the reactive mask as a second colour target,
    // created on first use. kind: 0 particles, 1 ribbons, 2 water.
    Result<rhi::PipelineHandle> reactive_pipeline(int kind, bool additive);
    // DebugView::Overdraw counting pipeline for particle billboards (kind 0) or ribbons (kind 1).
    Result<rhi::PipelineHandle> overdraw_particle_pipeline(int kind);
    // Mesh vertex stage + reactive_mesh.frag into the R8 mask (ADR-0187); cached in post_.
    Result<rhi::PipelineHandle> reactive_mesh_pipeline(std::uint32_t variant, bool double_sided);
    [[nodiscard]] Result<rhi::PipelineHandle> post_pipeline(const char* fragment, rhi::Format format,
                                                            rhi::BlendMode blend = rhi::BlendMode::Opaque,
                                                            rhi::Format second = rhi::Format::Invalid);
    void compute_cascades(float aspect, std::uint32_t resolution);
    // Pipeline for a project surface shader (created on first use); the built-in one on failure.
    // Vertex variants of the PBR pipelines: kVertexStatic (mesh.vert), kVertexSkinned
    // (mesh_skinned.vert) and kVertexLightmapped (mesh_lightmap.vert, ADR-0122).
    static constexpr std::uint32_t kVertexStatic = 0, kVertexSkinned = 1, kVertexLightmapped = 2, kVertexVariants = 3;
    // Pushes a vertex function's VertexMaterial block (time, shadow flag, shader_params) to vertex
    // uniform slot 1 (ADR-0170).
    void push_vertex_material(rhi::CommandBuffer& cmd, const Material& m, bool shadow) const;
    float shader_time_{0}; // seconds, as lighting.screen.z for this frame
    // Shadow-map pipeline for a material with a vertex function (ADR-0170): [skinned][alpha cut].
    [[nodiscard]] rhi::PipelineHandle surface_shadow_pipeline(const SurfaceVertexStage& stage, bool skinned, bool cut);
    [[nodiscard]] rhi::PipelineHandle surface_pipeline(rhi::ShaderHandle fragment, const SurfaceVertexStage* vertex, std::uint32_t variant, bool double_sided,
                                                       MaterialBlend blend);
    [[nodiscard]] rhi::PipelineHandle blend_pipeline(MaterialBlend blend, std::uint32_t variant, bool double_sided);
    // Forward pipelines remember their description so multisampled (and alpha-to-coverage)
    // copies can be made on first use (ADR-0152). An unknown base gives an invalid handle.
    void remember_forward(rhi::PipelineHandle pipeline, const rhi::PipelineDesc& desc);
    void forget_forward(rhi::PipelineHandle pipeline);
    [[nodiscard]] rhi::PipelineHandle multisampled(rhi::PipelineHandle base, std::uint32_t samples, bool alpha_to_coverage);
    [[nodiscard]] std::uint32_t supported_msaa(std::uint32_t samples);
    [[nodiscard]] const rhi::PipelineDesc& variant_desc(std::uint32_t variant) const noexcept;
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
    rhi::PipelineHandle lightmap_cull_, lightmap_double_;
    // DebugView::Overdraw: [skinned][double_sided] counters and the persistent RGBA8 count target.
    rhi::PipelineHandle overdraw_[2][2]{};
    rhi::TextureHandle overdraw_tex_{};
    std::uint32_t overdraw_w_{0}, overdraw_h_{0};
    bool overdraw_valid_{false};
    std::unordered_map<std::string, rhi::PipelineHandle> post_; // "<shader>/<format>/<blend>"
    // Built-in PBR pipeline descriptions, reused for surface shader variants.
    rhi::PipelineDesc opaque_desc_, skinned_desc_, lightmap_desc_;
    rhi::PipelineDesc shadow_desc_;                    // sun/local/rain shadow depth (static)
    rhi::ShaderHandle pbr_fragment_, shadow_fragment_, shadow_alpha_fragment_;
    // Vertex-function shadow pipelines by the stage's static shadow shader handle: [skinned * 2 + cut].
    std::unordered_map<std::uint64_t, std::array<rhi::PipelineHandle, 4>> surface_shadow_pipelines_;
    // Surface shader pipelines by shader handle: [blend][variant][double_sided]; all invalid
    // when creation failed (the built-in pipeline is used and the failure logged once).
    std::unordered_map<std::uint64_t, std::array<rhi::PipelineHandle, 4 * kVertexVariants * 2>> surface_pipelines_;
    std::unordered_set<std::uint64_t> surface_failed_; // shaders whose pipeline failed (logged once)
    std::array<rhi::PipelineHandle, 2 * kVertexVariants * 2> blend_pipelines_{}; // [additive][variant][double_sided], on first use
    std::unordered_map<std::uint64_t, rhi::PipelineDesc> forward_descs_;   // by pipeline handle
    std::unordered_map<std::uint64_t, rhi::PipelineHandle> msaa_pipelines_; // by base handle, bit 63: alpha to coverage
    std::uint32_t msaa_pipeline_samples_{0};
    std::array<std::int8_t, 9> msaa_support_{}; // by sample count: 0 unknown, 1 yes, -1 no
    std::optional<double> time_;
    RenderPathFacts facts_;
    RenderPathReport path_;
    rhi::TextureHandle no_shadow_;
    rhi::TextureHandle exposure_[2];
    std::uint32_t exposure_index_{0};
    rhi::TextureHandle history_[2];
    std::uint32_t history_w_[2]{}, history_h_[2]{};
    // ADR-0147: linear view depth of each history pixel (same size as history_), written by the resolve.
    rhi::TextureHandle depth_history_[2];
    rhi::Format depth_history_format_{rhi::Format::R16_FLOAT};
    // ADR-0143: reactive masks at render resolution, ping-ponged with the TAA history; last frame's
    // mask is reprojected so pixels a particle just left also trust the current frame (no trails).
    rhi::TextureHandle reactive_[2];
    std::uint32_t reactive_w_[2]{}, reactive_h_[2]{};
    bool reactive_valid_[2]{};
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
    std::vector<InstanceGroup> groups_; // slot i has id (generation << 16) | (i + 1)
    std::unordered_map<std::uint64_t, std::uint32_t> group_cache_; // cache key -> group id
    std::uint64_t group_frame_{0};                                   // begin() calls
    std::vector<math::Mat4> group_build_;                            // scratch for cached builds
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
    // LOD hysteresis history (ADR-0150): open addressing over a power-of-two table rebuilt each
    // frame without per-entry allocation (the node map it replaced allocated once per draw).
    struct LodHistory {
        std::vector<std::uint64_t> keys; // 0: empty slot (real keys are remapped away from 0)
        std::vector<std::uint8_t> lods;
        // Cross-fade state (ADR-0184): level faded from and frames into the fade (0 = not fading).
        std::vector<std::uint8_t> from, step;
        std::size_t count{0};
        struct Entry {
            int lod{-1};
            std::uint8_t from{0}, step{0};
        };
        static std::uint64_t slot_key(std::uint64_t k) noexcept { return k ? k : 0x9e3779b97f4a7c15ull; }
        void clear(std::size_t expected) {
            std::size_t cap = 16;
            while (cap < expected * 2) cap *= 2;
            if (keys.size() != cap) {
                keys.assign(cap, 0);
                lods.assign(cap, 0);
                from.assign(cap, 0);
                step.assign(cap, 0);
            } else if (count > 0) {
                std::fill(keys.begin(), keys.end(), 0);
            }
            count = 0;
        }
        // First insert of a key wins (as std::unordered_map::emplace).
        void insert(std::uint64_t k, std::uint8_t lod, std::uint8_t fade_from = 0, std::uint8_t fade_step = 0) noexcept {
            if (keys.empty() || (count + 1) * 2 > keys.size()) return; // sized by clear()
            k = slot_key(k);
            const std::size_t mask = keys.size() - 1;
            for (std::size_t i = static_cast<std::size_t>(k * 0x9e3779b97f4a7c15ull >> 17) & mask;; i = (i + 1) & mask) {
                if (keys[i] == k) return;
                if (keys[i] == 0) {
                    keys[i] = k;
                    lods[i] = lod;
                    from[i] = fade_from;
                    step[i] = fade_step;
                    ++count;
                    return;
                }
            }
        }
        [[nodiscard]] Entry entry(std::uint64_t k) const noexcept {
            if (count == 0) return {};
            k = slot_key(k);
            const std::size_t mask = keys.size() - 1;
            for (std::size_t i = static_cast<std::size_t>(k * 0x9e3779b97f4a7c15ull >> 17) & mask;; i = (i + 1) & mask) {
                if (keys[i] == k) return {lods[i], from[i], step[i]};
                if (keys[i] == 0) return {};
            }
        }
        [[nodiscard]] int find(std::uint64_t k) const noexcept { return entry(k).lod; }
    };
    LodHistory lod_prev_, lod_next_;
    std::vector<Draw> lod_fade_out_; // this frame's outgoing-level copies of cross-fading draws
    std::uint32_t lod_history_w_{0}, lod_history_h_{0};
    bool lod_history_valid_{false};
    std::vector<math::Mat4> motion_joints_, motion_joints_next_;
    rhi::Format depth_format_{rhi::Format::D32_FLOAT};
    rhi::Format hdr_format_{rhi::Format::RGBA16_FLOAT};
    bool compact_hdr_requested_{false};
    bool half_precision_requested_{false}, half_precision_{false};

    Camera3D camera_;
    DirectionalLight sun_;
    Environment env_;
    std::vector<PointLight> points_;
    std::vector<SpotLight> spots_;
    std::vector<AreaLight> areas_;
    // Cookie atlas (ADR-0195): kCookieSize^2 packed RGBA8 texels per slot, uploaded when dirty.
    struct CookieSlot {
        std::uint32_t slot{0};
        std::uint64_t version{0};
    };
    std::unordered_map<std::uint64_t, CookieSlot> cookie_slots_;
    std::vector<std::uint32_t> cookie_texels_;
    std::vector<std::uint32_t> cookie_free_;
    bool cookies_dirty_{true};
    rhi::BufferHandle cookie_buffer_;
    std::uint32_t cookie_capacity_{0}; // bytes
    rhi::BufferHandle light_buffer_, cluster_buffer_;
    std::uint32_t light_capacity_{0}, cluster_capacity_{0}; // bytes
    std::vector<Draw> draws_;
    std::vector<ReflectionProbe> probes_;
    std::uint32_t probe_requests_{0}, probe_unbaked_{0}, probe_over_budget_{0}; // render-path report (ADR-0105)
    std::vector<Decal> decals_;
    std::vector<WaterSurface> waters_;
    const IrradianceVolume* volume_{nullptr};
    float volume_intensity_{1.0f}, volume_normal_offset_{0.0f};
    bool volume_baked_{false};
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
    rhi::ComputePipelineHandle particle_sort_{}; // optional: without it alpha GPU particles draw unsorted
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
    bool precipitation_failed_{false}; // precipitation pipeline failed to build (logged once)
    bool scale_taa_{false};            // TAA on because the render scale is below taa_below_scale (ADR-0188)
    bool reactive_failed_{false};      // a reactive-mask pipeline failed to build (logged once, ADR-0143)
    bool splash_failed_{false};        // rain splash pipeline failed to build (logged once)
    bool clouds_failed_{false};      // cloud pipelines failed to build: clouds stay off (logged once)
    std::uint64_t cloud_frame_{0};   // jitter sequence of the cloud march
    std::uint64_t post_frame_{0};    // film grain seed (ADR-0146), advances every rendered frame
    std::array<math::Mat4, 4> cascade_vp_{math::Mat4::identity(), math::Mat4::identity(), math::Mat4::identity(),
                                          math::Mat4::identity()};
    std::array<float, 4> cascade_splits_{};
    std::array<float, 4> cascade_texel_{};
    // Light-space bounds of the points each cascade shades (its view-depth slice plus the
    // dithered transition band taken from the previous cascade), widened by the filter and
    // normal-offset footprint. A caster whose light-space box misses this region in x/y, or lies
    // entirely downstream of it, cannot darken a pixel that samples the cascade (ADR-0124).
    std::array<math::Aabb, 4> cascade_receivers_{};
    // The same slices as camera frusta (planes pushed out by the margin): a caster's shadow volume
    // (its box swept along the sun down to the lowest receiver) must reach inside all six.
    std::array<math::Frustum, 4> cascade_slices_{};
    math::Mat4 cascade_light_view_{math::Mat4::identity()};
    math::Vec3 cascade_sun_dir_{0, -1, 0};
    std::uint32_t cascade_count_{0};
    SceneStats stats_;
    CompiledGraphInfo graph_info_;
};

} // namespace victus::render
