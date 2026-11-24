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
    b->target_srgb_ = rhi::is_srgb(color_format);
    auto vs = ctx.shader("sprite.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx.shader("sprite.frag");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc p;
    p.name = "sprite_batch";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.vertex_buffers = {{0, sizeof(Vertex), false}};
    static_assert(sizeof(Vertex) == 20, "sprite vertex layout");
    p.attributes = {{0, 0, rhi::VertexFormat::Float2, 0},
                    {1, 0, rhi::VertexFormat::UShort2Norm, 8},
                    {2, 0, rhi::VertexFormat::UByte4Norm, 12},
                    {3, 0, rhi::VertexFormat::Float, 16}};
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
    auto normal_vs = ctx.shader("sprite_normal.vert");
    if (!normal_vs) return std::move(normal_vs).error();
    auto normal_fs = ctx.shader("sprite_normal.frag");
    if (!normal_fs) return std::move(normal_fs).error();
    p.name = "sprite_batch_normals";
    p.vertex = normal_vs.value();
    p.fragment = normal_fs.value();
    p.color_targets = {{rhi::Format::RGBA8_UNORM, rhi::BlendMode::Opaque}};
    auto npipe = ctx.device().create_pipeline(p);
    if (!npipe) return std::move(npipe).error();
    b->normal_pipeline_ = npipe.value();
    auto shadow_vs = ctx.shader("shadow2d.vert");
    if (!shadow_vs) return std::move(shadow_vs).error();
    auto shadow_fs = ctx.shader("shadow2d.frag");
    if (!shadow_fs) return std::move(shadow_fs).error();
    rhi::PipelineDesc sp;
    sp.name = "shadow2d";
    sp.vertex = shadow_vs.value();
    sp.fragment = shadow_fs.value();
    sp.vertex_buffers = {{0, sizeof(Vec2), false}};
    sp.attributes = {{0, 0, rhi::VertexFormat::Float2, 0}};
    sp.cull = rhi::CullMode::None;
    sp.color_targets = {{rhi::Format::R8_UNORM, rhi::BlendMode::Opaque}};
    auto spipe = ctx.device().create_pipeline(sp);
    if (!spipe) return std::move(spipe).error();
    b->shadow_pipeline_ = spipe.value();
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
    if (normal_pipeline_) d.destroy(normal_pipeline_);
    if (shadow_pipeline_) d.destroy(shadow_pipeline_);
    if (normal_target_) d.destroy(normal_target_);
    if (shadow_target_) d.destroy(shadow_target_);
    if (shadow_buffer_) d.destroy(shadow_buffer_);
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
    caster_points_.clear();
    caster_fill_.clear();
    casters_.clear();
    shadow_ranges_.clear();
    has_normal_maps_ = false;
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
// Stable LSD radix sort of (hi, lo) keys whose lo low 24 bits are the ascending submission index:
// those bytes are skipped (the input is already in index order), and so is every byte that is
// the same in all keys. Typical frames vary only in a layer byte and a texture byte.
template <class Key>
void radix_sort(std::vector<Key>& keys, std::vector<Key>& scratch) {
    const std::size_t n = keys.size();
    if (n < 2) return;
    constexpr int kDigits = 13; // lo bytes 3..7, then hi bytes 0..7
    const auto digit = [](const Key& k, int d) -> std::uint32_t {
        return d < 5 ? static_cast<std::uint32_t>(k.lo >> (24 + 8 * d)) & 0xFFu : static_cast<std::uint32_t>(k.hi >> (8 * (d - 5))) & 0xFFu;
    };
    std::uint32_t counts[kDigits][256] = {};
    for (const Key& k : keys)
        for (int d = 0; d < kDigits; ++d) ++counts[d][digit(k, d)];
    scratch.resize(n);
    for (int d = 0; d < kDigits; ++d) {
        std::uint32_t* c = counts[d];
        if (c[digit(keys[0], d)] == n) continue; // uniform byte
        std::uint32_t sum = 0;
        for (int b = 0; b < 256; ++b) {
            const std::uint32_t v = c[b];
            c[b] = sum;
            sum += v;
        }
        for (const Key& k : keys) scratch[c[digit(k, d)]++] = k;
        keys.swap(scratch);
    }
}

std::uint16_t unorm16(float v) {
    // NaN fails both comparisons and maps to 0.
    const float c = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    return static_cast<std::uint16_t>(c * 65535.0f + 0.5f);
}
// Twice the signed area (positive = counter-clockwise).
float signed_area2(std::span<const Vec2> pts) {
    float a = 0;
    for (std::size_t i = 0, n = pts.size(); i < n; ++i) {
        const Vec2 p = pts[i], q = pts[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a;
}
float cross2(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Ear clipping of a simple counter-clockwise polygon into triangle vertices. O(n^2) per caster;
// casters are small outlines. Degenerate leftovers (collinear runs) are dropped.
void ear_clip(std::span<const Vec2> pts, std::vector<Vec2>& out) {
    std::vector<std::uint32_t> idx(pts.size());
    std::iota(idx.begin(), idx.end(), 0u);
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < pts.size() * pts.size()) {
        bool clipped = false;
        for (std::size_t i = 0, n = idx.size(); i < n; ++i) {
            const Vec2 a = pts[idx[(i + n - 1) % n]], b = pts[idx[i]], c = pts[idx[(i + 1) % n]];
            if (cross2(a, b, c) <= 1e-9f) continue; // reflex or collinear
            bool inside = false;
            for (std::size_t j = 0; j < n && !inside; ++j) {
                const std::uint32_t k = idx[j];
                if (k == idx[(i + n - 1) % n] || k == idx[i] || k == idx[(i + 1) % n]) continue;
                const Vec2 p = pts[k];
                inside = cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0;
            }
            if (inside) continue;
            out.insert(out.end(), {a, b, c});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break; // not simple: keep what was clipped
    }
    if (idx.size() == 3 && cross2(pts[idx[0]], pts[idx[1]], pts[idx[2]]) > 1e-9f)
        out.insert(out.end(), {pts[idx[0]], pts[idx[1]], pts[idx[2]]});
}
} // namespace

void SpriteBatch::add_shadow_caster(std::span<const Vec2> outline, bool self_shadow) {
    if (outline.size() < 3) return;
    for (const Vec2 p : outline)
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
    const float area = signed_area2(outline);
    if (!(std::abs(area) > 1e-12f)) return;
    Caster c{};
    c.first = static_cast<std::uint32_t>(caster_points_.size());
    c.count = static_cast<std::uint32_t>(outline.size());
    if (area > 0) {
        caster_points_.insert(caster_points_.end(), outline.begin(), outline.end());
    } else {
        caster_points_.insert(caster_points_.end(), outline.rbegin(), outline.rend());
    }
    const std::span<const Vec2> pts(caster_points_.data() + c.first, c.count);
    c.lo = c.hi = pts[0];
    for (const Vec2 p : pts) {
        c.lo = {std::min(c.lo.x, p.x), std::min(c.lo.y, p.y)};
        c.hi = {std::max(c.hi.x, p.x), std::max(c.hi.y, p.y)};
    }
    c.self_shadow = self_shadow;
    c.fill_first = static_cast<std::uint32_t>(caster_fill_.size());
    if (!self_shadow) ear_clip(pts, caster_fill_);
    c.fill_count = static_cast<std::uint32_t>(caster_fill_.size()) - c.fill_first;
    casters_.push_back(c);
    ++stats_.shadow_casters;
}

namespace {
struct Light2DFrame {
    Mat4 view_proj;
    Vec4 center_radius;
};
struct Light2DParams {
    Vec4 color, direction, params, screen;
};
struct ShadowParams {
    Vec4 value;
};
bool light_visible(const Mat4& vp, const Light2D& l) {
    const Vec4 c = vp * Vec4(l.position.x, l.position.y, 0, 1);
    const Vec4 rx = vp * Vec4(l.radius, 0, 0, 0), ry = vp * Vec4(0, l.radius, 0, 0);
    const float ex = std::abs(rx.x) + std::abs(ry.x), ey = std::abs(rx.y) + std::abs(ry.y);
    return c.x - ex < 1.0f && c.x + ex > -1.0f && c.y - ey < 1.0f && c.y + ey > -1.0f;
}
struct SpriteLitParams {
    Vec4 screen; // xy: 1 / light target size, z: re-encode sRGB texels, w: sRGB target
};
struct SpriteParams {
    Vec4 flags; // x: re-encode sRGB texels, y: sRGB target
};
} // namespace

bool SpriteBatch::encodes(rhi::TextureHandle texture) const {
    if (!texture) return false;
    const rhi::TextureDesc* d = ctx_.device().describe(texture);
    return d && rhi::is_srgb(d->format);
}

Status SpriteBatch::ensure_target(rhi::TextureHandle& tex, rhi::Format format, const char* name, std::uint32_t w,
                                  std::uint32_t h) {
    auto& d = ctx_.device();
    if (tex) {
        const rhi::TextureDesc* desc = d.describe(tex);
        if (desc && desc->width == w && desc->height == h) return {};
        d.destroy(tex);
        tex = {};
    }
    rhi::TextureDesc td;
    td.name = name;
    td.format = format;
    td.width = w;
    td.height = h;
    td.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    auto t = d.create_texture(td);
    if (!t) return std::move(t).error();
    tex = t.value();
    return {};
}

// Builds, per visible shadowed light, its shadow volumes (every caster edge extruded away from the
// light past its radius; their union covers the caster and everything behind it) followed by the
// interiors of casters that are not self-shadowing (drawn as 0 so their tops stay lit).
void SpriteBatch::build_shadows() {
    shadow_vertices_.clear();
    shadow_ranges_.clear();
    if (casters_.empty()) return;
    for (std::uint32_t li = 0; li < lights_.size(); ++li) {
        const Light2D& l = lights_[li];
        if (!l.shadows || !(l.shadow_strength > 0.0f) || !light_visible(view_proj_, l)) continue;
        const Vec2 lp = l.position;
        const float reach = l.radius * 4.0f;
        ShadowRange r{li, static_cast<std::uint32_t>(shadow_vertices_.size()), 0, 0, 0};
        const auto touches = [&](const Caster& c) {
            return c.lo.x < lp.x + l.radius && c.hi.x > lp.x - l.radius && c.lo.y < lp.y + l.radius && c.hi.y > lp.y - l.radius;
        };
        const auto extrude = [&](Vec2 p) {
            const Vec2 dv = p - lp;
            const float len = length(dv);
            return len > 1e-6f ? p + dv * (reach / len) : p;
        };
        for (const Caster& c : casters_) {
            if (!touches(c)) continue;
            for (std::uint32_t i = 0; i < c.count; ++i) {
                const Vec2 a = caster_points_[c.first + i], b = caster_points_[c.first + (i + 1) % c.count];
                const Vec2 ea = extrude(a), eb = extrude(b);
                shadow_vertices_.insert(shadow_vertices_.end(), {a, b, eb, a, eb, ea});
            }
        }
        r.volume_count = static_cast<std::uint32_t>(shadow_vertices_.size()) - r.volume_first;
        if (r.volume_count == 0) continue;
        r.fill_first = static_cast<std::uint32_t>(shadow_vertices_.size());
        for (const Caster& c : casters_)
            if (!c.self_shadow && touches(c))
                shadow_vertices_.insert(shadow_vertices_.end(), caster_fill_.begin() + c.fill_first,
                                        caster_fill_.begin() + c.fill_first + c.fill_count);
        r.fill_count = static_cast<std::uint32_t>(shadow_vertices_.size()) - r.fill_first;
        shadow_ranges_.push_back(r);
    }
}

Status SpriteBatch::render_lights(rhi::CommandBuffer& cmd, std::uint32_t width, std::uint32_t height) {
    lights_ready_ = false;
    if (!lighting_ || width == 0 || height == 0) return {};
    VICTUS_PROFILE_SCOPE("SpriteBatch::render_lights");
    auto& d = ctx_.device();
    VICTUS_TRY_STATUS(ensure_target(light_target_, rhi::Format::RGBA16_FLOAT, "light2d_target", width, height));
    light_w_ = width;
    light_h_ = height;
    const float inv_w = 1.0f / static_cast<float>(width), inv_h = 1.0f / static_cast<float>(height);

    // Normal buffer (ADR-0255): only when a lit batch carries a normal map. Every lit sprite writes
    // it, so a flat sprite on top of a normal-mapped one hides the normals beneath.
    const bool normals = has_normal_maps_;
    if (normals) {
        VICTUS_TRY_STATUS(ensure_target(normal_target_, rhi::Format::RGBA8_UNORM, "light2d_normals", width, height));
        rhi::RenderPassDesc np;
        rhi::ColorAttachment nc;
        nc.texture = normal_target_;
        nc.load = rhi::LoadOp::Clear;
        nc.clear = {0.5f, 0.5f, 1.0f, 0.0f};
        np.colors.push_back(nc);
        auto pass = cmd.begin_render_pass(np);
        pass.bind_pipeline(normal_pipeline_);
        cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj_);
        pass.bind_vertex_buffer(0, vertex_buffer_);
        pass.bind_index_buffer(index_buffer_, rhi::IndexType::U32);
        for (const Batch& b : batches_) {
            if (!b.lit) continue;
            const bool has = static_cast<bool>(b.normal_map);
            cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, SpriteParams{{has ? 1.0f : 0.0f, 0, 0, 0}});
            pass.bind_texture(rhi::ShaderStage::Fragment, 0, b.texture, b.nearest ? ctx_.nearest_clamp() : ctx_.linear_clamp());
            pass.bind_texture(rhi::ShaderStage::Fragment, 1, has ? b.normal_map : ctx_.white(),
                              b.nearest ? ctx_.nearest_clamp() : ctx_.linear_clamp());
            pass.draw_indexed(b.index_count, 1, b.first_index, 0, 0);
        }
        pass.end();
    }

    build_shadows();
    if (!shadow_ranges_.empty()) {
        const std::size_t bytes = shadow_vertices_.size() * sizeof(Vec2);
        if (bytes > shadow_capacity_) {
            if (shadow_buffer_) d.destroy(shadow_buffer_);
            shadow_buffer_ = {};
            std::size_t cap = std::max<std::size_t>(shadow_capacity_ * 2, 64 * 1024);
            while (cap < bytes) cap *= 2;
            auto buf = d.create_buffer({"shadow2d_vertices", static_cast<std::uint32_t>(cap), rhi::BufferUsageVertex});
            if (!buf) return std::move(buf).error();
            shadow_buffer_ = buf.value();
            shadow_capacity_ = cap;
        }
        cmd.upload_buffer(shadow_buffer_, 0, {reinterpret_cast<const std::uint8_t*>(shadow_vertices_.data()), bytes});
        VICTUS_TRY_STATUS(ensure_target(shadow_target_, rhi::Format::R8_UNORM, "light2d_shadow", width, height));
    }

    const rhi::TextureHandle normal_tex = normals ? normal_target_ : ctx_.white();
    const auto draw_light = [&](rhi::RenderPass& pass, const Light2D& l, bool shadowed) {
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
        const float h = std::isfinite(l.height) ? std::max(l.height, 1e-3f) : 1.0f;
        const float strength = shadowed && std::isfinite(l.shadow_strength) ? std::clamp(l.shadow_strength, 0.0f, 1.0f) : 0.0f;
        p.params = {l.radius, falloff, h, strength};
        p.screen = {inv_w, inv_h, normals ? 1.0f : 0.0f, 0};
        cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, p);
        pass.bind_texture(rhi::ShaderStage::Fragment, 0, normal_tex, ctx_.nearest_clamp());
        pass.bind_texture(rhi::ShaderStage::Fragment, 1, shadowed ? shadow_target_ : ctx_.black(), ctx_.nearest_clamp());
        pass.draw(6);
    };

    // Unshadowed lights first, in one pass that clears to ambient. Light quads are culled against
    // the view: the view-projection maps the visible area to [-1, 1].
    std::uint32_t drawn = 0;
    {
        rhi::RenderPassDesc rp;
        rhi::ColorAttachment color;
        color.texture = light_target_;
        color.load = rhi::LoadOp::Clear;
        color.clear = ambient_;
        rp.colors.push_back(color);
        auto pass = cmd.begin_render_pass(rp);
        std::size_t next_shadowed = 0;
        for (std::uint32_t li = 0; li < lights_.size(); ++li) {
            if (next_shadowed < shadow_ranges_.size() && shadow_ranges_[next_shadowed].light == li) {
                ++next_shadowed;
                continue;
            }
            const Light2D& l = lights_[li];
            if (!light_visible(view_proj_, l)) continue;
            if (drawn == 0) pass.bind_pipeline(light_pipeline_);
            draw_light(pass, l, false);
            ++drawn;
        }
        pass.end();
    }
    // Each shadowed light: its mask, then the light on top of the accumulated target.
    for (const ShadowRange& r : shadow_ranges_) {
        {
            rhi::RenderPassDesc mp;
            rhi::ColorAttachment mc;
            mc.texture = shadow_target_;
            mc.load = rhi::LoadOp::Clear;
            mc.clear = {0, 0, 0, 0};
            mp.colors.push_back(mc);
            auto pass = cmd.begin_render_pass(mp);
            pass.bind_pipeline(shadow_pipeline_);
            cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj_);
            pass.bind_vertex_buffer(0, shadow_buffer_);
            cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, ShadowParams{{1, 0, 0, 0}});
            pass.draw(r.volume_count, 1, r.volume_first);
            if (r.fill_count > 0) {
                cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, ShadowParams{{0, 0, 0, 0}});
                pass.draw(r.fill_count, 1, r.fill_first);
            }
            pass.end();
        }
        rhi::RenderPassDesc rp;
        rhi::ColorAttachment color;
        color.texture = light_target_;
        color.load = rhi::LoadOp::Load;
        rp.colors.push_back(color);
        auto pass = cmd.begin_render_pass(rp);
        pass.bind_pipeline(light_pipeline_);
        draw_light(pass, lights_[r.light], true);
        pass.end();
        ++drawn;
    }
    stats_.lights = drawn;
    stats_.shadowed_lights = static_cast<std::uint32_t>(shadow_ranges_.size());
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
    has_normal_maps_ = false;
    if (count == 0) return {};
    VICTUS_TRY_STATUS(ensure_capacity(count));
    const rhi::TextureHandle white = ctx_.white();
    // Sort by packed keys (layer, additive, lit, texture | normal map, nearest, submission index):
    // the same order as a stable sort on those fields, without indirect compares. Texture slot
    // indices above 2^30 would only split batches, never merge different textures.
    sort_keys_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Sprite& sp = sprites_[i];
        const bool lit = sp.lit && !sp.additive;
        const std::uint32_t tex = sp.texture ? sp.texture.index : white.index;
        const std::uint32_t layer = static_cast<std::uint32_t>(sp.layer) ^ 0x80000000u;
        sort_keys_[i] = {(std::uint64_t{layer} << 32) | (std::uint64_t{sp.additive} << 31) | (std::uint64_t{lit} << 30) |
                             (tex & 0x3FFFFFFFu),
                         (std::uint64_t{lit ? sp.normal_map.index : 0u} << 32) | (std::uint64_t{sp.nearest} << 31) | i};
    }
    radix_sort(sort_keys_, sort_scratch_);
    order_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) order_[i] = static_cast<std::uint32_t>(sort_keys_[i].lo & 0x7FFFFFFFu);
    vertices_.resize(std::size_t{count} * 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Sprite& s = sprites_[order_[i]];
        const rhi::TextureHandle tex = s.texture ? s.texture : white;
        const bool nearest = pixel_art_ || s.nearest;
        const bool lit = s.lit && !s.additive;
        const rhi::TextureHandle nmap = lit ? s.normal_map : rhi::TextureHandle{};
        if (lit && lighting_) {
            ++stats_.lit_sprites;
            if (nmap) {
                ++stats_.normal_mapped_sprites;
                has_normal_maps_ = true;
            }
        }
        if (batches_.empty() || !(batches_.back().texture == tex) || batches_.back().additive != s.additive ||
            batches_.back().nearest != nearest || batches_.back().lit != lit || !(batches_.back().normal_map == nmap))
            batches_.push_back({tex, i * 6, 0, s.additive, nearest, lit, encodes(tex), nmap});
        batches_.back().index_count += 6;
        const float c = std::cos(s.rotation), sn = std::sin(s.rotation);
        float u0 = s.uv.x, v0 = s.uv.y, u1 = s.uv.z, v1 = s.uv.w;
        if (s.flip_x) std::swap(u0, u1);
        if (s.flip_y) std::swap(v0, v1);
        const Vec2 local[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const std::uint16_t qu0 = unorm16(u0), qu1 = unorm16(u1), qv0 = unorm16(v0), qv1 = unorm16(v1);
        const std::uint16_t uvs[4][2] = {{qu0, qv1}, {qu1, qv1}, {qu1, qv0}, {qu0, qv0}};
        const std::uint32_t color = s.color.rgba8();
        // Tangent = flip_x ? -(c, sn) : (c, sn); bitangent = (flip_x != flip_y ? -1 : 1) * perp(tangent).
        float angle = s.rotation + (s.flip_x ? 3.14159265f : 0.0f);
        if (!(angle >= -3.14159265f && angle <= 3.14159265f)) { // wrap into [-pi, pi]; NaN and infinity -> 0
            angle -= 6.28318531f * std::floor((angle + 3.14159265f) * 0.159154943f);
            angle = std::isfinite(angle) ? std::clamp(angle, -3.14159265f, 3.14159265f) : 0.0f;
        }
        const float frame = angle + (s.flip_x != s.flip_y ? 8.0f : 0.0f);
        for (int k = 0; k < 4; ++k) {
            Vec2 p = (local[k] - s.origin) * s.size;
            Vec2 r{p.x * c - p.y * sn, p.x * sn + p.y * c};
            vertices_[std::size_t{i} * 4 + k] = {r + s.position, {uvs[k][0], uvs[k][1]}, color, frame};
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
    int pushed_encode = -1;
    for (const Batch& b : batches_) {
        const bool lit = use_light && b.lit;
        const rhi::PipelineHandle want = b.additive ? additive_pipeline_ : lit ? lit_pipeline_ : pipeline_;
        if (!(bound == want)) {
            pass.bind_pipeline(want);
            cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj_);
            pass.bind_vertex_buffer(0, vertex_buffer_);
            pass.bind_index_buffer(index_buffer_, rhi::IndexType::U32);
            bound = want;
            pushed_encode = -1;
        }
        // 2D works in display space (sprite tints and untextured sprites are display colours, as
        // in an image editor): sRGB textures, decoded to linear when sampled, are encoded back, and
        // an sRGB target gets linear output so the hardware encode restores the display colour.
        if (pushed_encode != static_cast<int>(b.encode)) {
            const float e = b.encode ? 1.0f : 0.0f, t = target_srgb_ ? 1.0f : 0.0f;
            if (lit) {
                const SpriteLitParams sp{{1.0f / static_cast<float>(light_w_), 1.0f / static_cast<float>(light_h_), e, t}};
                cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, sp);
            } else {
                cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, SpriteParams{{e, t, 0, 0}});
            }
            pushed_encode = static_cast<int>(b.encode);
        }
        if (lit) pass.bind_texture(rhi::ShaderStage::Fragment, 1, light_target_, ctx_.nearest_clamp());
        pass.bind_texture(rhi::ShaderStage::Fragment, 0, b.texture, b.nearest ? ctx_.nearest_clamp() : ctx_.linear_clamp());
        pass.draw_indexed(b.index_count, 1, b.first_index, 0, 0);
    }
}

} // namespace victus::render
