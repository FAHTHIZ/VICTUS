// SPDX-License-Identifier: Apache-2.0
// Forward HDR 3D renderer scheduled through the render graph: cascaded directional shadow
// maps, optional depth pre-pass + SSAO, metallic-roughness PBR, point lights, procedural sky,
// debug lines, dual-filter bloom, auto exposure, tonemapping/grading and FXAA. Every feature
// scales through RenderSettings / QualityTier so the same content runs on phones and desktops.
#pragma once
#include "victus/render/environment_map.hpp"
#include "victus/render/gpu_context.hpp"
#include "victus/render/render_graph.hpp"

#include <array>
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
    math::Vec4 params;        // x rotation (radians), yzw reserved
};
struct ParticleMaterial {
    rhi::TextureHandle texture; // invalid => built-in soft disc
    ParticleBlend blend{ParticleBlend::Alpha};
    float softness{0.25f};      // metres over which particles fade into geometry
};

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
};

// Scalability tiers. Low targets entry-level phones, Medium mid-range phones/tablets, High
// desktops/consoles/flagship phones, Ultra high-end desktops.
enum class QualityTier : std::uint8_t { Low, Medium, High, Ultra };
[[nodiscard]] std::string_view quality_tier_name(QualityTier t) noexcept;
[[nodiscard]] std::optional<QualityTier> parse_quality_tier(std::string_view name) noexcept;

struct RenderSettings {
    QualityTier tier{QualityTier::High};
    float render_scale{1.0f};             // 3D resolution scale (0.25..2), upscaled bilinearly
    std::uint32_t shadow_cascades{3};     // 1..4, packed into a 2x2 atlas when > 1
    std::uint32_t shadow_resolution{2048}; // per cascade; also capped by DirectionalLight
    float cascade_split_lambda{0.8f};     // 0 uniform .. 1 logarithmic splits
    bool soft_shadows{true};              // 3x3 PCF (else one hardware 2x2 PCF tap)
    bool depth_prepass{false};            // forced on by SSAO
    bool ssao{true};
    bool ssao_half_resolution{true};
    std::uint32_t ssao_samples{8};        // 4..32
    bool bloom{true};
    std::uint32_t bloom_levels{5};        // 1..8 mip levels
    bool fxaa{true};                      // ignored while TAA is active
    // Temporal anti-aliasing: sub-pixel jitter + reprojected history (camera motion from depth,
    // moving objects bounded by neighbourhood clipping). Also stabilises SSAO and shadows.
    bool taa{true};                       // High/Ultra; Low/Medium use FXAA
    float taa_blend{0.1f};                // weight of the current frame (0.04..1)
    bool auto_exposure{true};             // allowed; scenes enable it through Environment
    bool instancing{true};                // merge identical mesh+material draws into instanced batches
    // Mesh levels of detail: each draw uses the coarsest level whose geometric error, projected
    // at the draw's distance, stays within `lod_error_pixels` render pixels (0.25..16).
    bool lods{true};
    float lod_error_pixels{1.0f};
    int force_lod{-1};                    // debugging: >= 0 draws that level (clamped per mesh)
    // GPU occlusion culling: when the depth pre-pass runs, a compute pass builds a Hi-Z pyramid
    // from it and tests every opaque main-pass instance; survivors are drawn with indirect draws.
    // Conservative: the image is identical with it off. Needs compute (RenderCaps::compute).
    bool occlusion_culling{true};
    // Local light shadows (ADR-0052): lights with cast_shadows get perspective shadow maps in one
    // atlas of local_shadow_atlas^2 texels split into local_shadow_tile^2 tiles (at most 8x8). Spot
    // lights take one tile, point lights six; lights covering more of the screen win the tiles.
    bool local_shadows{true};
    std::uint32_t local_shadow_atlas{2048}; // 256..8192
    std::uint32_t local_shadow_tile{512};   // 64..atlas
    std::uint32_t max_point_lights{512};  // local (point + spot) lights per frame, <= SceneRenderer::kMaxLights

