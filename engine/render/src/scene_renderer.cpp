// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/render/light_clusters.hpp"
#include "victus/render/temporal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace victus::render {
using namespace math;

namespace {
// std140-compatible uniform blocks; must match engine/render/shaders.
struct FrameUniforms {
    Mat4 view_proj;
};
// Per-instance vertex data (vertex buffer slot 1, locations 4..10): model matrix columns, then
// the inverse-transpose 3x3 normal matrix as three padded columns.
struct InstanceData {
    Mat4 model;
    Vec4 normal[3];
};
static_assert(sizeof(InstanceData) == 112);

// normal[0].w carries the first joint matrix of a skinned instance (exact as float up to 2^24).
InstanceData make_instance(const Mat4& model, std::uint32_t palette = 0) {
    InstanceData d;
    d.model = model;
    const Mat4 n = transpose(inverse(model));
    for (int i = 0; i < 3; ++i) d.normal[i] = Vec4(n[i].x, n[i].y, n[i].z, 0.0f);
    d.normal[0].w = static_cast<float>(palette);
    return d;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
std::uint64_t bits(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
std::uint64_t handle_bits(rhi::TextureHandle t) noexcept { return (std::uint64_t{t.generation} << 32) | t.index; }

std::uint64_t material_key(const Material& m) noexcept {
    std::uint64_t h = 0;
    for (float f : {m.base_color.r, m.base_color.g, m.base_color.b, m.base_color.a, m.emissive.r, m.emissive.g,
                    m.emissive.b, m.emissive_strength, m.metallic, m.roughness, m.normal_scale, m.alpha_cutoff,
                    m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y, m.coverage_response})
        h = mix(h, bits(f));
    for (auto t : {m.base_color_texture, m.metallic_roughness_texture, m.normal_texture, m.emissive_texture})
        h = mix(h, handle_bits(t));
    if (m.surface_shader) {
        h = mix(h, (std::uint64_t{m.surface_shader.generation} << 32) | m.surface_shader.index);
        for (const Vec4& p : m.shader_params)
            for (float f : {p.x, p.y, p.z, p.w}) h = mix(h, bits(f));
    }
    return mix(h, (m.double_sided ? 1u : 0u) | (m.unlit ? 2u : 0u) | (m.cast_shadows ? 4u : 0u) | (m.nearest_filter ? 8u : 0u) |
                      (m.vertex_alpha_coverage_mask ? 16u : 0u) | (std::uint64_t{static_cast<std::uint8_t>(m.blend)} << 8));
}

bool same_material(const Material& a, const Material& b) noexcept {
    auto eq = [](const Color& x, const Color& y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; };
    return eq(a.base_color, b.base_color) && eq(a.emissive, b.emissive) && a.emissive_strength == b.emissive_strength &&
           a.metallic == b.metallic && a.roughness == b.roughness && a.normal_scale == b.normal_scale &&
           a.alpha_cutoff == b.alpha_cutoff && a.base_color_texture == b.base_color_texture &&
           a.metallic_roughness_texture == b.metallic_roughness_texture && a.normal_texture == b.normal_texture &&
           a.emissive_texture == b.emissive_texture && a.double_sided == b.double_sided && a.unlit == b.unlit &&
           a.cast_shadows == b.cast_shadows && a.uv_scale == b.uv_scale && a.uv_offset == b.uv_offset &&
           a.nearest_filter == b.nearest_filter && a.surface_shader == b.surface_shader &&
           std::equal(a.shader_params.begin(), a.shader_params.end(), b.shader_params.begin(),
                      [](const Vec4& x, const Vec4& y) { return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w; }) &&
           a.vertex_alpha_coverage_mask == b.vertex_alpha_coverage_mask && a.coverage_response == b.coverage_response &&
           a.blend == b.blend;
}
struct LightingUniforms {
    Vec4 camera_pos;
    Vec4 camera_forward;
    Vec4 sun_dir;
    Vec4 sun_color;
    Vec4 sky_color;
    Vec4 horizon_color;
    Vec4 ground_color;
    Vec4 shadow_params;
    Vec4 cascade_params;
    Vec4 cascade_splits;
    Vec4 cascade_texel;
    Vec4 screen;
    Vec4 fog_color;
    Vec4 fog_params;
    Vec4 coverage_color;
    Vec4 coverage_params;
    Vec4 env_params;
    Vec4 env_sh[9];
    Mat4 cascade_vp[4];
    Vec4 cluster_grid;
    Vec4 cluster_depth;
};
struct MaterialUniforms {
    Vec4 base_color;
    Vec4 emissive;
    Vec4 params;
    Vec4 flags;
    Vec4 uv_transform; // xy scale, zw offset
    Vec4 custom[4];    // surface shader parameters
};
struct ProbeUniforms {
    Vec4 box_min, box_max, position;
    Vec4 sh[9];
};
static_assert(sizeof(ProbeUniforms) == 12 * 16);
struct ShadowUniforms {
    Mat4 light_view_proj;
};
struct SkyUniforms {
    Mat4 inv_view_proj;
    Vec4 zenith, horizon, ground, sun_dir, sun_color, env;
};
struct SsaoUniforms {
    Mat4 inv_proj;
    Vec4 params;
    Vec4 screen;
};
struct BlurUniforms {
    Vec4 proj;
    Vec4 screen;
};
struct ParticleFrameUniforms {
    Mat4 view_proj;
    Vec4 right, up;
};
struct ParticleUniforms {
    Vec4 proj;   // view depth = proj.x / (depth + proj.y)
    Vec4 screen; // xy: 1 / render size, z: 1 / softness
};
struct BloomDownUniforms {
    Vec4 params;
    Vec4 threshold;
};
struct ExposureUniforms {
    Vec4 params;
    Vec4 flags;
};
struct PostUniforms {
    Vec4 params, params2, params3;
};
struct TaaUniforms {
    Mat4 reproject;
    Vec4 params, screen;
};
static_assert(sizeof(LightingUniforms) == 28 * 16 + 64 * 4);

constexpr std::uint32_t kLuminanceSize = 64;

bool is_srgb(rhi::Format f) { return f == rhi::Format::RGBA8_SRGB || f == rhi::Format::BGRA8_SRGB; }
Vec4 rgb(Color c, float scale = 1.0f, float w = 0.0f) { return {c.r * scale, c.g * scale, c.b * scale, w}; }
std::chrono::steady_clock::time_point render_epoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}
Vec4 inv_size(std::uint32_t w, std::uint32_t h, float z = 0.0f, float ww = 0.0f) {
    return {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), z, ww};
}
rhi::TextureDesc color_desc(rhi::Format f, std::uint32_t w, std::uint32_t h) {
    rhi::TextureDesc d;
    d.format = f;
    d.width = std::max(1u, w);
    d.height = std::max(1u, h);
    d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    return d;
}

// Coarsest level whose error, scaled to world units and projected at the distance from the eye
// to the nearest point of the draw's bounds, stays within the pixel budget.
std::uint8_t select_lod(const GpuMesh& mesh, const Mat4& model, const Aabb& world, Vec3 eye, float pixels_per_unit,
                        const RenderSettings& s) {
    if (mesh.lod_count <= 1 || (!s.lods && s.force_lod < 0)) return 0;
    const auto last = static_cast<int>(mesh.lod_count) - 1;
    if (s.force_lod >= 0) return static_cast<std::uint8_t>(std::min(s.force_lod, last));
    const Vec3 nearest{std::clamp(eye.x, world.min.x, world.max.x), std::clamp(eye.y, world.min.y, world.max.y),
                       std::clamp(eye.z, world.min.z, world.max.z)};
    const float distance = length(nearest - eye);
    if (!(distance > 1e-4f)) return 0;
    const float scale = std::max({length(model.c[0].xyz()), length(model.c[1].xyz()), length(model.c[2].xyz())});
    const float k = scale * pixels_per_unit / distance; // pixels per unit of mesh error
    int lod = 0;
    while (lod < last && mesh.lods[static_cast<std::size_t>(lod) + 1].error * k <= s.lod_error_pixels) ++lod;
    return static_cast<std::uint8_t>(lod);
}

void draw_mesh_lod(rhi::RenderPass& pass, const GpuMesh& mesh, std::uint32_t lod, std::uint32_t instances) {
    const GpuMeshLod& l = mesh.lods[std::min(lod, mesh.lod_count - 1)];
    pass.draw_indexed(l.index_count, instances, l.first_index);
}

} // namespace

std::string_view tonemapper_name(Tonemapper t) noexcept {
    switch (t) {
    case Tonemapper::None: return "none";
    case Tonemapper::Aces: return "aces";
    case Tonemapper::Neutral: return "neutral";
    case Tonemapper::Reinhard: return "reinhard";
    }
    return "aces";
}

std::optional<Tonemapper> parse_tonemapper(std::string_view n) noexcept {
    for (Tonemapper t : {Tonemapper::None, Tonemapper::Aces, Tonemapper::Neutral, Tonemapper::Reinhard})
        if (tonemapper_name(t) == n) return t;
    return std::nullopt;
}

std::string_view material_blend_name(MaterialBlend b) noexcept {
    switch (b) {
    case MaterialBlend::Alpha: return "alpha";
    case MaterialBlend::Additive: return "additive";
    default: return "opaque";
    }
}

