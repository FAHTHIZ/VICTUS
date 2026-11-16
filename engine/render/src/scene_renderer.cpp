// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::render {
using namespace math;

namespace {
// std140-compatible uniform blocks; must match engine/render/shaders.
struct FrameUniforms {
    Mat4 view_proj;
    Mat4 light_view_proj;
};
struct ObjectUniforms {
    Mat4 model;
    Mat4 normal_matrix;
};
struct GpuPointLight {
    Vec4 position_range;
    Vec4 color_intensity;
};
struct LightingUniforms {
    Vec4 camera_pos;
    Vec4 sun_dir;
    Vec4 sun_color;
    Vec4 sky_color;
    Vec4 horizon_color;
    Vec4 ground_color;
    Vec4 shadow_params;
    GpuPointLight points[SceneRenderer::kMaxPointLights];
};
struct MaterialUniforms {
    Vec4 base_color;
    Vec4 emissive;
    Vec4 params;
    Vec4 flags;
};
struct ShadowUniforms {
    Mat4 light_view_proj;
    Mat4 model;
};
struct SkyUniforms {
    Mat4 inv_view_proj;
    Vec4 zenith, horizon, ground, sun_dir, sun_color;
};
struct PostUniforms {
    Vec4 params;
};
static_assert(sizeof(LightingUniforms) == 16 * 7 + 32 * SceneRenderer::kMaxPointLights);

bool is_srgb(rhi::Format f) { return f == rhi::Format::RGBA8_SRGB || f == rhi::Format::BGRA8_SRGB; }
Vec4 rgb(Color c, float scale = 1.0f, float w = 0.0f) { return {c.r * scale, c.g * scale, c.b * scale, w}; }
} // namespace

Camera3D Camera3D::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Camera3D c;
    c.position = eye;
    c.view = Mat4::look_at(eye, target, up);
    return c;
}

Camera3D Camera3D::from_transform(Vec3 position, Quat rotation) {
    Camera3D c;
    c.position = position;
    c.view = inverse(Mat4::trs(position, rotation, {1, 1, 1}));
    return c;
}

Mat4 Camera3D::projection(float aspect) const noexcept { return Mat4::perspective(fov_y, aspect, z_near, z_far); }

Vec3 Camera3D::forward() const noexcept { return normalize(inverse(view).transform_vector({0, 0, -1})); }

