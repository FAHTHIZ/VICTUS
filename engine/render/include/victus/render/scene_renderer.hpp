// SPDX-License-Identifier: Apache-2.0
// Forward HDR 3D renderer: directional shadow map, metallic-roughness PBR, point lights,
// procedural sky, debug lines and ACES tonemapping, scheduled through the render graph.
#pragma once
#include "victus/render/gpu_context.hpp"
#include "victus/render/render_graph.hpp"

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

struct Environment {
    math::Color sky_zenith{0.18f, 0.36f, 0.75f, 1};
    math::Color sky_horizon{0.62f, 0.72f, 0.85f, 1};
    math::Color ground{0.22f, 0.2f, 0.18f, 1};
    float ambient_intensity{0.6f};
    float exposure{1.0f};
    float vignette{0.25f};
    bool tonemap{true};
    bool draw_sky{true};
    math::Color clear_color{0.05f, 0.05f, 0.06f, 1}; // when draw_sky is false
};

struct SceneStats {
    std::uint32_t submitted{0}, visible{0}, culled{0}, shadow_casters{0}, lines{0};
    std::uint32_t point_lights{0};
};

class SceneRenderer {
public:
    static constexpr std::uint32_t kMaxPointLights = 16;
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>> create(GpuContext& ctx);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

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
    [[nodiscard]] const math::Mat4& light_view_projection() const noexcept { return light_vp_; }

private:
    explicit SceneRenderer(GpuContext& ctx) : ctx_(ctx), pool_(ctx.device()) {}
    struct Draw {
        const GpuMesh* mesh;
        Material material;
        math::Mat4 model;
        float distance;
    };
    struct LineVertex {
        math::Vec3 pos;
        std::uint32_t color;
    };
    [[nodiscard]] Result<rhi::PipelineHandle> tonemap_pipeline(rhi::Format format);
    void compute_shadow_matrix();

    GpuContext& ctx_;
    TransientTexturePool pool_;
    rhi::PipelineHandle opaque_cull_, opaque_double_, shadow_pipeline_, sky_pipeline_, line_pipeline_;
    std::unordered_map<std::uint8_t, rhi::PipelineHandle> tonemap_;
    rhi::TextureHandle no_shadow_;
    rhi::BufferHandle line_buffer_;
    std::uint32_t line_capacity_{0};
    rhi::Format depth_format_{rhi::Format::D32_FLOAT};

    Camera3D camera_;
    DirectionalLight sun_;
    Environment env_;
    std::vector<PointLight> points_;
    std::vector<Draw> draws_;
    std::vector<LineVertex> lines_;
    math::Mat4 light_vp_;
    SceneStats stats_;
    CompiledGraphInfo graph_info_;
};

} // namespace victus::render