std::optional<MaterialBlend> parse_material_blend(std::string_view name) noexcept {
    if (name == "opaque") return MaterialBlend::Opaque;
    if (name == "alpha") return MaterialBlend::Alpha;
    if (name == "additive") return MaterialBlend::Additive;
    return std::nullopt;
}

std::string_view fog_mode_name(FogMode m) noexcept {
    switch (m) {
    case FogMode::None: return "none";
    case FogMode::Linear: return "linear";
    case FogMode::Exponential: return "exponential";
    case FogMode::ExponentialSquared: return "exponential_squared";
    }
    return "none";
}

std::optional<FogMode> parse_fog_mode(std::string_view n) noexcept {
    for (FogMode m : {FogMode::None, FogMode::Linear, FogMode::Exponential, FogMode::ExponentialSquared})
        if (fog_mode_name(m) == n) return m;
    return std::nullopt;
}

std::string_view quality_tier_name(QualityTier t) noexcept {
    switch (t) {
    case QualityTier::Low: return "low";
    case QualityTier::Medium: return "medium";
    case QualityTier::High: return "high";
    case QualityTier::Ultra: return "ultra";
    }
    return "high";
}

std::optional<QualityTier> parse_quality_tier(std::string_view n) noexcept {
    for (QualityTier t : {QualityTier::Low, QualityTier::Medium, QualityTier::High, QualityTier::Ultra})
        if (quality_tier_name(t) == n) return t;
    return std::nullopt;
}

RenderSettings RenderSettings::preset(QualityTier tier) noexcept {
    RenderSettings s;
    s.tier = tier;
    switch (tier) {
    case QualityTier::Low:
        s.render_scale = 0.75f;
        s.shadow_cascades = 1;
        s.shadow_resolution = 1024;
        s.soft_shadows = false;
        s.ssao = false;
        s.bloom = false;
        s.bloom_levels = 4;
        s.fxaa = true;
        s.taa = false;
        s.max_point_lights = 64;
        s.lod_error_pixels = 2.0f;
        break;
    case QualityTier::Medium:
        s.render_scale = 1.0f;
        s.shadow_cascades = 2;
        s.shadow_resolution = 1024;
        s.soft_shadows = true;
        s.ssao = false;
        s.bloom = true;
        s.bloom_levels = 4;
        s.fxaa = true;
        s.taa = false;
        s.max_point_lights = 256;
        s.lod_error_pixels = 1.5f;
        break;
    case QualityTier::High: break; // struct defaults
    case QualityTier::Ultra:
        s.shadow_cascades = 4;
        s.shadow_resolution = 2048;
        s.ssao_half_resolution = false;
        s.ssao_samples = 16;
        s.bloom_levels = 6;
        s.max_point_lights = 1024;
        s.lod_error_pixels = 0.5f;
        break;
    }
    return s;
}