    [[nodiscard]] static RenderSettings preset(QualityTier tier) noexcept;
    // Clamps every field into its supported range.
    [[nodiscard]] RenderSettings sanitized() const noexcept;
};

struct SceneStats {
    std::uint32_t submitted{0}, visible{0}, culled{0}, shadow_casters{0}, lines{0};
    std::uint32_t point_lights{0};      // local lights used (point + spot)
    std::uint32_t spot_lights{0};
    std::uint32_t light_indices{0};     // clustered light index list length
    std::uint32_t max_cluster_lights{0}; // most lights in one cluster
    std::uint32_t shadow_cascades{0}, shadow_draws{0}, prepass_draws{0};
    std::uint32_t render_width{0}, render_height{0};
    bool taa{false};                   // TAA resolved this frame
    bool taa_history{false};           // ... with a valid history (false on the first frame / after a reset)
    math::Vec2 jitter{};               // sub-pixel jitter in render pixels
    // Draw calls issued per pass after instancing (a batch = one draw_indexed with N instances).
    std::uint32_t main_batches{0}, shadow_batches{0}, prepass_batches{0};
    std::uint32_t transparent_draws{0};
    std::uint32_t reflection_probes{0}, probe_draws{0}; // probes used / visible draws lit by a probe
    std::uint32_t instances_uploaded{0};
    std::uint32_t skinned_draws{0};    // skinned submissions this frame
    std::uint32_t joints_uploaded{0};  // joint matrices in the per-frame palette
    std::uint32_t particle_batches{0}; // visible particle batches (one draw each)
    std::uint32_t particles{0};        // particles drawn
    std::uint32_t lod_draws{0};        // visible draws using a coarser level than LOD 0
    std::uint64_t triangles{0};        // triangles in the main and transparent passes (all instances)
    // GPU occlusion culling this frame: instances tested on the GPU / main batches drawn
    // indirectly (survivor counts stay on the GPU; see SceneRenderer::read_occlusion_survivors).
    bool occlusion{false};
    // Local light shadows: shadowed lights, atlas tiles rendered, caster draws over all tiles.
    std::uint32_t local_shadow_lights{0}, local_shadow_tiles{0}, local_shadow_draws{0};
    std::uint32_t occlusion_tested{0}, occlusion_batches{0};
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
    void reset_history() noexcept { history_valid_ = false; }

    void begin(const Camera3D& camera, const DirectionalLight& sun, const Environment& env);
    void add_point_light(const PointLight& light);
    void add_spot_light(const SpotLight& light);
    void submit(const GpuMesh& mesh, const Material& material, const math::Mat4& model);
    // Skinned draw (GPU skinning). `joints` are skin matrices in the mesh's model space
    // (inverse(model) * joint_world * inverse_bind); `world_bounds` must contain the posed mesh.
    // Falls back to submit() when the mesh has no skin stream or no joints are given. Skinned
    // instances of one mesh+material are still batched into one instanced draw.
    void submit_skinned(const GpuMesh& mesh, const Material& material, const math::Mat4& model,
                        std::span<const math::Mat4> joints, const math::Aabb& world_bounds);
    // One batch of billboards sharing a material (e.g. one emitter); `bounds` (world space,
    // including particle sizes) is used for frustum culling and batch ordering.
    void submit_particles(std::span<const ParticleInstance> particles, const ParticleMaterial& material,
                          const math::Aabb& bounds);
    // Local reflection probe: draws whose bounds centre lies inside `box` use the probe's cube
    // (box-projected) for reflections and its SH for diffuse light instead of the scene
    // environment; the smallest containing box wins. The map must outlive the frame.
    void add_reflection_probe(const ReflectionProbe& probe);
    void line(math::Vec3 a, math::Vec3 b, math::Color color);
    void box(const math::Aabb& box, math::Color color);
    void grid(float half_size, float spacing, math::Color color);

