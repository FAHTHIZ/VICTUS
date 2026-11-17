// SPDX-License-Identifier: Apache-2.0
// Batched 2D sprite renderer. World space is Y-up; sprites are sorted by layer, then texture,
// preserving submission order within a layer/texture run.
#pragma once
#include "victus/render/gpu_context.hpp"

#include <vector>

namespace victus::render {

struct Camera2D {
    math::Vec2 position;
    float zoom{1.0f};
    float rotation{0.0f};
    float pixels_per_unit{1.0f};
    [[nodiscard]] math::Mat4 view_projection(float viewport_width, float viewport_height) const noexcept;
    // Viewport pixel (origin top-left) to world position.
    [[nodiscard]] math::Vec2 screen_to_world(math::Vec2 pixel, float viewport_width, float viewport_height) const noexcept;
    // World position to viewport pixel (origin top-left); inverse of screen_to_world.
    [[nodiscard]] math::Vec2 world_to_screen(math::Vec2 world, float viewport_width, float viewport_height) const noexcept;
};

struct Sprite {
    rhi::TextureHandle texture; // invalid => white
    math::Vec2 position;
    math::Vec2 size{1, 1};
    math::Vec2 origin{0.5f, 0.5f}; // pivot in normalized sprite space
    float rotation{0.0f};          // radians, counter-clockwise
    math::Vec4 uv{0, 0, 1, 1};     // u0, v0 (top), u1, v1 (bottom)
    math::Color color{1, 1, 1, 1};
    std::int32_t layer{0};
    bool flip_x{false}, flip_y{false};
    bool additive{false}; // additive blending (glows, sparks); sorted after alpha sprites of a layer
    bool nearest{false};  // point sampling for this sprite (pixel art); begin(.., true) forces it for all
    // Multiplied by the 2D light at its pixels while lighting is enabled (set_lighting). Additive
    // sprites are emissive and never lit; editor overlays leave this off.
    bool lit{false};
};

// 2D light (ADR-0065), accumulated into a light target that lit sprites multiply with. Point lights
// shine in all directions, spot lights in a cone around `direction`; both fall off smoothly to zero
// at `radius`.
struct Light2D {
    enum class Type : std::uint8_t { Point, Spot };
    Type type{Type::Point};
    math::Vec2 position;
    math::Color color{1, 1, 1, 1};
    float intensity{1.0f};
    float radius{5.0f};              // world units
    math::Vec2 direction{0, -1};     // spot axis (normalized on use)
    float inner_angle{0.35f};        // spot half-angles in radians: full light inside inner,
    float outer_angle{0.6f};         // fading to none at outer
    float falloff{1.0f};             // exponent on the smooth window: < 1 broader, > 1 tighter
};

struct SpriteBatchStats {
    std::uint32_t sprites{0};
    std::uint32_t batches{0};
    std::uint32_t lights{0};      // 2D lights drawn into the light target
    std::uint32_t lit_sprites{0}; // sprites multiplied by the light target
    bool lighting{false};         // the frame used 2D lighting
};

class SpriteBatch {
public:
    [[nodiscard]] static Result<std::unique_ptr<SpriteBatch>> create(GpuContext& ctx, rhi::Format color_format,
                                                                     rhi::BlendMode blend = rhi::BlendMode::Alpha);
    ~SpriteBatch();
    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    // Clears sprites and lights; 2D lighting starts disabled.
    void begin(const math::Mat4& view_projection, bool pixel_art = false);
    // Enables 2D lighting for this frame: lit sprites are multiplied by `ambient` plus the lights.
    void set_lighting(math::Color ambient);
    void add_light(const Light2D& light);
    [[nodiscard]] bool lighting() const noexcept { return lighting_; }
    void draw(const Sprite& sprite);
    void draw_rect(math::Vec2 center, math::Vec2 size, math::Color color, std::int32_t layer = 0, float rotation = 0);
    void draw_line(math::Vec2 a, math::Vec2 b, float thickness, math::Color color, std::int32_t layer = 0);
    // Sorts, builds geometry and uploads it. Must be called outside a render pass.
    [[nodiscard]] Status prepare(rhi::CommandBuffer& cmd);
    // With lighting enabled, renders the light target (width x height = the colour target's size).
    // Call after prepare() and outside a render pass; a no-op without lighting.
    [[nodiscard]] Status render_lights(rhi::CommandBuffer& cmd, std::uint32_t width, std::uint32_t height);
    // Draws the prepared batches into an active render pass whose colour format matches.
    void render(rhi::RenderPass& pass, rhi::CommandBuffer& cmd);

    [[nodiscard]] const SpriteBatchStats& stats() const noexcept { return stats_; }

private:
    explicit SpriteBatch(GpuContext& ctx) : ctx_(ctx) {}
    struct Vertex {
        math::Vec2 pos;
        math::Vec2 uv;
        std::uint32_t color;
    };
    struct Batch {
        rhi::TextureHandle texture;
        std::uint32_t first_index, index_count;
        bool additive{false};
        bool nearest{false};
        bool lit{false};
    };
    [[nodiscard]] Status ensure_capacity(std::uint32_t sprites);

    GpuContext& ctx_;
    rhi::PipelineHandle pipeline_, additive_pipeline_, lit_pipeline_, light_pipeline_;
    rhi::TextureHandle light_target_;
    std::uint32_t light_w_{0}, light_h_{0};
    bool lighting_{false};
    bool lights_ready_{false}; // render_lights() filled the light target this frame
    math::Color ambient_{1, 1, 1, 1};
    std::vector<Light2D> lights_;
    rhi::BufferHandle vertex_buffer_, index_buffer_;
    std::uint32_t capacity_{0};
    math::Mat4 view_proj_;
    bool pixel_art_{false};
    std::vector<Sprite> sprites_;
    std::vector<std::uint32_t> order_;
    std::vector<Vertex> vertices_;
    std::vector<Batch> batches_;
    SpriteBatchStats stats_;
};

} // namespace victus::render
