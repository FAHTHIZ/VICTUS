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
};

struct SpriteBatchStats {
    std::uint32_t sprites{0};
    std::uint32_t batches{0};
};

class SpriteBatch {
public:
    [[nodiscard]] static Result<std::unique_ptr<SpriteBatch>> create(GpuContext& ctx, rhi::Format color_format,
                                                                     rhi::BlendMode blend = rhi::BlendMode::Alpha);
    ~SpriteBatch();
    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    void begin(const math::Mat4& view_projection, bool pixel_art = false);
    void draw(const Sprite& sprite);
    void draw_rect(math::Vec2 center, math::Vec2 size, math::Color color, std::int32_t layer = 0, float rotation = 0);
    void draw_line(math::Vec2 a, math::Vec2 b, float thickness, math::Color color, std::int32_t layer = 0);
    // Sorts, builds geometry and uploads it. Must be called outside a render pass.
    [[nodiscard]] Status prepare(rhi::CommandBuffer& cmd);
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
    };
    [[nodiscard]] Status ensure_capacity(std::uint32_t sprites);

    GpuContext& ctx_;
    rhi::PipelineHandle pipeline_;
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
