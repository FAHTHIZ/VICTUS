// SPDX-License-Identifier: Apache-2.0
#include "victus/render/sprite_batch.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace victus::render {
using namespace math;

Mat4 Camera2D::view_projection(float w, float h) const noexcept {
    const float scale = pixels_per_unit * (zoom <= 0 ? 1.0f : zoom);
    const float hw = w / (2.0f * scale), hh = h / (2.0f * scale);
    Mat4 proj = Mat4::orthographic(-hw, hw, -hh, hh, -100.0f, 100.0f);
    Mat4 view = Mat4::rotation(Quat::axis_angle({0, 0, 1}, -rotation)) * Mat4::translation({-position.x, -position.y, 0});
    return proj * view;
}

Vec2 Camera2D::screen_to_world(Vec2 pixel, float w, float h) const noexcept {
    const float scale = pixels_per_unit * (zoom <= 0 ? 1.0f : zoom);
    Vec2 local{(pixel.x - w * 0.5f) / scale, (h * 0.5f - pixel.y) / scale};
    const float c = std::cos(rotation), s = std::sin(rotation);
    return Vec2{local.x * c - local.y * s, local.x * s + local.y * c} + position;
}

Vec2 Camera2D::world_to_screen(Vec2 world, float w, float h) const noexcept {
    const float scale = pixels_per_unit * (zoom <= 0 ? 1.0f : zoom);
    const Vec2 d = world - position;
    const float c = std::cos(rotation), s = std::sin(rotation);
    const Vec2 local{d.x * c + d.y * s, -d.x * s + d.y * c};
    return {w * 0.5f + local.x * scale, h * 0.5f - local.y * scale};
}

Result<std::unique_ptr<SpriteBatch>> SpriteBatch::create(GpuContext& ctx, rhi::Format color_format, rhi::BlendMode blend) {
    std::unique_ptr<SpriteBatch> b(new SpriteBatch(ctx));
    auto vs = ctx.shader("sprite.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx.shader("sprite.frag");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc p;
    p.name = "sprite_batch";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.vertex_buffers = {{0, sizeof(Vertex), false}};
    p.attributes = {{0, 0, rhi::VertexFormat::Float2, 0},
                    {1, 0, rhi::VertexFormat::Float2, 8},
                    {2, 0, rhi::VertexFormat::UByte4Norm, 16}};
    p.cull = rhi::CullMode::None;
    p.color_targets = {{color_format, blend}};
    auto pipe = ctx.device().create_pipeline(p);
    if (!pipe) return std::move(pipe).error();
    b->pipeline_ = pipe.value();
    p.name = "sprite_batch_additive";
    p.color_targets = {{color_format, rhi::BlendMode::Additive}};
    auto add = ctx.device().create_pipeline(p);
    if (!add) return std::move(add).error();
    b->additive_pipeline_ = add.value();
    auto lit_fs = ctx.shader("sprite_lit.frag");
    if (!lit_fs) return std::move(lit_fs).error();
    p.name = "sprite_batch_lit";
    p.fragment = lit_fs.value();
    p.color_targets = {{color_format, blend}};
    auto lit = ctx.device().create_pipeline(p);
    if (!lit) return std::move(lit).error();
    b->lit_pipeline_ = lit.value();
    auto light_vs = ctx.shader("light2d.vert");
    if (!light_vs) return std::move(light_vs).error();
    auto light_fs = ctx.shader("light2d.frag");
    if (!light_fs) return std::move(light_fs).error();
    rhi::PipelineDesc lp;
    lp.name = "light2d";
    lp.vertex = light_vs.value();
    lp.fragment = light_fs.value();
    lp.cull = rhi::CullMode::None;
    lp.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Additive}};
    auto lpipe = ctx.device().create_pipeline(lp);
    if (!lpipe) return std::move(lpipe).error();
    b->light_pipeline_ = lpipe.value();
    VICTUS_TRY_STATUS(b->ensure_capacity(1024));
    return b;
}

SpriteBatch::~SpriteBatch() {
    auto& d = ctx_.device();
    if (pipeline_) d.destroy(pipeline_);
    if (additive_pipeline_) d.destroy(additive_pipeline_);
    if (lit_pipeline_) d.destroy(lit_pipeline_);
    if (light_pipeline_) d.destroy(light_pipeline_);
    if (light_target_) d.destroy(light_target_);
    if (vertex_buffer_) d.destroy(vertex_buffer_);
    if (index_buffer_) d.destroy(index_buffer_);
}