Result<std::unique_ptr<SceneRenderer>> SceneRenderer::create(GpuContext& ctx) {
    std::unique_ptr<SceneRenderer> r(new SceneRenderer(ctx));
    auto& dev = ctx.device();
    r->depth_format_ = ctx.sampled_depth_format();
    auto get = [&](const char* n) { return ctx.shader(n); };
    auto mesh_vs = get("mesh.vert");
    auto pbr_fs = get("pbr.frag");
    auto shadow_vs = get("shadow.vert");
    auto shadow_fs = get("shadow.frag");
    auto full_vs = get("fullscreen.vert");
    auto sky_fs = get("sky.frag");
    auto line_vs = get("line.vert");
    auto line_fs = get("line.frag");
    for (auto* s : {&mesh_vs, &pbr_fs, &shadow_vs, &shadow_fs, &full_vs, &sky_fs, &line_vs, &line_fs})
        if (!*s) return std::move(*s).error();

    const rhi::VertexBufferLayout mesh_layout{0, sizeof(Vertex3D), false};
    rhi::PipelineDesc opaque;
    opaque.name = "pbr_opaque";
    opaque.vertex = mesh_vs.value();
    opaque.fragment = pbr_fs.value();
    opaque.vertex_buffers = {mesh_layout};
    opaque.attributes = {{0, 0, rhi::VertexFormat::Float3, 0},
                         {1, 0, rhi::VertexFormat::Float3, 12},
                         {2, 0, rhi::VertexFormat::Float2, 24},
                         {3, 0, rhi::VertexFormat::Float4, 32}};
    opaque.depth_test = opaque.depth_write = true;
    opaque.depth_compare = rhi::CompareOp::Less;
    opaque.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Opaque}};
    opaque.depth_format = r->depth_format_;
    auto p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_cull_ = p.value();
    opaque.name = "pbr_double_sided";
    opaque.cull = rhi::CullMode::None;
    p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_double_ = p.value();

    rhi::PipelineDesc shadow;
    shadow.name = "shadow_depth";
    shadow.vertex = shadow_vs.value();
    shadow.fragment = shadow_fs.value();
    shadow.vertex_buffers = {mesh_layout};
    shadow.attributes = {{0, 0, rhi::VertexFormat::Float3, 0}};
    shadow.cull = rhi::CullMode::None;
    shadow.depth_test = shadow.depth_write = true;
    shadow.depth_compare = rhi::CompareOp::Less;
    shadow.depth_bias_constant = 1.25f;
    shadow.depth_bias_slope = 1.75f;
    shadow.depth_format = r->depth_format_;
    p = dev.create_pipeline(shadow);
    if (!p) return std::move(p).error();
    r->shadow_pipeline_ = p.value();

    rhi::PipelineDesc sky;
    sky.name = "sky";
    sky.vertex = full_vs.value();
    sky.fragment = sky_fs.value();
    sky.cull = rhi::CullMode::None;
    sky.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Opaque}};
    sky.depth_format = r->depth_format_;
    p = dev.create_pipeline(sky);
    if (!p) return std::move(p).error();
    r->sky_pipeline_ = p.value();

    rhi::PipelineDesc lines;
    lines.name = "debug_lines";
    lines.vertex = line_vs.value();
    lines.fragment = line_fs.value();
    lines.vertex_buffers = {{0, sizeof(LineVertex), false}};
    lines.attributes = {{0, 0, rhi::VertexFormat::Float3, 0}, {1, 0, rhi::VertexFormat::UByte4Norm, 12}};
    lines.primitive = rhi::Primitive::LineList;
    lines.cull = rhi::CullMode::None;
    lines.depth_test = true;
    lines.depth_compare = rhi::CompareOp::LessEqual;
    lines.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Alpha}};
    lines.depth_format = r->depth_format_;
    p = dev.create_pipeline(lines);
    if (!p) return std::move(p).error();
    r->line_pipeline_ = p.value();

    // 1x1 cleared depth texture bound when shadows are disabled (compare always passes).
    rhi::TextureDesc ns;
    ns.name = "no_shadow";
    ns.format = r->depth_format_;
    ns.usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
    auto t = dev.create_texture(ns);
    if (!t) return std::move(t).error();
    r->no_shadow_ = t.value();
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.depth.texture = r->no_shadow_;
    rp.depth.clear_depth = 1.0f;
    auto pass = cmd.begin_render_pass(rp);
    pass.end();
    cmd.submit();
    return r;
}

SceneRenderer::~SceneRenderer() {
    auto& d = ctx_.device();
    for (auto pl : {opaque_cull_, opaque_double_, shadow_pipeline_, sky_pipeline_, line_pipeline_})
        if (pl) d.destroy(pl);
    for (auto& [f, pl] : tonemap_) d.destroy(pl);
    if (no_shadow_) d.destroy(no_shadow_);
    if (line_buffer_) d.destroy(line_buffer_);
    pool_.clear();
}