    // Renders into `target` (any colour-target format; sRGB encoding is applied for UNORM targets).
    [[nodiscard]] Status render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width,
                                std::uint32_t height, rhi::Format target_format);

    [[nodiscard]] const SceneStats& stats() const noexcept { return stats_; }
    // Debug/test readback (waits for the GPU): instances that passed the last frame's occlusion
    // test, summed over its culled batches. 0 when the last frame did not cull.
    [[nodiscard]] Result<std::uint32_t> read_occlusion_survivors();
    [[nodiscard]] const CompiledGraphInfo& graph_info() const noexcept { return graph_info_; }
    // Cascade 0 matrix (identity when shadows are off).
    [[nodiscard]] const math::Mat4& light_view_projection() const noexcept { return cascade_vp_[0]; }
    [[nodiscard]] const std::array<math::Mat4, 4>& cascade_view_projections() const noexcept { return cascade_vp_; }
    [[nodiscard]] const std::array<float, 4>& cascade_splits() const noexcept { return cascade_splits_; }

private:
    explicit SceneRenderer(GpuContext& ctx) : ctx_(ctx), pool_(ctx.device()) {}
    struct Draw {
        const GpuMesh* mesh;
        Material material;
        math::Mat4 model;
        float distance;
        std::uint64_t batch_key; // equal keys => same mesh and identical material
        std::uint32_t palette{0}; // first joint matrix in palette_ (skinned only)
        bool skinned{false};
        std::uint8_t lod{0};      // level of detail picked for this frame
        math::Aabb world_bounds;  // skinned only (static draws use mesh bounds * model)
        int probe{-1};            // reflection probe index (-1: scene environment)
    };
    // A run of consecutive instances in the per-frame instance buffer sharing one mesh/material.
    struct Batch {
        const Draw* first;
        std::uint32_t instance_offset, instance_count;
    };
    struct ParticleBatch {
        ParticleMaterial material;
        std::uint32_t offset, count;
        math::Aabb bounds;
        float distance{0};
    };
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
    [[nodiscard]] Status ensure_buffer(rhi::BufferHandle& buffer, std::uint32_t& capacity, std::uint32_t bytes,
                                       std::uint32_t usage, const char* name);

    GpuContext& ctx_;
    TransientTexturePool pool_;
    rhi::PipelineHandle opaque_cull_, opaque_double_, shadow_pipeline_, sky_pipeline_, line_pipeline_;
    rhi::PipelineHandle prepass_cull_, prepass_double_;
    rhi::PipelineHandle particle_alpha_, particle_additive_;
    rhi::PipelineHandle skin_cull_, skin_double_, skin_prepass_cull_, skin_prepass_double_, skin_shadow_;
    std::unordered_map<std::string, rhi::PipelineHandle> post_; // "<shader>/<format>/<blend>"
    // Built-in PBR pipeline descriptions, reused for surface shader variants.
    rhi::PipelineDesc opaque_desc_, skinned_desc_;
    // Surface shader pipelines by shader handle: [skinned * 2 + double_sided]; all invalid
    // when creation failed (the built-in pipeline is used and the failure logged once).
    std::unordered_map<std::uint64_t, std::array<rhi::PipelineHandle, 12>> surface_pipelines_;
    std::unordered_set<std::uint64_t> surface_failed_; // shaders whose pipeline failed (logged once)
    std::array<rhi::PipelineHandle, 8> blend_pipelines_{}; // [blend-1][skinned][double_sided], created on first use
    std::optional<double> time_;
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
    bool capture_mode_{false};
    std::vector<LineVertex> lines_;
    std::vector<ParticleInstance> particles_, particle_upload_;
    std::vector<ParticleBatch> particle_batches_;
    rhi::BufferHandle particle_buffer_;
    std::uint32_t particle_capacity_{0}; // in particles
    std::array<math::Mat4, 4> cascade_vp_{math::Mat4::identity(), math::Mat4::identity(), math::Mat4::identity(),
                                          math::Mat4::identity()};
    std::array<float, 4> cascade_splits_{};
    std::array<float, 4> cascade_texel_{};
    std::uint32_t cascade_count_{0};
    SceneStats stats_;
    CompiledGraphInfo graph_info_;
};

} // namespace victus::render