Status SpriteBatch::ensure_capacity(std::uint32_t sprites) {
    if (sprites <= capacity_) return {};
    std::uint32_t cap = std::max(capacity_ * 2, 1024u);
    while (cap < sprites) cap *= 2;
    auto& d = ctx_.device();
    if (vertex_buffer_) d.destroy(vertex_buffer_);
    if (index_buffer_) d.destroy(index_buffer_);
    auto vb = d.create_buffer({"sprite_vertices", cap * 4 * static_cast<std::uint32_t>(sizeof(Vertex)), rhi::BufferUsageVertex});
    if (!vb) return std::move(vb).error();
    vertex_buffer_ = vb.value();
    auto ib = d.create_buffer({"sprite_indices", cap * 6 * 4, rhi::BufferUsageIndex});
    if (!ib) return std::move(ib).error();
    index_buffer_ = ib.value();
    std::vector<std::uint32_t> idx(std::size_t{cap} * 6);
    for (std::uint32_t i = 0; i < cap; ++i) {
        const std::uint32_t v = i * 4;
        const std::uint32_t q[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
        std::copy(q, q + 6, idx.begin() + i * 6);
    }
    auto cmd = d.begin_commands();
    cmd.upload_buffer(index_buffer_, 0, {reinterpret_cast<const std::uint8_t*>(idx.data()), idx.size() * 4});
    cmd.submit();
    capacity_ = cap;
    return {};
}

void SpriteBatch::begin(const Mat4& view_projection, bool pixel_art) {
    view_proj_ = view_projection;
    pixel_art_ = pixel_art;
    sprites_.clear();
    batches_.clear();
    lights_.clear();
    lighting_ = false;
    lights_ready_ = false;
    ambient_ = {1, 1, 1, 1};
    stats_ = {};
}

void SpriteBatch::set_lighting(Color ambient) {
    lighting_ = true;
    const auto clean = [](float v) { return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f; };
    ambient_ = {clean(ambient.r), clean(ambient.g), clean(ambient.b), 1.0f};
}

void SpriteBatch::add_light(const Light2D& light) {
    if (!(light.radius > 0.0f) || !std::isfinite(light.radius) || !(light.intensity > 0.0f) || !std::isfinite(light.intensity) ||
        !std::isfinite(light.position.x) || !std::isfinite(light.position.y))
        return;
    lights_.push_back(light);
}

namespace {
struct Light2DFrame {
    Mat4 view_proj;
    Vec4 center_radius;
};
struct Light2DParams {
    Vec4 color, direction, params;
};
struct SpriteLitParams {
    Vec4 screen;
};
} // namespace

Status SpriteBatch::render_lights(rhi::CommandBuffer& cmd, std::uint32_t width, std::uint32_t height) {
    lights_ready_ = false;
    if (!lighting_ || width == 0 || height == 0) return {};
    VICTUS_PROFILE_SCOPE("SpriteBatch::render_lights");
    auto& d = ctx_.device();
    if (!light_target_ || light_w_ != width || light_h_ != height) {
        if (light_target_) d.destroy(light_target_);
        light_target_ = {};
        rhi::TextureDesc td;
        td.name = "light2d_target";
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width = width;
        td.height = height;
        td.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
        auto t = d.create_texture(td);
        if (!t) return std::move(t).error();
        light_target_ = t.value();
        light_w_ = width;
        light_h_ = height;
    }
    rhi::RenderPassDesc rp;
    rhi::ColorAttachment color;
    color.texture = light_target_;
    color.load = rhi::LoadOp::Clear;
    color.clear = ambient_;
    rp.colors.push_back(color);
    auto pass = cmd.begin_render_pass(rp);
    // Light quads are culled against the view: the view-projection maps the visible area to [-1, 1].
    std::uint32_t drawn = 0;
    for (const Light2D& l : lights_) {
        const Vec4 c = view_proj_ * Vec4(l.position.x, l.position.y, 0, 1);
        const Vec4 rx = view_proj_ * Vec4(l.radius, 0, 0, 0), ry = view_proj_ * Vec4(0, l.radius, 0, 0);
        const float ex = std::abs(rx.x) + std::abs(ry.x), ey = std::abs(rx.y) + std::abs(ry.y);
        if (!(c.x - ex < 1.0f && c.x + ex > -1.0f && c.y - ey < 1.0f && c.y + ey > -1.0f)) continue;
        if (drawn == 0) pass.bind_pipeline(light_pipeline_);
        const Light2DFrame f{view_proj_, {l.position.x, l.position.y, l.radius, 0}};
        cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, f);
        Light2DParams p{};
        const float k = l.intensity;
        p.color = {std::max(l.color.r, 0.0f) * k, std::max(l.color.g, 0.0f) * k, std::max(l.color.b, 0.0f) * k, 1};
        if (l.type == Light2D::Type::Spot) {
            const float len = length(l.direction);
            const Vec2 dir = len > 1e-6f ? l.direction * (1.0f / len) : Vec2{0, -1};
            const float outer = std::clamp(std::isfinite(l.outer_angle) ? l.outer_angle : 0.6f, 0.001f, 3.14159f);
            const float inner = std::clamp(std::isfinite(l.inner_angle) ? l.inner_angle : outer, 0.0f, outer);
            p.direction = {dir.x, dir.y, std::cos(outer), std::cos(inner)};
        } else {
            p.direction = {0, -1, -2, -2};
        }
        const float falloff = std::isfinite(l.falloff) ? std::clamp(l.falloff, 0.05f, 16.0f) : 1.0f;
        p.params = {l.radius, falloff, 0, 0};
        cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, p);
        pass.draw(6);
        ++drawn;
    }
    pass.end();
    stats_.lights = drawn;
    stats_.lighting = true;
    lights_ready_ = true;
    return {};
}