RenderSettings RenderSettings::sanitized() const noexcept {
    RenderSettings s = *this;
    s.render_scale = std::isfinite(s.render_scale) ? std::clamp(s.render_scale, 0.25f, 2.0f) : 1.0f;
    s.shadow_cascades = std::clamp(s.shadow_cascades, 1u, 4u);
    s.shadow_resolution = std::clamp(s.shadow_resolution, 64u, 4096u);
    s.cascade_split_lambda = std::isfinite(s.cascade_split_lambda) ? std::clamp(s.cascade_split_lambda, 0.0f, 1.0f) : 0.8f;
    s.ssao_samples = std::clamp(s.ssao_samples, 4u, 32u);
    s.bloom_levels = std::clamp(s.bloom_levels, 1u, 8u);
    s.taa_blend = std::isfinite(s.taa_blend) ? std::clamp(s.taa_blend, 0.04f, 1.0f) : 0.1f;
    s.max_point_lights = std::min(s.max_point_lights, SceneRenderer::kMaxLights);
    s.lod_error_pixels = std::isfinite(s.lod_error_pixels) ? std::clamp(s.lod_error_pixels, 0.25f, 16.0f) : 1.0f;
    s.force_lod = std::clamp(s.force_lod, -1, static_cast<int>(kMaxMeshLods) - 1);
    return s;
}

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
    auto depth_vs = get("depth.vert");
    auto skin_vs = get("mesh_skinned.vert");
    auto skin_depth_vs = get("depth_skinned.vert");
    auto skin_shadow_vs = get("shadow_skinned.vert");
    auto particle_vs = get("particle.vert");
    auto particle_fs = get("particle.frag");
    for (auto* s : {&mesh_vs, &pbr_fs, &shadow_vs, &shadow_fs, &full_vs, &sky_fs, &line_vs, &line_fs, &depth_vs, &skin_vs,
                    &skin_depth_vs, &skin_shadow_vs, &particle_vs, &particle_fs})
        if (!*s) return std::move(*s).error();
    const rhi::VertexBufferLayout skin_layout{2, sizeof(SkinVertex), false};
    const std::vector<rhi::VertexAttribute> skin_attrs{{11, 2, rhi::VertexFormat::UShort4, 0},
                                                       {12, 2, rhi::VertexFormat::Float4, 8}};

    const rhi::VertexBufferLayout mesh_layout{0, sizeof(Vertex3D), false};
    const rhi::VertexBufferLayout instance_layout{1, sizeof(InstanceData), true};
    std::vector<rhi::VertexAttribute> model_attrs;
    for (std::uint32_t i = 0; i < 4; ++i) model_attrs.push_back({4 + i, 1, rhi::VertexFormat::Float4, i * 16});
    std::vector<rhi::VertexAttribute> normal_attrs;
    for (std::uint32_t i = 0; i < 3; ++i) normal_attrs.push_back({8 + i, 1, rhi::VertexFormat::Float4, 64 + i * 16});
    auto with = [](std::vector<rhi::VertexAttribute> a, const std::vector<rhi::VertexAttribute>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    rhi::PipelineDesc opaque;
    opaque.name = "pbr_opaque";
    opaque.vertex = mesh_vs.value();
    opaque.fragment = pbr_fs.value();
    opaque.vertex_buffers = {mesh_layout, instance_layout};
    opaque.attributes = with(with({{0, 0, rhi::VertexFormat::Float3, 0},
                                   {1, 0, rhi::VertexFormat::Float3, 12},
                                   {2, 0, rhi::VertexFormat::Float2, 24},
                                   {3, 0, rhi::VertexFormat::Float4, 32},
                                   {13, 0, rhi::VertexFormat::UByte4Norm, 48}},
                                  model_attrs),
                             normal_attrs);
    opaque.depth_test = opaque.depth_write = true;
    opaque.depth_compare = rhi::CompareOp::LessEqual; // also accepts depth pre-pass values
    opaque.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Opaque}};
    opaque.depth_format = r->depth_format_;
    auto p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_cull_ = p.value();
    r->opaque_desc_ = opaque;
    opaque.name = "pbr_double_sided";
    opaque.cull = rhi::CullMode::None;
    p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_double_ = p.value();
    // Skinned variants: same state, extra skin stream and joint palette.
    rhi::PipelineDesc skinned = opaque;
    skinned.name = "pbr_skinned";
    skinned.vertex = skin_vs.value();
    skinned.vertex_buffers = {mesh_layout, instance_layout, skin_layout};
    skinned.attributes = with(opaque.attributes, skin_attrs);
    skinned.cull = rhi::CullMode::Back;
    p = dev.create_pipeline(skinned);
    if (!p) return std::move(p).error();
    r->skin_cull_ = p.value();
    r->skinned_desc_ = skinned;
    skinned.name = "pbr_skinned_double_sided";
    skinned.cull = rhi::CullMode::None;
    p = dev.create_pipeline(skinned);
    if (!p) return std::move(p).error();
    r->skin_double_ = p.value();

    rhi::PipelineDesc prepass;
    prepass.name = "depth_prepass";
    prepass.vertex = depth_vs.value();
    prepass.fragment = shadow_fs.value();
    prepass.vertex_buffers = {mesh_layout, instance_layout};
    prepass.attributes = with({{0, 0, rhi::VertexFormat::Float3, 0}}, model_attrs);
    prepass.depth_test = prepass.depth_write = true;
    prepass.depth_compare = rhi::CompareOp::Less;
    prepass.depth_format = r->depth_format_;
    p = dev.create_pipeline(prepass);
    if (!p) return std::move(p).error();
    r->prepass_cull_ = p.value();
    prepass.name = "depth_prepass_double_sided";
    prepass.cull = rhi::CullMode::None;
    p = dev.create_pipeline(prepass);
    if (!p) return std::move(p).error();
    r->prepass_double_ = p.value();
    rhi::PipelineDesc skin_prepass = prepass;
    skin_prepass.name = "depth_prepass_skinned";
    skin_prepass.vertex = skin_depth_vs.value();
    skin_prepass.vertex_buffers = {mesh_layout, instance_layout, skin_layout};
    skin_prepass.attributes = with(with({{0, 0, rhi::VertexFormat::Float3, 0}}, model_attrs),
                                   {{8, 1, rhi::VertexFormat::Float4, 64}});
    skin_prepass.attributes = with(skin_prepass.attributes, skin_attrs);
    skin_prepass.cull = rhi::CullMode::Back;
    p = dev.create_pipeline(skin_prepass);
    if (!p) return std::move(p).error();
    r->skin_prepass_cull_ = p.value();
    skin_prepass.name = "depth_prepass_skinned_double_sided";
    skin_prepass.cull = rhi::CullMode::None;
    p = dev.create_pipeline(skin_prepass);
    if (!p) return std::move(p).error();
    r->skin_prepass_double_ = p.value();

    rhi::PipelineDesc shadow;
    shadow.name = "shadow_depth";
    shadow.vertex = shadow_vs.value();
    shadow.fragment = shadow_fs.value();
    shadow.vertex_buffers = {mesh_layout, instance_layout};
    shadow.attributes = with({{0, 0, rhi::VertexFormat::Float3, 0}}, model_attrs);
    shadow.cull = rhi::CullMode::None;
    shadow.depth_test = shadow.depth_write = true;
    shadow.depth_compare = rhi::CompareOp::Less;
    shadow.depth_bias_constant = 1.25f;
    shadow.depth_bias_slope = 1.75f;
    shadow.depth_format = r->depth_format_;
    p = dev.create_pipeline(shadow);
    if (!p) return std::move(p).error();
    r->shadow_pipeline_ = p.value();
    shadow.name = "shadow_depth_skinned";
    shadow.vertex = skin_shadow_vs.value();
    shadow.vertex_buffers = {mesh_layout, instance_layout, skin_layout};
    shadow.attributes = with(with(with({{0, 0, rhi::VertexFormat::Float3, 0}}, model_attrs),
                                  {{8, 1, rhi::VertexFormat::Float4, 64}}),
                             skin_attrs);
    p = dev.create_pipeline(shadow);
    if (!p) return std::move(p).error();
    r->skin_shadow_ = p.value();

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

    // Particles: instanced billboards, no depth attachment (the shader samples scene depth).
    for (const bool additive : {false, true}) {
        rhi::PipelineDesc pd;
        pd.name = additive ? "particles_additive" : "particles_alpha";
        pd.vertex = particle_vs.value();
        pd.fragment = particle_fs.value();
        pd.vertex_buffers = {{0, sizeof(ParticleInstance), true}};
        pd.attributes = {{0, 0, rhi::VertexFormat::Float4, 0}, {1, 0, rhi::VertexFormat::Float4, 16}, {2, 0, rhi::VertexFormat::Float4, 32}};
        pd.cull = rhi::CullMode::None;
        pd.color_targets = {{rhi::Format::RGBA16_FLOAT, additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        p = dev.create_pipeline(pd);
        if (!p) return std::move(p).error();
        (additive ? r->particle_additive_ : r->particle_alpha_) = p.value();
    }

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

rhi::PipelineHandle SceneRenderer::blend_pipeline(MaterialBlend blend, bool skinned, bool double_sided) {
    if (blend == MaterialBlend::Opaque) return {};
    const std::size_t i = (blend == MaterialBlend::Additive ? 4u : 0u) + (skinned ? 2u : 0u) + (double_sided ? 1u : 0u);
    if (!blend_pipelines_[i]) {
        rhi::PipelineDesc d = skinned ? skinned_desc_ : opaque_desc_;
        d.name = std::format("pbr_{}{}{}", material_blend_name(blend), skinned ? "_skinned" : "", double_sided ? "_double_sided" : "");
        d.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        d.depth_write = false;
        d.color_targets = {{rhi::Format::RGBA16_FLOAT, blend == MaterialBlend::Additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        auto p = ctx_.device().create_pipeline(d);
        if (!p) {
            VICTUS_LOG_ERROR("render", "blend pipeline: {}", p.error().message);
            return {};
        }
        blend_pipelines_[i] = *p;
    }
    return blend_pipelines_[i];
}

rhi::PipelineHandle SceneRenderer::surface_pipeline(rhi::ShaderHandle fragment, bool skinned, bool double_sided, MaterialBlend blend) {
    const std::uint64_t key = (std::uint64_t{fragment.generation} << 32) | fragment.index;
    auto it = surface_pipelines_.find(key);
    if (it == surface_pipelines_.end()) {
        // Hot reload creates new shader handles; drop the oldest sets instead of growing forever.
        if (surface_pipelines_.size() >= 64) {
            for (auto& [k, set] : surface_pipelines_)
                for (auto pl : set)
                    if (pl) ctx_.device().destroy(pl);
            surface_pipelines_.clear();
        }
        it = surface_pipelines_.emplace(key, std::array<rhi::PipelineHandle, 12>{}).first;
    }
    // Variants are created on first use: [blend][skinned][double_sided].
    const std::size_t i = static_cast<std::size_t>(blend) * 4u + (skinned ? 2u : 0u) + (double_sided ? 1u : 0u);
    if (i >= it->second.size()) return {};
    if (!it->second[i]) {
        if (surface_failed_.contains(key)) return {};
        rhi::PipelineDesc d = skinned ? skinned_desc_ : opaque_desc_;
        d.name = std::format("surface_{}_{}", fragment.index, i);
        d.fragment = fragment;
        d.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        if (blend != MaterialBlend::Opaque) {
            d.depth_write = false;
            d.color_targets = {{rhi::Format::RGBA16_FLOAT, blend == MaterialBlend::Additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        }
        auto p = ctx_.device().create_pipeline(d);
        if (!p) {
            VICTUS_LOG_ERROR("render", "surface shader pipeline: {}", p.error().message);
            surface_failed_.insert(key);
            return {};
        }
        it->second[i] = *p;
    }
    return it->second[i];
}

SceneRenderer::~SceneRenderer() {
    auto& d = ctx_.device();
    for (auto pl : blend_pipelines_)
        if (pl) d.destroy(pl);
    for (auto pl : {opaque_cull_, opaque_double_, shadow_pipeline_, sky_pipeline_, line_pipeline_, prepass_cull_,
                    prepass_double_, skin_cull_, skin_double_, skin_prepass_cull_, skin_prepass_double_, skin_shadow_,
                    particle_alpha_, particle_additive_})
        if (pl) d.destroy(pl);
    if (palette_buffer_) d.destroy(palette_buffer_);
    if (light_buffer_) d.destroy(light_buffer_);
    if (cluster_buffer_) d.destroy(cluster_buffer_);
    for (auto& [k, pl] : post_) d.destroy(pl);
    for (auto& [k, set] : surface_pipelines_)
        for (auto pl : set)
            if (pl) d.destroy(pl);
    if (no_shadow_) d.destroy(no_shadow_);
    for (auto t : exposure_)
        if (t) d.destroy(t);
    for (auto t : history_)
        if (t) d.destroy(t);
    if (line_buffer_) d.destroy(line_buffer_);
    if (instance_buffer_) d.destroy(instance_buffer_);
    if (particle_buffer_) d.destroy(particle_buffer_);
    pool_.clear();
}

Result<rhi::PipelineHandle> SceneRenderer::post_pipeline(const char* fragment, rhi::Format format, rhi::BlendMode blend) {
    std::string key = std::format("{}/{}/{}", fragment, static_cast<int>(format), static_cast<int>(blend));
    if (auto it = post_.find(key); it != post_.end()) return it->second;
    auto vs = ctx_.shader("fullscreen.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx_.shader(fragment);
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc d;
    d.name = fragment;
    d.vertex = vs.value();
    d.fragment = fs.value();
    d.cull = rhi::CullMode::None;
    d.color_targets = {{format, blend}};
    auto p = ctx_.device().create_pipeline(d);
    if (!p) return p;
    post_.emplace(std::move(key), p.value());
    return p;
}

Status SceneRenderer::ensure_exposure_textures() {
    if (exposure_[0]) return {};
    for (int i = 0; i < 2; ++i) {
        rhi::TextureDesc d = color_desc(rhi::Format::R32_FLOAT, 1, 1);
        d.name = i == 0 ? "exposure_a" : "exposure_b";
        auto t = ctx_.device().create_texture(d);
        if (!t) return std::move(t).error();
        exposure_[i] = t.value();
    }
    adaptation_valid_ = false;
    return {};
}

Status SceneRenderer::ensure_history(std::uint32_t index, std::uint32_t width, std::uint32_t height) {
    if (history_[index] && history_w_[index] == width && history_h_[index] == height) return {};
    if (history_[index]) ctx_.device().destroy(history_[index]);
    history_[index] = {};
    rhi::TextureDesc d = color_desc(rhi::Format::RGBA16_FLOAT, width, height);
    d.name = index == 0 ? "taa_history_a" : "taa_history_b";
    auto t = ctx_.device().create_texture(d);
    if (!t) return std::move(t).error();
    history_[index] = t.value();
    history_w_[index] = width;
    history_h_[index] = height;
    return {};
}

void SceneRenderer::begin(const Camera3D& camera, const DirectionalLight& sun, const Environment& env) {
    camera_ = camera;
    sun_ = sun;
    env_ = env;
    points_.clear();
    spots_.clear();
    draws_.clear();
    probes_.clear();
    lines_.clear();
    palette_.clear();
    particles_.clear();
    particle_batches_.clear();
    stats_ = {};
}

void SceneRenderer::add_point_light(const PointLight& light) { points_.push_back(light); }
void SceneRenderer::add_spot_light(const SpotLight& light) { spots_.push_back(light); }

void SceneRenderer::add_reflection_probe(const ReflectionProbe& probe) {
    if (!probe.map || !probe.map->cube() || !probe.box.valid() || probes_.size() >= kMaxReflectionProbes) return;
    probes_.push_back(probe);
}

void SceneRenderer::submit(const GpuMesh& mesh, const Material& material, const Mat4& model) {
    if (!mesh.valid()) return;
    ++stats_.submitted;
    draws_.push_back({&mesh, material, model, 0.0f,
                      mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh))});
}

void SceneRenderer::submit_skinned(const GpuMesh& mesh, const Material& material, const Mat4& model,
                                   std::span<const Mat4> joints, const Aabb& world_bounds) {
    if (!mesh.skinned() || joints.empty()) {
        submit(mesh, material, model);
        return;
    }
    if (!mesh.valid()) return;
    ++stats_.submitted;
    ++stats_.skinned_draws;
    Draw d{&mesh, material, model, 0.0f, mix(mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh)), 0x5c1du)};
    d.palette = static_cast<std::uint32_t>(palette_.size());
    d.skinned = true;
    d.world_bounds = world_bounds;
    palette_.insert(palette_.end(), joints.begin(), joints.end());
    draws_.push_back(d);
}

void SceneRenderer::submit_particles(std::span<const ParticleInstance> particles, const ParticleMaterial& material,
                                     const Aabb& bounds) {
    if (particles.empty() || !bounds.valid()) return;
    particle_batches_.push_back({material, static_cast<std::uint32_t>(particles_.size()),
                                 static_cast<std::uint32_t>(particles.size()), bounds, 0.0f});
    particles_.insert(particles_.end(), particles.begin(), particles.end());
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
    // Lines are emitted as short segments: long lines that cross the camera plane are clipped
    // into degenerate, screen-filling primitives by some rasterizers (observed on llvmpipe).
    const int cells = static_cast<int>(std::floor(2.0f * half / spacing + 1e-4f));
    for (int i = 0; i <= cells; ++i) {
        const float x = -half + static_cast<float>(i) * spacing;
        for (int j = 0; j < cells; ++j) {
            const float z0 = -half + static_cast<float>(j) * spacing, z1 = z0 + spacing;
            line({x, 0, z0}, {x, 0, z1}, color);
            line({z0, 0, x}, {z1, 0, x}, color);
        }
    }
}

void SceneRenderer::compute_cascades(float aspect, std::uint32_t resolution) {
    const Vec3 d = normalize(sun_.direction);
    const Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Mat4 light_view = Mat4::look_at({0, 0, 0}, d, up);
    const Mat4 inv_view = inverse(camera_.view);
    const float n = std::max(camera_.z_near, 0.01f);
    const float f = std::max(n + 0.1f, std::min(sun_.shadow_distance, camera_.z_far));
    const std::uint32_t count = settings_.shadow_cascades;
    const float lambda = settings_.cascade_split_lambda;
    const float tan_y = std::tan(camera_.fov_y * 0.5f), tan_x = tan_y * aspect;
    cascade_count_ = count;
    cascade_splits_ = {};
    cascade_texel_ = {};
    float prev = n;
    for (std::uint32_t i = 0; i < 4; ++i) cascade_vp_[i] = Mat4::identity();
    for (std::uint32_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(count);
        const float split = lambda * n * std::pow(f / n, t) + (1.0f - lambda) * (n + (f - n) * t);
        cascade_splits_[i] = split;
        // Bounding sphere of the frustum slice: rotation invariant, so the projection only
        // changes when the camera moves (then snapped to whole texels).
        Vec3 center{};
        Vec3 corners[8];
        int k = 0;
        for (float z : {prev, split})
            for (float sy : {-1.0f, 1.0f})
                for (float sx : {-1.0f, 1.0f}) {
                    corners[k] = inv_view.transform_point({sx * tan_x * z, sy * tan_y * z, -z});
                    center = center + corners[k];
                    ++k;
                }
        center = center * (1.0f / 8.0f);
        float r = 0.0f;
        for (const Vec3& c : corners) r = std::max(r, length(c - center));
        r = std::ceil(std::max(r, 0.5f) * 16.0f) / 16.0f;
        Vec3 c = light_view.transform_point(center);
        const float texel = (2.0f * r) / static_cast<float>(resolution);
        c.x = std::floor(c.x / texel) * texel;
        c.y = std::floor(c.y / texel) * texel;
        // Depth range extends towards the light so off-screen casters still cast.
        const float back = std::max(r * 4.0f, 30.0f);
        const Mat4 proj = Mat4::orthographic(c.x - r, c.x + r, c.y - r, c.y + r, -c.z - back, -c.z + r);
        cascade_vp_[i] = proj * light_view;
        cascade_texel_[i] = texel;
        prev = split;
    }
}

Status SceneRenderer::render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width, std::uint32_t height,
                             rhi::Format target_format) {
    VICTUS_PROFILE_SCOPE("SceneRenderer::render");
    if (width == 0 || height == 0) return {};
    auto& dev = ctx_.device();
    const RenderSettings& S = settings_;

    // Internal 3D resolution (dynamic resolution / render scale).
    const auto scaled = [&](std::uint32_t v) {
        return std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(v) * S.render_scale)));
    };
    const std::uint32_t rw = scaled(width), rh = scaled(height);
    stats_.render_width = rw;
    stats_.render_height = rh;

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 proj_unjittered = camera_.projection(aspect);
    const Mat4 view_proj_unjittered = proj_unjittered * camera_.view;
    const Frustum frustum = Frustum::from_matrix(view_proj_unjittered);
    // TAA: every 3D pass renders with a sub-pixel jittered projection.
    const bool taa = S.taa && !capture_mode_;
    if (!taa) history_valid_ = false;
    const math::Vec2 jitter = taa ? taa_jitter(frame_index_) : math::Vec2{};
    const Mat4 proj = taa ? jitter_projection(proj_unjittered, jitter, rw, rh) : proj_unjittered;
    const Mat4 view_proj = proj * camera_.view;
    stats_.taa = taa;
    stats_.taa_history = taa && history_valid_;
    stats_.jitter = jitter;
    const bool shadows = sun_.shadows && sun_.intensity > 0.0f;
    const std::uint32_t shadow_res = std::max(64u, std::min(sun_.shadow_resolution, S.shadow_resolution));
    if (shadows) compute_cascades(aspect, shadow_res);
    else {
        cascade_count_ = 0;
        cascade_splits_ = {};
        for (auto& m : cascade_vp_) m = Mat4::identity();
    }
    stats_.shadow_cascades = cascade_count_;
    std::array<Frustum, 4> cascade_frustum{};
    for (std::uint32_t c = 0; c < cascade_count_; ++c) cascade_frustum[c] = Frustum::from_matrix(cascade_vp_[c]);

    // Visibility + sorting (front-to-back within pipeline and base texture runs).
    std::vector<const Draw*> visible, transparent;
    std::array<std::vector<const Draw*>, 4> casters;
    visible.reserve(draws_.size());
    // Pixels per unit of geometric error at distance 1 (perspective, render resolution).
    const float lod_pixels_per_unit = static_cast<float>(rh) / (2.0f * std::tan(std::max(camera_.fov_y, 1e-3f) * 0.5f));
    for (Draw& d : draws_) {
        const Aabb world = d.skinned ? d.world_bounds : d.mesh->bounds.transformed(d.model);
        const bool blended = d.material.blend != MaterialBlend::Opaque;
        d.lod = select_lod(*d.mesh, d.model, world, camera_.position, lod_pixels_per_unit, S);
        if (d.material.cast_shadows && !blended) {
            bool any = false;
            for (std::uint32_t c = 0; c < cascade_count_; ++c)
                if (cascade_frustum[c].intersects(world)) {
                    casters[c].push_back(&d);
                    any = true;
                }
            if (any) ++stats_.shadow_casters;
        }
        if (!frustum.intersects(world)) {
            ++stats_.culled;
            continue;
        }
        d.distance = length(world.center() - camera_.position);
        d.probe = -1;
        float best = std::numeric_limits<float>::max();
        const Vec3 c = world.center();
        for (std::size_t p = 0; p < probes_.size(); ++p) {
            const Aabb& b = probes_[p].box;
            if (c.x < b.min.x || c.y < b.min.y || c.z < b.min.z || c.x > b.max.x || c.y > b.max.y || c.z > b.max.z) continue;
            const Vec3 e = b.max - b.min;
            const float volume = e.x * e.y * e.z;
            if (volume < best) {
                best = volume;
                d.probe = static_cast<int>(p);
            }
        }
        if (d.probe >= 0) ++stats_.probe_draws;
        if (d.lod > 0) ++stats_.lod_draws;
        stats_.triangles += d.mesh->lods[d.lod].index_count / 3;
        (blended ? transparent : visible).push_back(&d);
    }
    std::stable_sort(transparent.begin(), transparent.end(), [](const Draw* a, const Draw* b) { return a->distance > b->distance; });
    std::sort(visible.begin(), visible.end(), [](const Draw* a, const Draw* b) {
        if (a->material.surface_shader.index != b->material.surface_shader.index)
            return a->material.surface_shader.index < b->material.surface_shader.index;
        if (a->material.double_sided != b->material.double_sided) return !a->material.double_sided;
        if (a->material.base_color_texture.index != b->material.base_color_texture.index)
            return a->material.base_color_texture.index < b->material.base_color_texture.index;
        if (a->batch_key != b->batch_key) return a->batch_key < b->batch_key;
        if (a->lod != b->lod) return a->lod < b->lod;
        if (a->probe != b->probe) return a->probe < b->probe;
        return a->distance < b->distance;
    });
    stats_.visible = static_cast<std::uint32_t>(visible.size());

    // Instancing: consecutive draws with the same mesh and identical material become one batch.
    // Every pass reads its own contiguous range of the per-frame instance buffer.
    std::vector<InstanceData> instances;
    instances.reserve(visible.size() + stats_.shadow_casters);
    auto build_batches = [&](const std::vector<const Draw*>& list, bool match_material) {
        std::vector<Batch> out;
        for (const Draw* d : list) {
            const auto offset = static_cast<std::uint32_t>(instances.size());
            instances.push_back(make_instance(d->model, d->palette));
            if (S.instancing && !out.empty()) {
                const Draw* f = out.back().first;
                if (f->mesh == d->mesh && f->lod == d->lod && f->skinned == d->skinned &&
                    (!match_material || (same_material(f->material, d->material) && f->probe == d->probe))) {
                    ++out.back().instance_count;
                    continue;
                }
            }
            out.push_back({d, offset, 1});
        }
        return out;
    };
    const std::vector<Batch> main_batches = build_batches(visible, true);
    const std::vector<Batch> transparent_batches = build_batches(transparent, true);
    stats_.transparent_draws = static_cast<std::uint32_t>(transparent.size());
    std::array<std::vector<Batch>, 4> shadow_batches;
    for (std::uint32_t c = 0; c < cascade_count_; ++c) {
        std::sort(casters[c].begin(), casters[c].end(), [](const Draw* a, const Draw* b) {
            if (a->skinned != b->skinned) return !a->skinned;
            if (a->mesh != b->mesh) return a->mesh < b->mesh;
            return a->lod < b->lod;
        });
        shadow_batches[c] = build_batches(casters[c], false);
        stats_.shadow_batches += static_cast<std::uint32_t>(shadow_batches[c].size());
    }
    stats_.main_batches = static_cast<std::uint32_t>(main_batches.size());
    stats_.reflection_probes = static_cast<std::uint32_t>(probes_.size());
    stats_.instances_uploaded = static_cast<std::uint32_t>(instances.size());
    stats_.joints_uploaded = static_cast<std::uint32_t>(palette_.size());
    if (!palette_.empty()) {
        const auto needed = static_cast<std::uint32_t>(palette_.size());
        if (needed > palette_capacity_) {
            if (palette_buffer_) dev.destroy(palette_buffer_);
            palette_capacity_ = std::max({needed, palette_capacity_ * 2, 256u});
            auto b = dev.create_buffer({"joint_palette", palette_capacity_ * static_cast<std::uint32_t>(sizeof(Mat4)),
                                        rhi::BufferUsageStorageRead});
            if (!b) return std::move(b).error();
            palette_buffer_ = b.value();
        }
        cmd.upload_buffer(palette_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(palette_.data()), palette_.size() * sizeof(Mat4)}, true);
    }
    if (!instances.empty()) {
        const auto needed = static_cast<std::uint32_t>(instances.size());
        if (needed > instance_capacity_) {
            if (instance_buffer_) dev.destroy(instance_buffer_);
            instance_capacity_ = std::max({needed, instance_capacity_ * 2, 256u});
            auto b = dev.create_buffer(
                {"scene_instances", instance_capacity_ * static_cast<std::uint32_t>(sizeof(InstanceData)),
                 rhi::BufferUsageVertex});
            if (!b) return std::move(b).error();
            instance_buffer_ = b.value();
        }
        cmd.upload_buffer(instance_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(instances.data()), instances.size() * sizeof(InstanceData)}, true);
    }
    constexpr auto kInstanceStride = static_cast<std::uint32_t>(sizeof(InstanceData));
    for (std::uint32_t c = 0; c < cascade_count_; ++c) stats_.shadow_draws += static_cast<std::uint32_t>(casters[c].size());

    // Local lights: frustum-cull bounding spheres, keep the nearest when over budget, then
    // assign them to view-space clusters and upload both storage buffers.
    std::vector<GpuLight> local_lights;
    local_lights.reserve(points_.size() + spots_.size());
    std::uint32_t spot_count = 0;
    {
        const auto finite_or = [](float v, float d) { return std::isfinite(v) ? v : d; };
        for (const PointLight& pl : points_) {
            GpuLight g{};
            g.position_range = Vec4(pl.position, std::max(finite_or(pl.range, 0.01f), 0.01f));
            g.color_intensity = rgb(pl.color, 1.0f, std::max(finite_or(pl.intensity, 0.0f), 0.0f));
            local_lights.push_back(g);
        }
        for (const SpotLight& sl : spots_) {
            GpuLight g{};
            const float outer = std::clamp(finite_or(sl.outer_angle, 0.5f), 0.001f, 3.1f);
            const float inner = std::clamp(finite_or(sl.inner_angle, 0.35f), 0.0f, outer);
            const Vec3 dir = length(sl.direction) > 1e-6f ? normalize(sl.direction) : Vec3{0, -1, 0};
            g.position_range = Vec4(sl.position, std::max(finite_or(sl.range, 0.01f), 0.01f));
            g.color_intensity = rgb(sl.color, 1.0f, std::max(finite_or(sl.intensity, 0.0f), 0.0f));
            g.direction_cos_outer = Vec4(dir, std::cos(outer));
            g.spot = {std::cos(inner), 1.0f, 0.0f, 0.0f};
            local_lights.push_back(g);
        }
        std::erase_if(local_lights, [&](const GpuLight& g) {
            const Vec4 b = light_bounding_sphere(g);
            return g.color_intensity.w <= 0.0f ||
                   !frustum.intersects(Aabb{{b.x - b.w, b.y - b.w, b.z - b.w}, {b.x + b.w, b.y + b.w, b.z + b.w}});
        });
        // Nearest first: the budget and the per-cluster cap then drop the farthest lights.
        const std::size_t budget = std::min<std::size_t>(S.max_point_lights, local_lights.size());
        std::vector<std::pair<float, std::uint32_t>> order(local_lights.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) {
            const Vec4 b = light_bounding_sphere(local_lights[i]);
            order[i] = {length(Vec3{b.x, b.y, b.z} - camera_.position) - b.w, i};
        }
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(budget), order.end());
        std::vector<GpuLight> kept(budget);
        for (std::size_t i = 0; i < budget; ++i) kept[i] = local_lights[order[i].second];
        local_lights = std::move(kept);
        for (const GpuLight& g : local_lights) spot_count += g.spot.y > 0.5f ? 1u : 0u;
    }
    const LightClusters clusters =
        build_light_clusters(local_lights, camera_.view, proj_unjittered, camera_.z_near, camera_.z_far);
    stats_.point_lights = static_cast<std::uint32_t>(local_lights.size());
    stats_.spot_lights = spot_count;
    stats_.light_indices = clusters.index_count;
    stats_.max_cluster_lights = clusters.max_per_cluster;
    {
        const auto upload = [&](rhi::BufferHandle& buf, std::uint32_t& capacity, const char* name, const void* data,
                                std::size_t bytes) -> Status {
            const auto needed = static_cast<std::uint32_t>(std::max<std::size_t>(bytes, 64));
            if (needed > capacity) {
                if (buf) dev.destroy(buf);
                capacity = std::max({needed, capacity * 2, 4096u});
                auto b = dev.create_buffer({name, capacity, rhi::BufferUsageStorageRead});
                if (!b) return std::move(b).error();
                buf = b.value();
            }
            static const std::array<std::uint8_t, 64> zeros{};
            if (bytes == 0) cmd.upload_buffer(buf, 0, {zeros.data(), zeros.size()}, true);
            else cmd.upload_buffer(buf, 0, {static_cast<const std::uint8_t*>(data), bytes}, true);
            return {};
        };
        VICTUS_TRY_STATUS(upload(light_buffer_, light_capacity_, "local_lights", local_lights.data(),
                                 local_lights.size() * sizeof(GpuLight)));
        VICTUS_TRY_STATUS(upload(cluster_buffer_, cluster_capacity_, "light_clusters", clusters.data.data(),
                                 clusters.data.size() * sizeof(std::uint32_t)));
    }

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
                          {reinterpret_cast<const std::uint8_t*>(lines_.data()), lines_.size() * sizeof(LineVertex)}, true);
    }

    // Particles: cull batches, order them back to front, sort alpha batches' particles back to
    // front, then upload everything as one instance stream.
    std::vector<ParticleBatch> particle_draws;
    particle_upload_.clear();
    for (ParticleBatch b : particle_batches_) {
        if (!frustum.intersects(b.bounds)) continue;
        b.distance = length(b.bounds.center() - camera_.position);
        particle_draws.push_back(b);
    }
    std::stable_sort(particle_draws.begin(), particle_draws.end(),
                     [](const ParticleBatch& a, const ParticleBatch& b) { return a.distance > b.distance; });
    {
        const Vec3 fwd = camera_.forward();
        std::vector<std::pair<float, std::uint32_t>> order;
        for (ParticleBatch& b : particle_draws) {
            const auto first = static_cast<std::uint32_t>(particle_upload_.size());
            if (b.material.blend == ParticleBlend::Alpha) {
                order.clear();
                for (std::uint32_t i = 0; i < b.count; ++i) {
                    const Vec4& p = particles_[b.offset + i].position_size;
                    order.emplace_back(dot(Vec3{p.x, p.y, p.z} - camera_.position, fwd), b.offset + i);
                }
                std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
                for (const auto& o : order) particle_upload_.push_back(particles_[o.second]);
            } else {
                particle_upload_.insert(particle_upload_.end(), particles_.begin() + b.offset, particles_.begin() + b.offset + b.count);
            }
            b.offset = first;
        }
    }
    stats_.particle_batches = static_cast<std::uint32_t>(particle_draws.size());
    stats_.particles = static_cast<std::uint32_t>(particle_upload_.size());
    if (!particle_upload_.empty()) {
        const auto needed = static_cast<std::uint32_t>(particle_upload_.size());
        if (needed > particle_capacity_) {
            if (particle_buffer_) dev.destroy(particle_buffer_);
            particle_capacity_ = std::max(needed, particle_capacity_ * 2);
            auto b = dev.create_buffer({"particles", particle_capacity_ * static_cast<std::uint32_t>(sizeof(ParticleInstance)),
                                        rhi::BufferUsageVertex});
            if (!b) return std::move(b).error();
            particle_buffer_ = b.value();
        }
        cmd.upload_buffer(particle_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(particle_upload_.data()),
                           particle_upload_.size() * sizeof(ParticleInstance)},
                          true);
    }

    const bool ssao = S.ssao && env_.ao_intensity > 0.0f && env_.ao_radius > 0.0f;
    const bool prepass = S.depth_prepass || ssao;
    const bool bloom = S.bloom && env_.bloom_intensity > 0.0f;
    const bool auto_exposure = S.auto_exposure && env_.auto_exposure;
    const bool linear_out = env_.linear_output || capture_mode_;
    const bool fxaa = S.fxaa && !linear_out && !taa; // captures keep float precision
    if (auto_exposure) VICTUS_TRY_STATUS(ensure_exposure_textures());

    const std::uint32_t atlas_tiles = cascade_count_ > 1 ? 2u : 1u;
    const std::uint32_t atlas_size = shadow_res * atlas_tiles;

    LightingUniforms lighting{};
    lighting.camera_pos = Vec4(camera_.position, ssao ? 1.0f : 0.0f);
    lighting.camera_forward = Vec4(camera_.forward(), 0.0f);
    lighting.sun_dir = Vec4(normalize(sun_.direction), shadows ? 1.0f : 0.0f);
    lighting.sun_color = rgb(sun_.color, sun_.intensity);
    lighting.sky_color = rgb(env_.sky_zenith, env_.ambient_intensity, env_.exposure);
    lighting.horizon_color = rgb(env_.sky_horizon, env_.ambient_intensity);
    lighting.ground_color = rgb(env_.ground, env_.ambient_intensity, static_cast<float>(local_lights.size()));
    lighting.cluster_grid = {static_cast<float>(clusters.grid.tiles_x), static_cast<float>(clusters.grid.tiles_y),
                             static_cast<float>(clusters.grid.slices), static_cast<float>(clusters.grid.max_lights_per_cluster)};
    lighting.cluster_depth = {clusters.z_near, static_cast<float>(clusters.grid.slices) / std::log(clusters.z_far / clusters.z_near), 0, 0};
    lighting.shadow_params = {1.0f / static_cast<float>(atlas_size), 0.0004f, 1.0f, static_cast<float>(cascade_count_)};
    lighting.cascade_params = {S.soft_shadows ? 1.0f : 0.0f, 1.0f / static_cast<float>(atlas_tiles), 0.1f, 0.0f};
    lighting.cascade_splits = {cascade_splits_[0], cascade_splits_[1], cascade_splits_[2], cascade_splits_[3]};
    lighting.cascade_texel = {cascade_texel_[0], cascade_texel_[1], cascade_texel_[2], cascade_texel_[3]};
    {
        double t = time_ ? *time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - render_epoch()).count();
        t = std::fmod(std::max(t, 0.0), 3600.0); // keeps float precision for sin(time) and scrolling
        lighting.screen = inv_size(rw, rh, static_cast<float>(t));
        const float ai = env_.ambient_intensity;
        lighting.fog_color = {env_.fog_color.r * ai, env_.fog_color.g * ai, env_.fog_color.b * ai,
                              static_cast<float>(env_.fog_mode)};
        const float start = std::max(env_.fog_start, 0.0f);
        lighting.fog_params = {start, std::max(env_.fog_end, start + 0.001f), std::max(env_.fog_density, 0.0f),
                               std::clamp(env_.fog_max_opacity, 0.0f, 1.0f)};
        lighting.coverage_color = {env_.coverage_color.r, env_.coverage_color.g, env_.coverage_color.b,
                                   std::clamp(env_.coverage_amount, 0.0f, 1.0f)};
        lighting.coverage_params = {std::clamp(env_.coverage_roughness, 0.045f, 1.0f), std::clamp(env_.coverage_slope, -1.0f, 1.0f),
                                    std::max(env_.coverage_sharpness, 0.01f), 0.0f};
    }
    const EnvironmentMap* env_map = env_.environment_map;
    const float env_intensity = std::isfinite(env_.environment_intensity) ? std::max(env_.environment_intensity, 0.0f) : 1.0f;
    const float env_rotation = std::isfinite(env_.environment_rotation) ? env_.environment_rotation : 0.0f;
    if (env_map && env_map->cube()) {
        lighting.env_params = {1.0f, static_cast<float>(env_map->mip_count() - 1), env_intensity, env_rotation};
        for (std::size_t i = 0; i < 9; ++i) lighting.env_sh[i] = Vec4(env_map->sh().coefficients[i] * env_intensity, 0.0f);
    }
    for (std::uint32_t c = 0; c < 4; ++c) lighting.cascade_vp[c] = cascade_vp_[c];

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
        sd.width = sd.height = atlas_size;
        sd.usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
        shadow_map = graph.create_texture("shadow_atlas", sd);
        graph.add_pass(
            "shadow", [&](PassBuilder& b) { b.write_depth(shadow_map, rhi::LoadOp::Clear, 1.0f); },
            [&](PassContext& ctx) {
                for (std::uint32_t c = 0; c < cascade_count_; ++c) {
                    const float x = static_cast<float>((c & 1u) * shadow_res), y = static_cast<float>((c >> 1u) * shadow_res);
                    ctx.pass->set_viewport({x, y, static_cast<float>(shadow_res), static_cast<float>(shadow_res), 0, 1});
                    ShadowUniforms su{cascade_vp_[c]};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, su);
                    rhi::PipelineHandle bound{};
                    for (const Batch& b : shadow_batches[c]) {
                        const rhi::PipelineHandle want = b.first->skinned ? skin_shadow_ : shadow_pipeline_;
                        if (!(bound == want)) {
                            ctx.pass->bind_pipeline(want);
                            if (b.first->skinned) ctx.pass->bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                            bound = want;
                        }
                        if (b.first->skinned) ctx.pass->bind_vertex_buffer(2, b.first->mesh->skin);
                        ctx.pass->bind_vertex_buffer(0, b.first->mesh->vertices);
                        ctx.pass->bind_vertex_buffer(1, instance_buffer_, b.instance_offset * kInstanceStride);
                        ctx.pass->bind_index_buffer(b.first->mesh->indices, rhi::IndexType::U32);
                        draw_mesh_lod(*ctx.pass, *b.first->mesh, b.first->lod, b.instance_count);
                    }
                }
            });
    }

    rhi::TextureDesc hdr_desc = color_desc(rhi::Format::RGBA16_FLOAT, rw, rh);
    const RGTexture hdr = graph.create_texture("hdr_color", hdr_desc);
    rhi::TextureDesc depth_desc = hdr_desc;
    depth_desc.format = depth_format_;
    depth_desc.usage = rhi::TextureUsageDepthTarget | ((ssao || taa || !particle_draws.empty()) ? rhi::TextureUsageSampled : 0u);
    const RGTexture depth = graph.create_texture("scene_depth", depth_desc);

    if (prepass) {
        graph.add_pass(
            "depth_prepass", [&](PassBuilder& b) { b.write_depth(depth, rhi::LoadOp::Clear, 1.0f); },
            [&](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                FrameUniforms frame{view_proj};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
                rhi::PipelineHandle bound{};
                for (const Batch& b : main_batches) {
                    const Draw* d = b.first;
                    // Alpha-tested and surface-shaded draws (which may discard): depth comes from the main pass.
                    if (d->material.alpha_cutoff > 0.0f || d->material.surface_shader) continue;
                    const rhi::PipelineHandle want = d->skinned ? (d->material.double_sided ? skin_prepass_double_ : skin_prepass_cull_)
                                                                : (d->material.double_sided ? prepass_double_ : prepass_cull_);
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                        bound = want;
                    }
                    if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                    pass.bind_vertex_buffer(0, d->mesh->vertices);
                    pass.bind_vertex_buffer(1, instance_buffer_, b.instance_offset * kInstanceStride);
                    pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                    draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                    stats_.prepass_draws += b.instance_count;
                    ++stats_.prepass_batches;
                }
            });
    }

    RGTexture ao;
    if (ssao) {
        const std::uint32_t aw = S.ssao_half_resolution ? std::max(1u, rw / 2) : rw;
        const std::uint32_t ah = S.ssao_half_resolution ? std::max(1u, rh / 2) : rh;
        const RGTexture ao_raw = graph.create_texture("ssao_raw", color_desc(rhi::Format::R8_UNORM, aw, ah));
        ao = graph.create_texture("ssao", color_desc(rhi::Format::R8_UNORM, aw, ah));
        auto ssao_pl = post_pipeline("ssao.frag", rhi::Format::R8_UNORM);
        if (!ssao_pl) return std::move(ssao_pl).error();
        auto blur_pl = post_pipeline("ssao_blur.frag", rhi::Format::R8_UNORM);
        if (!blur_pl) return std::move(blur_pl).error();
        SsaoUniforms su{};
        su.inv_proj = inverse(proj);
        su.params = {env_.ao_radius, env_.ao_intensity, 0.002f, static_cast<float>(S.ssao_samples)};
        su.screen = inv_size(rw, rh, 0.5f * static_cast<float>(rh) / std::tan(camera_.fov_y * 0.5f),
                             std::max(env_.ao_fade_distance, 1.0f));
        BlurUniforms bu{};
        bu.proj = {-proj[3].z, 1.0f, proj[2].z, 0.0f};
        bu.screen = inv_size(aw, ah);
        graph.add_pass(
            "ssao",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(ao_raw, rhi::LoadOp::DontCare);
            },
            [&, pl = ssao_pl.value(), su](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, su);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
        graph.add_pass(
            "ssao_blur",
            [&](PassBuilder& b) {
                b.read(ao_raw);
                b.read(depth);
                b.write_color(ao, rhi::LoadOp::DontCare);
            },
            [&, pl = blur_pl.value(), bu, ao_raw](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, bu);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(ao_raw), ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
    }

    graph.add_pass(
        "forward",
        [&](PassBuilder& b) {
            if (shadow_map.valid()) b.read(shadow_map);
            if (ao.valid()) b.read(ao);
            b.write_color(hdr, rhi::LoadOp::Clear, env_.clear_color);
            b.write_depth(depth, prepass ? rhi::LoadOp::Load : rhi::LoadOp::Clear, 1.0f);
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
                const bool has_env = env_map && env_map->cube();
                if (has_env) {
                    const float blur = std::isfinite(env_.sky_blur) ? std::clamp(env_.sky_blur, 0.0f, 1.0f) : 0.0f;
                    sky.env = {env_.environment_sun_disc ? 2.0f : 1.0f, blur * static_cast<float>(env_map->mip_count() - 1), env_intensity, env_rotation};
                }
                pass.bind_pipeline(sky_pipeline_);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, sky);
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, has_env ? env_map->cube() : ctx_.black_cube(), ctx_.linear_clamp());
                pass.draw(3);
            }
            const rhi::TextureHandle shadow_tex = shadow_map.valid() ? ctx.texture(shadow_map) : no_shadow_;
            const rhi::TextureHandle ao_tex = ao.valid() ? ctx.texture(ao) : ctx_.white();
            FrameUniforms frame{view_proj};
            ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
            ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, lighting);
            rhi::PipelineHandle bound{};
            auto draw_batch = [&](const Batch& b, rhi::PipelineHandle want) {
                const Draw* d = b.first;
                if (!(bound == want)) {
                    pass.bind_pipeline(want);
                    if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                    bound = want;
                }
                if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                const Material& m = d->material;
                MaterialUniforms mat{};
                mat.base_color = m.base_color.vec();
                mat.emissive = rgb(m.emissive, m.emissive_strength);
                mat.params = {m.metallic, m.roughness, m.normal_scale, m.alpha_cutoff};
                mat.flags = {m.normal_texture ? 1.0f : 0.0f, m.unlit ? 1.0f : 0.0f, m.vertex_alpha_coverage_mask ? 1.0f : 0.0f,
                             std::clamp(m.coverage_response, 0.0f, 1.0f)};
                mat.uv_transform = {m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y};
                for (std::size_t i = 0; i < 4; ++i) mat.custom[i] = m.shader_params[i];
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 1, mat);
                auto pick = [](rhi::TextureHandle t, rhi::TextureHandle fallback) { return t ? t : fallback; };
                const rhi::SamplerHandle tex_sampler = m.nearest_filter ? ctx_.nearest_repeat() : ctx_.linear_repeat();
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, pick(m.base_color_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 1, pick(m.metallic_roughness_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 2, pick(m.normal_texture, ctx_.flat_normal()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 3, pick(m.emissive_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 4, shadow_tex, ctx_.shadow_compare());
                pass.bind_texture(rhi::ShaderStage::Fragment, 5, ao_tex, ctx_.linear_clamp());
                ProbeUniforms pu{};
                rhi::TextureHandle env_cube = env_map && env_map->cube() ? env_map->cube() : ctx_.black_cube();
                if (d->probe >= 0) {
                    const ReflectionProbe& p = probes_[static_cast<std::size_t>(d->probe)];
                    const float pi = std::isfinite(p.intensity) ? std::max(p.intensity, 0.0f) : 1.0f;
                    pu.box_min = Vec4(p.box.min, 1.0f);
                    pu.box_max = Vec4(p.box.max, static_cast<float>(p.map->mip_count() - 1));
                    pu.position = Vec4(p.position, pi);
                    for (std::size_t i = 0; i < 9; ++i) pu.sh[i] = Vec4(p.map->sh().coefficients[i] * pi, 0.0f);
                    env_cube = p.map->cube();
                }
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 2, pu);
                pass.bind_texture(rhi::ShaderStage::Fragment, 6, env_cube, ctx_.linear_clamp());
                pass.bind_storage_buffer(rhi::ShaderStage::Fragment, 0, light_buffer_);
                pass.bind_storage_buffer(rhi::ShaderStage::Fragment, 1, cluster_buffer_);
                pass.bind_vertex_buffer(0, d->mesh->vertices);
                pass.bind_vertex_buffer(1, instance_buffer_, b.instance_offset * kInstanceStride);
                pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
            };
            for (const Batch& b : main_batches) {
                const Draw* d = b.first;
                rhi::PipelineHandle want = d->skinned ? (d->material.double_sided ? skin_double_ : skin_cull_)
                                                      : (d->material.double_sided ? opaque_double_ : opaque_cull_);
                if (d->material.surface_shader)
                    if (const auto custom = surface_pipeline(d->material.surface_shader, d->skinned, d->material.double_sided,
                                                             MaterialBlend::Opaque))
                        want = custom;
                draw_batch(b, want);
            }
            // Blended geometry after every opaque draw, far to near.
            for (const Batch& b : transparent_batches) {
                const Draw* d = b.first;
                rhi::PipelineHandle want = blend_pipeline(d->material.blend, d->skinned, d->material.double_sided);
                if (d->material.surface_shader)
                    if (const auto custom = surface_pipeline(d->material.surface_shader, d->skinned, d->material.double_sided,
                                                             d->material.blend))
                        want = custom;
                if (want) draw_batch(b, want);
            }
            if (!lines_.empty()) {
                pass.bind_pipeline(line_pipeline_);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj);
                pass.bind_vertex_buffer(0, line_buffer_);
                pass.draw(static_cast<std::uint32_t>(lines_.size()));
            }
        });

    if (!particle_draws.empty()) {
        ParticleFrameUniforms pf{};
        pf.view_proj = view_proj;
        const Mat4 cam_world = inverse(camera_.view);
        pf.right = Vec4(normalize(cam_world.transform_vector({1, 0, 0})), 0);
        pf.up = Vec4(normalize(cam_world.transform_vector({0, 1, 0})), 0);
        graph.add_pass(
            "particles",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
            },
            [&, pf, proj, rw, rh](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, pf);
                pass.bind_vertex_buffer(0, particle_buffer_);
                rhi::PipelineHandle bound{};
                for (const ParticleBatch& b : particle_draws) {
                    const rhi::PipelineHandle want = b.material.blend == ParticleBlend::Additive ? particle_additive_ : particle_alpha_;
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        bound = want;
                    }
                    ParticleUniforms pu{};
                    pu.proj = {proj[3].z, proj[2].z, 0, 0};
                    pu.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 1.0f / std::max(b.material.softness, 1e-3f), 0};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, pu);
                    pass.bind_texture(rhi::ShaderStage::Fragment, 0, b.material.texture ? b.material.texture : ctx_.soft_particle(),
                                      ctx_.linear_clamp());
                    pass.bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                    pass.bind_vertex_buffer(0, particle_buffer_, b.offset * static_cast<std::uint32_t>(sizeof(ParticleInstance)));
                    pass.draw(6, b.count);
                }
            });
    }

    // TAA resolve into the persistent history; everything after reads the resolved image.
    RGTexture scene_color = hdr;
    if (taa) {
        const std::uint32_t cur = history_index_ ^ 1u, prev = history_index_;
        VICTUS_TRY_STATUS(ensure_history(cur, rw, rh));
        if (!history_[prev]) VICTUS_TRY_STATUS(ensure_history(prev, rw, rh)); // steady texture count from frame one
        const bool have_prev = history_valid_ && history_[prev];
        auto taa_pl = post_pipeline("taa.frag", rhi::Format::RGBA16_FLOAT);
        if (!taa_pl) return std::move(taa_pl).error();
        TaaUniforms tu{};
        tu.reproject = prev_view_proj_ * inverse(view_proj_unjittered);
        tu.params = {S.taa_blend, have_prev ? 1.0f : 0.0f, 1.25f, 0.0f};
        tu.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh),
                     1.0f / static_cast<float>(have_prev ? history_w_[prev] : rw),
                     1.0f / static_cast<float>(have_prev ? history_h_[prev] : rh)};
        const RGTexture resolved = graph.import_texture("taa_resolved", history_[cur], color_desc(rhi::Format::RGBA16_FLOAT, rw, rh));
        RGTexture previous;
        if (have_prev)
            previous = graph.import_texture("taa_history", history_[prev],
                                            color_desc(rhi::Format::RGBA16_FLOAT, history_w_[prev], history_h_[prev]));
        graph.add_pass(
            "taa",
            [&](PassBuilder& b) {
                b.read(hdr);
                b.read(depth);
                if (previous.valid()) b.read(previous);
                b.write_color(resolved, rhi::LoadOp::DontCare);
            },
            [this, hdr, depth, previous, tu, pl = taa_pl.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, tu);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(hdr), ctx_.nearest_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1,
                                       previous.valid() ? ctx.texture(previous) : ctx.texture(hdr), ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 2, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
        scene_color = resolved;
    }

    // Dual-filter bloom: progressively halved mip chain, then tent upsampling back up.
    RGTexture bloom_result;
    if (bloom) {
        std::uint32_t levels = 0;
        std::vector<RGTexture> chain;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
        std::uint32_t bw = rw, bh = rh;
        while (levels < S.bloom_levels && bw >= 4 && bh >= 4) {
            bw /= 2;
            bh /= 2;
            chain.push_back(graph.create_texture(std::format("bloom_{}", levels), color_desc(rhi::Format::RGBA16_FLOAT, bw, bh)));
            sizes.emplace_back(bw, bh);
            ++levels;
        }
        if (levels > 0) {
            auto down_pl = post_pipeline("bloom_down.frag", rhi::Format::RGBA16_FLOAT);
            if (!down_pl) return std::move(down_pl).error();
            auto up_pl = post_pipeline("bloom_up.frag", rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Additive);
            if (!up_pl) return std::move(up_pl).error();
            for (std::uint32_t i = 0; i < levels; ++i) {
                const RGTexture src = i == 0 ? scene_color : chain[i - 1];
                const auto [sw, sh] = i == 0 ? std::pair{rw, rh} : sizes[i - 1];
                BloomDownUniforms du{inv_size(sw, sh, i == 0 ? 1.0f : 0.0f),
                                     {env_.bloom_threshold, env_.bloom_knee, 256.0f, 0.0f}};
                const RGTexture dst = chain[i];
                graph.add_pass(
                    std::format("bloom_down_{}", i),
                    [src, dst](PassBuilder& b) {
                        b.read(src);
                        b.write_color(dst, rhi::LoadOp::DontCare);
                    },
                    [this, src, du, pl = down_pl.value()](PassContext& ctx) {
                        ctx.pass->bind_pipeline(pl);
                        ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, du);
                        ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(src), ctx_.linear_clamp());
                        ctx.pass->draw(3);
                    });
            }
            for (std::uint32_t i = levels - 1; i-- > 0;) {
                const RGTexture src = chain[i + 1], dst = chain[i];
                const auto [sw, sh] = sizes[i + 1];
                PostUniforms up{inv_size(sw, sh, 1.0f, 1.0f), {}, {}};
                graph.add_pass(
                    std::format("bloom_up_{}", i),
                    [src, dst](PassBuilder& b) {
                        b.read(src);
                        b.write_color(dst, rhi::LoadOp::Load);
                    },
                    [this, src, up, pl = up_pl.value()](PassContext& ctx) {
                        ctx.pass->bind_pipeline(pl);
                        ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, up.params);
                        ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(src), ctx_.linear_clamp());
                        ctx.pass->draw(3);
                    });
            }
            bloom_result = chain[0];
        }
    }

    // Auto exposure: log-luminance image, then adaptation into a persistent 1x1 texture.
    RGTexture exposure_now;
    if (auto_exposure) {
        auto lum_pl = post_pipeline("luminance.frag", rhi::Format::R16_FLOAT);
        if (!lum_pl) return std::move(lum_pl).error();
        auto exp_pl = post_pipeline("exposure.frag", rhi::Format::R32_FLOAT);
        if (!exp_pl) return std::move(exp_pl).error();
        const RGTexture lum =
            graph.create_texture("log_luminance", color_desc(rhi::Format::R16_FLOAT, kLuminanceSize, kLuminanceSize));
        const rhi::TextureDesc one = color_desc(rhi::Format::R32_FLOAT, 1, 1);
        const RGTexture previous = graph.import_texture("exposure_previous", exposure_[exposure_index_], one);
        exposure_now = graph.import_texture("exposure_current", exposure_[exposure_index_ ^ 1u], one);
        const float dt = std::clamp(std::isfinite(frame_time_) ? frame_time_ : 0.0f, 0.0f, 1.0f);
        ExposureUniforms eu{};
        eu.params = {1.0f - std::exp(-dt * std::max(env_.adaptation_speed_up, 0.0f)),
                     1.0f - std::exp(-dt * std::max(env_.adaptation_speed_down, 0.0f)), env_.auto_exposure_min_ev,
                     std::max(env_.auto_exposure_min_ev, env_.auto_exposure_max_ev)};
        eu.flags = {adaptation_valid_ ? 0.0f : 1.0f, 0, 0, 0};
        PostUniforms lu{inv_size(kLuminanceSize, kLuminanceSize), {}, {}};
        graph.add_pass(
            "luminance",
            [&](PassBuilder& b) {
                b.read(scene_color);
                b.write_color(lum, rhi::LoadOp::DontCare);
            },
            [this, scene_color, lu, pl = lum_pl.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, lu.params);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(scene_color), ctx_.linear_clamp());
                ctx.pass->draw(3);
            });
        graph.add_pass(
            "exposure",
            [&](PassBuilder& b) {
                b.read(lum);
                b.read(previous);
                b.write_color(exposure_now, rhi::LoadOp::DontCare);
            },
            [this, lum, previous, eu, pl = exp_pl.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, eu);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(lum), ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(previous), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
    }

    // Tonemap (+ grading) into the target, or into an LDR buffer that FXAA resolves.
    const rhi::Format tonemap_format = fxaa ? rhi::Format::RGBA8_UNORM : target_format;
    auto tonemap = post_pipeline("tonemap.frag", tonemap_format);
    if (!tonemap) return std::move(tonemap).error();
    const RGTexture ldr = fxaa ? graph.create_texture("ldr_color", color_desc(rhi::Format::RGBA8_UNORM, width, height)) : out;
    PostUniforms post{};
    const Tonemapper op = env_.tonemap ? env_.tonemapper : Tonemapper::None;
    post.params = {env_.exposure, env_.vignette, is_srgb(target_format) ? 0.0f : 1.0f, static_cast<float>(op)};
    post.params2 = {bloom_result.valid() ? env_.bloom_intensity : 0.0f, exposure_now.valid() ? 1.0f : 0.0f,
                    std::exp2(env_.exposure_compensation), env_.saturation};
    post.params3 = {env_.contrast, fxaa ? 1.0f : 0.0f, linear_out ? 1.0f : 0.0f, 0.0f};
    if (capture_mode_) { // scene-referred radiance: the viewer's exposure applies later
        post.params.x = 1.0f;
        post.params2.y = 0.0f;
        post.params2.z = 1.0f;
    }
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& b) {
            b.read(scene_color);
            if (bloom_result.valid()) b.read(bloom_result);
            if (exposure_now.valid()) b.read(exposure_now);
            b.write_color(ldr, rhi::LoadOp::DontCare);
        },
        [&, tm = tonemap.value()](PassContext& ctx) {
            ctx.pass->bind_pipeline(tm);
            ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, post);
            ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(scene_color), ctx_.linear_clamp());
            ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1,
                                   bloom_result.valid() ? ctx.texture(bloom_result) : ctx_.black(), ctx_.linear_clamp());
            ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 2,
                                   exposure_now.valid() ? ctx.texture(exposure_now) : ctx_.white(), ctx_.nearest_clamp());
            ctx.pass->draw(3);
        });
    if (fxaa) {
        auto fx = post_pipeline("fxaa.frag", target_format);
        if (!fx) return std::move(fx).error();
        PostUniforms fu{inv_size(width, height, is_srgb(target_format) ? 1.0f : 0.0f, 0.75f), {}, {}};
        graph.add_pass(
            "fxaa",
            [&](PassBuilder& b) {
                b.read(ldr);
                b.write_color(out, rhi::LoadOp::DontCare);
            },
            [this, ldr, fu, pl = fx.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, fu.params);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(ldr), ctx_.linear_clamp());
                ctx.pass->draw(3);
            });
    }

    VICTUS_TRY_STATUS(graph.compile());
    VICTUS_TRY_STATUS(graph.execute(dev, cmd, pool_));
    graph_info_ = graph.info();
    if (taa) {
        history_index_ ^= 1u;
        history_valid_ = true;
        prev_view_proj_ = view_proj_unjittered;
        ++frame_index_;
    }
    if (auto_exposure) {
        exposure_index_ ^= 1u;
        adaptation_valid_ = true;
    } else {
        adaptation_valid_ = false;
    }
    return {};
}

} // namespace victus::render