Result<rhi::PipelineHandle> SceneRenderer::tonemap_pipeline(rhi::Format format) {
    auto key = static_cast<std::uint8_t>(format);
    if (auto it = tonemap_.find(key); it != tonemap_.end()) return it->second;
    auto vs = ctx_.shader("fullscreen.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx_.shader("tonemap.frag");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc d;
    d.name = "tonemap";
    d.vertex = vs.value();
    d.fragment = fs.value();
    d.cull = rhi::CullMode::None;
    d.color_targets = {{format, rhi::BlendMode::Opaque}};
    auto p = ctx_.device().create_pipeline(d);
    if (!p) return p;
    tonemap_.emplace(key, p.value());
    return p;
}

void SceneRenderer::begin(const Camera3D& camera, const DirectionalLight& sun, const Environment& env) {
    camera_ = camera;
    sun_ = sun;
    env_ = env;
    points_.clear();
    draws_.clear();
    lines_.clear();
    stats_ = {};
}

void SceneRenderer::add_point_light(const PointLight& light) { points_.push_back(light); }

void SceneRenderer::submit(const GpuMesh& mesh, const Material& material, const Mat4& model) {
    if (!mesh.valid()) return;
    ++stats_.submitted;
    draws_.push_back({&mesh, material, model, 0.0f});
}

void SceneRenderer::line(Vec3 a, Vec3 b, Color color) {
    const std::uint32_t c = color.rgba8();
    lines_.push_back({a, c});
    lines_.push_back({b, c});
}

void SceneRenderer::box(const Aabb& bx, Color color) {
    const Vec3 mn = bx.min, mx = bx.max;
    const Vec3 v[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                       {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    const int e[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
    for (int i = 0; i < 24; i += 2) line(v[e[i]], v[e[i + 1]], color);
}

void SceneRenderer::grid(float half, float spacing, Color color) {
    if (spacing <= 0) return;
    for (float x = -half; x <= half + 1e-4f; x += spacing) {
        line({x, 0, -half}, {x, 0, half}, color);
        line({-half, 0, x}, {half, 0, x}, color);
    }
}

void SceneRenderer::compute_shadow_matrix() {
    const Vec3 d = normalize(sun_.direction);
    const float r = std::max(1.0f, sun_.shadow_distance * 0.5f);
    const Vec3 center = camera_.position + camera_.forward() * r;
    const Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Mat4 light_view = Mat4::look_at({0, 0, 0}, d, up);
    Vec3 c = light_view.transform_point(center);
    // Snap to shadow texels so the shadow does not shimmer as the camera moves.
    const float texel = (2.0f * r) / static_cast<float>(std::max(1u, sun_.shadow_resolution));
    c.x = std::floor(c.x / texel) * texel;
    c.y = std::floor(c.y / texel) * texel;
    const Mat4 proj = Mat4::orthographic(c.x - r, c.x + r, c.y - r, c.y + r, -c.z - r * 4.0f, -c.z + r * 4.0f);
    light_vp_ = proj * light_view;
}

Status SceneRenderer::render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width, std::uint32_t height,
                             rhi::Format target_format) {
    VICTUS_PROFILE_SCOPE("SceneRenderer::render");
    if (width == 0 || height == 0) return {};
    auto& dev = ctx_.device();
    auto tonemap = tonemap_pipeline(target_format);
    if (!tonemap) return std::move(tonemap).error();

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 view_proj = camera_.projection(aspect) * camera_.view;
    const Frustum frustum = Frustum::from_matrix(view_proj);
    const bool shadows = sun_.shadows && sun_.intensity > 0.0f;
    if (shadows) compute_shadow_matrix();
    else light_vp_ = Mat4::identity();
    const Frustum light_frustum = Frustum::from_matrix(light_vp_);

    // Visibility + sorting (front-to-back within pipeline and base texture runs).
    std::vector<const Draw*> visible, casters;
    visible.reserve(draws_.size());
    for (Draw& d : draws_) {
        const Aabb world = d.mesh->bounds.transformed(d.model);
        if (shadows && d.material.cast_shadows && light_frustum.intersects(world)) casters.push_back(&d);
        if (!frustum.intersects(world)) {
            ++stats_.culled;
            continue;
        }
        d.distance = length(world.center() - camera_.position);
        visible.push_back(&d);
    }
    std::sort(visible.begin(), visible.end(), [](const Draw* a, const Draw* b) {
        if (a->material.double_sided != b->material.double_sided) return !a->material.double_sided;
        if (a->material.base_color_texture.index != b->material.base_color_texture.index)
            return a->material.base_color_texture.index < b->material.base_color_texture.index;
        return a->distance < b->distance;
    });
    stats_.visible = static_cast<std::uint32_t>(visible.size());
    stats_.shadow_casters = static_cast<std::uint32_t>(casters.size());

    // Nearest point lights when over budget.
    if (points_.size() > kMaxPointLights) {
        std::partial_sort(points_.begin(), points_.begin() + kMaxPointLights, points_.end(),
                          [&](const PointLight& a, const PointLight& b) {
                              return length(a.position - camera_.position) < length(b.position - camera_.position);
                          });
        points_.resize(kMaxPointLights);
    }
    stats_.point_lights = static_cast<std::uint32_t>(points_.size());

    // Debug line upload happens before any render pass.
    stats_.lines = static_cast<std::uint32_t>(lines_.size() / 2);
    if (!lines_.empty()) {
        const auto needed = static_cast<std::uint32_t>(lines_.size());
        if (needed > line_capacity_) {
            if (line_buffer_) dev.destroy(line_buffer_);
            line_capacity_ = std::max(needed, line_capacity_ * 2);
            auto b = dev.create_buffer({"debug_lines", line_capacity_ * static_cast<std::uint32_t>(sizeof(LineVertex)),
                                        rhi::BufferUsageVertex});
            if (!b) return std::move(b).error();
            line_buffer_ = b.value();
        }
        cmd.upload_buffer(line_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(lines_.data()), lines_.size() * sizeof(LineVertex)});
    }

    LightingUniforms lighting{};
    lighting.camera_pos = Vec4(camera_.position, 1.0f);
    lighting.sun_dir = Vec4(normalize(sun_.direction), shadows ? 1.0f : 0.0f);
    lighting.sun_color = rgb(sun_.color, sun_.intensity);
    lighting.sky_color = rgb(env_.sky_zenith, env_.ambient_intensity, env_.exposure);
    lighting.horizon_color = rgb(env_.sky_horizon, env_.ambient_intensity);
    lighting.ground_color = rgb(env_.ground, env_.ambient_intensity, static_cast<float>(points_.size()));
    lighting.shadow_params = {1.0f / static_cast<float>(std::max(1u, sun_.shadow_resolution)), 0.0008f, 0.002f, 0.0f};
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const auto& pl = points_[i];
        lighting.points[i] = {Vec4(pl.position, std::max(pl.range, 0.01f)), rgb(pl.color, 1.0f, pl.intensity)};
    }

    RenderGraph graph;
    rhi::TextureDesc target_desc;
    target_desc.name = "scene_target";
    target_desc.format = target_format;
    target_desc.width = width;
    target_desc.height = height;
    target_desc.usage = rhi::TextureUsageColorTarget;
    const RGTexture out = graph.import_texture("target", target, target_desc);
    graph.mark_output(out);

    RGTexture shadow_map;
    if (shadows) {
        rhi::TextureDesc sd;
        sd.format = depth_format_;
        sd.width = sd.height = sun_.shadow_resolution;
        sd.usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
        shadow_map = graph.create_texture("shadow_map", sd);
        graph.add_pass(
            "shadow", [&](PassBuilder& b) { b.write_depth(shadow_map, rhi::LoadOp::Clear, 1.0f); },
            [&](PassContext& ctx) {
                ctx.pass->bind_pipeline(shadow_pipeline_);
                for (const Draw* d : casters) {
                    ShadowUniforms su{light_vp_, d->model};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, su);
                    ctx.pass->bind_vertex_buffer(0, d->mesh->vertices);
                    ctx.pass->bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                    ctx.pass->draw_indexed(d->mesh->index_count);
                }
            });
    }

    rhi::TextureDesc hdr_desc;
    hdr_desc.format = rhi::Format::RGBA16_FLOAT;
    hdr_desc.width = width;
    hdr_desc.height = height;
    hdr_desc.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    const RGTexture hdr = graph.create_texture("hdr_color", hdr_desc);
    rhi::TextureDesc depth_desc = hdr_desc;
    depth_desc.format = depth_format_;
    depth_desc.usage = rhi::TextureUsageDepthTarget;
    const RGTexture depth = graph.create_texture("scene_depth", depth_desc);

    graph.add_pass(
        "forward",
        [&](PassBuilder& b) {
            if (shadow_map.valid()) b.read(shadow_map);
            b.write_color(hdr, rhi::LoadOp::Clear, env_.clear_color);
            b.write_depth(depth, rhi::LoadOp::Clear, 1.0f);
        },
        [&](PassContext& ctx) {
            rhi::RenderPass& pass = *ctx.pass;
            if (env_.draw_sky) {
                SkyUniforms sky{};
                sky.inv_view_proj = inverse(view_proj);
                sky.zenith = rgb(env_.sky_zenith);
                sky.horizon = rgb(env_.sky_horizon);
                sky.ground = rgb(env_.ground);
                sky.sun_dir = Vec4(-normalize(sun_.direction), 0);
                sky.sun_color = rgb(sun_.color, sun_.intensity * 4.0f, 0.9995f);
                pass.bind_pipeline(sky_pipeline_);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, sky);
                pass.draw(3);
            }
            const rhi::TextureHandle shadow_tex = shadow_map.valid() ? ctx.texture(shadow_map) : no_shadow_;
            FrameUniforms frame{view_proj, light_vp_};
            ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
            ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, lighting);
            rhi::PipelineHandle bound{};
            for (const Draw* d : visible) {
                const rhi::PipelineHandle want = d->material.double_sided ? opaque_double_ : opaque_cull_;
                if (!(bound == want)) {
                    pass.bind_pipeline(want);
                    bound = want;
                }
                const Material& m = d->material;
                ObjectUniforms obj{d->model, transpose(inverse(d->model))};
                MaterialUniforms mat{};
                mat.base_color = m.base_color.vec();
                mat.emissive = rgb(m.emissive, m.emissive_strength);
                mat.params = {m.metallic, m.roughness, m.normal_scale, m.alpha_cutoff};
                mat.flags = {m.normal_texture ? 1.0f : 0.0f, m.unlit ? 1.0f : 0.0f, 0, 0};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 1, obj);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 1, mat);
                auto pick = [](rhi::TextureHandle t, rhi::TextureHandle fallback) { return t ? t : fallback; };
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, pick(m.base_color_texture, ctx_.white()), ctx_.linear_repeat());
                pass.bind_texture(rhi::ShaderStage::Fragment, 1, pick(m.metallic_roughness_texture, ctx_.white()), ctx_.linear_repeat());
                pass.bind_texture(rhi::ShaderStage::Fragment, 2, pick(m.normal_texture, ctx_.flat_normal()), ctx_.linear_repeat());
                pass.bind_texture(rhi::ShaderStage::Fragment, 3, pick(m.emissive_texture, ctx_.white()), ctx_.linear_repeat());
                pass.bind_texture(rhi::ShaderStage::Fragment, 4, shadow_tex, ctx_.shadow_compare());
                pass.bind_vertex_buffer(0, d->mesh->vertices);
                pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                pass.draw_indexed(d->mesh->index_count);
            }
            if (!lines_.empty()) {
                pass.bind_pipeline(line_pipeline_);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj);
                pass.bind_vertex_buffer(0, line_buffer_);
                pass.draw(static_cast<std::uint32_t>(lines_.size()));
            }
        });

    const rhi::PipelineHandle tm = tonemap.value();
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& b) {
            b.read(hdr);
            b.write_color(out, rhi::LoadOp::DontCare);
        },
        [&](PassContext& ctx) {
            ctx.pass->bind_pipeline(tm);
            PostUniforms post{{env_.exposure, env_.vignette, is_srgb(target_format) ? 0.0f : 1.0f, env_.tonemap ? 1.0f : 0.0f}};
            ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, post);
            ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(hdr), ctx_.linear_clamp());
            ctx.pass->draw(3);
        });

    VICTUS_TRY_STATUS(graph.compile());
    VICTUS_TRY_STATUS(graph.execute(dev, cmd, pool_));
    graph_info_ = graph.info();
    return {};
}

} // namespace victus::render