void SpriteBatch::draw(const Sprite& sprite) { sprites_.push_back(sprite); }

void SpriteBatch::draw_rect(Vec2 center, Vec2 size, Color color, std::int32_t layer, float rotation) {
    Sprite s;
    s.position = center;
    s.size = size;
    s.color = color;
    s.layer = layer;
    s.rotation = rotation;
    sprites_.push_back(s);
}

void SpriteBatch::draw_line(Vec2 a, Vec2 b, float thickness, Color color, std::int32_t layer) {
    Vec2 d = b - a;
    Sprite s;
    s.position = (a + b) * 0.5f;
    s.size = {length(d), thickness};
    s.rotation = std::atan2(d.y, d.x);
    s.color = color;
    s.layer = layer;
    sprites_.push_back(s);
}

Status SpriteBatch::prepare(rhi::CommandBuffer& cmd) {
    VICTUS_PROFILE_SCOPE("SpriteBatch::prepare");
    const auto count = static_cast<std::uint32_t>(sprites_.size());
    stats_.sprites = count;
    batches_.clear();
    if (count == 0) return {};
    VICTUS_TRY_STATUS(ensure_capacity(count));
    const rhi::TextureHandle white = ctx_.white();
    order_.resize(count);
    std::iota(order_.begin(), order_.end(), 0u);
    std::stable_sort(order_.begin(), order_.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Sprite& sa = sprites_[a];
        const Sprite& sb = sprites_[b];
        if (sa.layer != sb.layer) return sa.layer < sb.layer;
        if (sa.additive != sb.additive) return !sa.additive;
        const bool la = sa.lit && !sa.additive, lb = sb.lit && !sb.additive;
        if (la != lb) return lb; // unlit runs first within a layer
        const auto ta = sa.texture ? sa.texture.index : white.index;
        const auto tb = sb.texture ? sb.texture.index : white.index;
        if (ta != tb) return ta < tb;
        return !sa.nearest && sb.nearest;
    });
    vertices_.resize(std::size_t{count} * 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Sprite& s = sprites_[order_[i]];
        const rhi::TextureHandle tex = s.texture ? s.texture : white;
        const bool nearest = pixel_art_ || s.nearest;
        const bool lit = s.lit && !s.additive;
        if (lit && lighting_) ++stats_.lit_sprites;
        if (batches_.empty() || !(batches_.back().texture == tex) || batches_.back().additive != s.additive ||
            batches_.back().nearest != nearest || batches_.back().lit != lit)
            batches_.push_back({tex, i * 6, 0, s.additive, nearest, lit});
        batches_.back().index_count += 6;
        const float c = std::cos(s.rotation), sn = std::sin(s.rotation);
        float u0 = s.uv.x, v0 = s.uv.y, u1 = s.uv.z, v1 = s.uv.w;
        if (s.flip_x) std::swap(u0, u1);
        if (s.flip_y) std::swap(v0, v1);
        const Vec2 local[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const Vec2 uvs[4] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
        const std::uint32_t color = s.color.rgba8();
        for (int k = 0; k < 4; ++k) {
            Vec2 p = (local[k] - s.origin) * s.size;
            Vec2 r{p.x * c - p.y * sn, p.x * sn + p.y * c};
            vertices_[std::size_t{i} * 4 + k] = {r + s.position, uvs[k], color};
        }
    }
    stats_.batches = static_cast<std::uint32_t>(batches_.size());
    cmd.upload_buffer(vertex_buffer_, 0,
                      {reinterpret_cast<const std::uint8_t*>(vertices_.data()), vertices_.size() * sizeof(Vertex)});
    return {};
}

void SpriteBatch::render(rhi::RenderPass& pass, rhi::CommandBuffer& cmd) {
    if (batches_.empty()) return;
    VICTUS_PROFILE_SCOPE("SpriteBatch::render");
    const bool use_light = lighting_ && lights_ready_ && light_target_;
    rhi::PipelineHandle bound{};
    for (const Batch& b : batches_) {
        const bool lit = use_light && b.lit;
        const rhi::PipelineHandle want = b.additive ? additive_pipeline_ : lit ? lit_pipeline_ : pipeline_;
        if (!(bound == want)) {
            pass.bind_pipeline(want);
            cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj_);
            if (lit) {
                const SpriteLitParams sp{{1.0f / static_cast<float>(light_w_), 1.0f / static_cast<float>(light_h_), 0, 0}};
                cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, sp);
            }
            pass.bind_vertex_buffer(0, vertex_buffer_);
            pass.bind_index_buffer(index_buffer_, rhi::IndexType::U32);
            bound = want;
        }
        if (lit) pass.bind_texture(rhi::ShaderStage::Fragment, 1, light_target_, ctx_.nearest_clamp());
        pass.bind_texture(rhi::ShaderStage::Fragment, 0, b.texture, b.nearest ? ctx_.nearest_clamp() : ctx_.linear_clamp());
        pass.draw_indexed(b.index_count, 1, b.first_index, 0, 0);
    }
}

} // namespace victus::render
