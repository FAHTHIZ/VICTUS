// SPDX-License-Identifier: Apache-2.0
// Forward HDR 3D renderer scheduled through the render graph: cascaded directional shadow
// maps, optional depth pre-pass + SSAO, metallic-roughness PBR, point lights, procedural sky,
// debug lines, dual-filter bloom, auto exposure, tonemapping/grading and FXAA. Every feature
// scales through RenderSettings / QualityTier so the same content runs on phones and desktops.
#pragma once
#include "victus/render/gpu_context.hpp"
#include "victus/render/render_graph.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct Material {
    math::Color base_color{1, 1, 1, 1}; // linear
    math::Color emissive{0, 0, 0, 1};   // linear rgb, multiplied by emissive_strength
    float emissive_strength{1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    float normal_scale{1.0f};
    float alpha_cutoff{0.0f}; // >0 enables alpha testing
    rhi::TextureHandle base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;
    bool double_sided{false};
    bool unlit{false};
    bool cast_shadows{true};
};

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
};

enum class Tonemapper : std::uint8_t { None, Aces, Neutral, Reinhard };
[[nodiscard]] std::string_view tonemapper_name(Tonemapper t) noexcept;
[[nodiscard]] std::optional<Tonemapper> parse_tonemapper(std::string_view name) noexcept;

// Per-scene look (lighting environment and post-processing artistic controls).
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
    bool fxaa{true};
    bool auto_exposure{true};             // allowed; scenes enable it through Environment
    bool instancing{true};                // merge identical mesh+material draws into instanced batches
    std::uint32_t max_point_lights{16};   // <= SceneRenderer::kMaxPointLights

    [[nodiscard]] static RenderSettings preset(QualityTier tier) noexcept;
    // Clamps every field into its supported range.
    [[nodiscard]] RenderSettings sanitized() const noexcept;
};

struct SceneStats {
    std::uint32_t submitted{0}, visible{0}, culled{0}, shadow_casters{0}, lines{0};
    std::uint32_t point_lights{0};
    std::uint32_t shadow_cascades{0}, shadow_draws{0}, prepass_draws{0};
    std::uint32_t render_width{0}, render_height{0};
    // Draw calls issued per pass after instancing (a batch = one draw_indexed with N instances).
    std::uint32_t main_batches{0}, shadow_batches{0}, prepass_batches{0};
    std::uint32_t instances_uploaded{0};
};

class SceneRenderer {
public:
    static constexpr std::uint32_t kMaxPointLights = 16;
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>> create(GpuContext& ctx);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void set_settings(const RenderSettings& settings) noexcept { settings_ = settings.sanitized(); }
    [[nodiscard]] const RenderSettings& settings() const noexcept { return settings_; }
    // Seconds since the previous frame (drives auto exposure adaptation). Default 1/60.
    void set_frame_time(float seconds) noexcept { frame_time_ = seconds; }
    // Forces auto exposure to jump to the current scene luminance on the next frame.
    void reset_adaptation() noexcept { adaptation_valid_ = false; }

    void begin(const Camera3D& camera, const DirectionalLight& sun, const Environment& env);
    void add_point_light(const PointLight& light);
    void submit(const GpuMesh& mesh, const Material& material, const math::Mat4& model);
    void line(math::Vec3 a, math::Vec3 b, math::Color color);
    void box(const math::Aabb& box, math::Color color);
    void grid(float half_size, float spacing, math::Color color);

    // Renders into `target` (any colour-target format; sRGB encoding is applied for UNORM targets).
    [[nodiscard]] Status render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width,
                                std::uint32_t height, rhi::Format target_format);

    [[nodiscard]] const SceneStats& stats() const noexcept { return stats_; }
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
    };
    // A run of consecutive instances in the per-frame instance buffer sharing one mesh/material.
    struct Batch {
        const Draw* first;
        std::uint32_t instance_offset, instance_count;
    };
    struct LineVertex {
        math::Vec3 pos;
        std::uint32_t color;
    };
    [[nodiscard]] Result<rhi::PipelineHandle> post_pipeline(const char* fragment, rhi::Format format,
                                                            rhi::BlendMode blend = rhi::BlendMode::Opaque);
    void compute_cascades(float aspect, std::uint32_t resolution);
    [[nodiscard]] Status ensure_exposure_textures();

    GpuContext& ctx_;
    TransientTexturePool pool_;
    rhi::PipelineHandle opaque_cull_, opaque_double_, shadow_pipeline_, sky_pipeline_, line_pipeline_;
    rhi::PipelineHandle prepass_cull_, prepass_double_;
    std::unordered_map<std::string, rhi::PipelineHandle> post_; // "<shader>/<format>/<blend>"
    rhi::TextureHandle no_shadow_;
    rhi::TextureHandle exposure_[2];
    std::uint32_t exposure_index_{0};
    bool adaptation_valid_{false};
    float frame_time_{1.0f / 60.0f};
    RenderSettings settings_;
    rhi::BufferHandle line_buffer_;
    std::uint32_t line_capacity_{0};
    rhi::BufferHandle instance_buffer_;
    std::uint32_t instance_capacity_{0}; // in instances
    rhi::Format depth_format_{rhi::Format::D32_FLOAT};

    Camera3D camera_;
    DirectionalLight sun_;
    Environment env_;
    std::vector<PointLight> points_;
    std::vector<Draw> draws_;
    std::vector<LineVertex> lines_;
    std::array<math::Mat4, 4> cascade_vp_{math::Mat4::identity(), math::Mat4::identity(), math::Mat4::identity(),
                                          math::Mat4::identity()};
    std::array<float, 4> cascade_splits_{};
    std::array<float, 4> cascade_texel_{};
    std::uint32_t cascade_count_{0};
    SceneStats stats_;
    CompiledGraphInfo graph_info_;
};

} // namespace victus::render
