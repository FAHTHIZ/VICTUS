// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/render/builtin_shaders.hpp"
#include "victus/render/light_clusters.hpp"
#include "victus/render/temporal.hpp"

#include <algorithm>
#include <bit>
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
struct MotionUniforms {
    Mat4 view_proj, prev_view_proj;
};
// Per-instance data of the motion vector pass (vertex buffer slot 1): current model matrix columns
// (locations 4..7), previous model matrix as three affine rows (8..10) and, for skinned draws, the
// current / previous palette bases (13).
struct MotionInstance {
    Mat4 model;
    Vec4 prev_rows[3];
    Vec4 params;
};
static_assert(sizeof(MotionInstance) == 128);
// Per-instance vertex data (vertex buffer slot 1, locations 4..10): model matrix columns, then
// the inverse-transpose 3x3 normal matrix as three padded columns.
struct InstanceData {
    Mat4 model;
    Vec4 normal[3];
};
static_assert(sizeof(InstanceData) == 112);

// normal[0].w carries the first joint matrix of a skinned instance (exact as float up to 2^24);
// normal[1].w and normal[2].w the lightmap uv offset of a lightmapped one (ADR-0135).
InstanceData make_instance(const Mat4& model, std::uint32_t palette = 0, Vec2 lightmap_offset = {}) {
    InstanceData d;
    d.model = model;
    const Mat4 n = transpose(inverse(model));
    for (int i = 0; i < 3; ++i) d.normal[i] = Vec4(n[i].x, n[i].y, n[i].z, 0.0f);
    d.normal[0].w = static_cast<float>(palette);
    d.normal[1].w = lightmap_offset.x;
    d.normal[2].w = lightmap_offset.y;
    return d;
}

// Retained instance group lookup by id (slot + generation, ADR-0162); nullptr when stale.
template <class Groups> auto* find_group(Groups& groups, std::uint32_t id) noexcept {
    const std::uint32_t slot = (id & 0xFFFFu);
    using G = std::remove_reference_t<decltype(groups[0])>;
    if (slot == 0 || slot > groups.size()) return static_cast<G*>(nullptr);
    auto& g = groups[slot - 1];
    return g.live && g.generation == (id >> 16) ? &g : static_cast<G*>(nullptr);
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
                    m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y, m.coverage_response,
                    m.occlusion_strength})
        h = mix(h, bits(f));
    for (auto t : {m.base_color_texture, m.metallic_roughness_texture, m.normal_texture, m.emissive_texture})
        h = mix(h, handle_bits(t));
    if (m.surface_shader || m.vertex_stage) {
        h = mix(h, (std::uint64_t{m.surface_shader.generation} << 32) | m.surface_shader.index);
        h = mix(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m.vertex_stage)));
        for (const Vec4& p : m.shader_params)
            for (float f : {p.x, p.y, p.z, p.w}) h = mix(h, bits(f));
        for (std::size_t i = 0; i < m.shader_texture_count; ++i) h = mix(h, handle_bits(m.shader_textures[i]));
    }
    return mix(h, (m.double_sided ? 1u : 0u) | (m.unlit ? 2u : 0u) | (m.cast_shadows ? 4u : 0u) | (m.nearest_filter ? 8u : 0u) |
                      (m.vertex_alpha_coverage_mask ? 16u : 0u) | (m.shadow_only ? 32u : 0u) | (std::uint64_t{static_cast<std::uint8_t>(m.blend)} << 8));
}

bool same_material(const Material& a, const Material& b) noexcept {
    auto eq = [](const Color& x, const Color& y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; };
    return eq(a.base_color, b.base_color) && eq(a.emissive, b.emissive) && a.emissive_strength == b.emissive_strength &&
           a.metallic == b.metallic && a.roughness == b.roughness && a.normal_scale == b.normal_scale &&
           a.alpha_cutoff == b.alpha_cutoff && a.base_color_texture == b.base_color_texture &&
           a.metallic_roughness_texture == b.metallic_roughness_texture && a.normal_texture == b.normal_texture &&
           a.emissive_texture == b.emissive_texture && a.double_sided == b.double_sided && a.unlit == b.unlit &&
           a.cast_shadows == b.cast_shadows && a.uv_scale == b.uv_scale && a.uv_offset == b.uv_offset &&
           a.nearest_filter == b.nearest_filter && a.surface_shader == b.surface_shader && a.vertex_stage == b.vertex_stage &&
           a.shader_texture_count == b.shader_texture_count && a.shader_textures == b.shader_textures &&
           std::equal(a.shader_params.begin(), a.shader_params.end(), b.shader_params.begin(),
                      [](const Vec4& x, const Vec4& y) { return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w; }) &&
           a.vertex_alpha_coverage_mask == b.vertex_alpha_coverage_mask && a.coverage_response == b.coverage_response &&
           a.occlusion_strength == b.occlusion_strength && a.blend == b.blend && a.shadow_only == b.shadow_only;
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
    Vec4 local_shadow; // x atlas texel size, y tiles per row, z first shadow matrix index, w enabled
    Vec4 gi_min, gi_max, gi_counts; // irradiance volume (ADR-0056)
    Vec4 cloud_layer, cloud_shadow, cloud_wind; // cloud shadows (ADR-0112)
    Vec4 weather; // x surface wetness (ADR-0113), y rain occlusion on, z its depth bias (ADR-0114)
    Mat4 rain_vp; // world -> rain occlusion map (ADR-0114)
    Vec4 puddles; // x 1 / puddle size, y ripple strength, z 1 / ripple cell, w ripples per second (ADR-0117)
    Vec4 volumetric; // volumetric fog grid for blended draws: xyz froxels, w far, 0 off (ADR-0119)
};
struct MaterialUniforms {
    Vec4 base_color;
    Vec4 emissive;
    Vec4 params;
    Vec4 flags;
    Vec4 uv_transform; // xy scale, zw offset
    Vec4 custom[4];    // surface shader parameters
    Vec4 blend;        // x 0 opaque, 1 alpha, 2 additive (ADR-0119)
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
    Mat4 proj;
    Vec4 light;   // contact shadows: xyz towards the sun (view space), w ray length
    Vec4 contact; // x steps, y thickness, z enabled, w fade distance
};
struct BlurUniforms {
    Vec4 proj;
    Vec4 screen;
};
struct ParticleFrameUniforms {
    Mat4 view_proj;
    Vec4 right, up;
    Vec4 eye; // ribbon.vert only (particle.vert declares the first three members)
};
struct ParticleUniforms {
    Vec4 proj;   // view depth = proj.x / (depth + proj.y)
    Vec4 screen; // xy: 1 / render size, z: 1 / softness, w additive
    Vec4 fog_color, fog_params; // distance fog, as LightingUniforms (ADR-0116)
    Vec4 view;   // xy: 1 / projection x and y scale
    Vec4 volumetric; // volumetric fog grid: xyz froxels, w far, 0 off (ADR-0119)
};
struct PrecipitationUniforms {
    Mat4 view_proj;
    Vec4 eye;      // xyz camera, w wrapped time
    Vec4 velocity; // xyz m/s, w type (0 rain, 1 snow)
    Vec4 volume;   // x box size, y width, z streak seconds, w pixel size at 1 m
    Vec4 color;    // rgb lit colour, a opacity
    Vec4 right, up;
};
struct DecalFrameUniforms {
    Mat4 mvp;
};
struct DecalUniforms {
    Mat4 inv_view_proj;
    Mat4 world_to_decal;
    Vec4 color, uv, screen, axis;
};
struct WaterFrameUniforms {
    Mat4 view_proj;
    Vec4 center, extent;
    Vec4 wave_a[4], wave_b[4];
};
// reactive_mesh.frag Params (ADR-0187).
struct ReactiveMeshUniforms {
    Vec4 base_color;
    Vec4 uv_transform;
    Vec4 params; // x responsive, y alpha cutoff, z vertex alpha is a coverage mask, w additive
    Vec4 screen; // xy 1 / render size
};
struct WaterUniforms {
    Mat4 inv_view_proj;
    Vec4 camera, sun_dir, sun_color, shallow, deep, params, env, sky, horizon, ground, screen, fog_color, fog_params;
    Vec4 wave_a[4], wave_b[4];
};
static_assert(sizeof(WaterUniforms) == 64 + 21 * 16);
struct BloomDownUniforms {
    Vec4 params;
    Vec4 threshold;
};
struct ExposureUniforms {
    Vec4 params;
    Vec4 flags;
};
struct PostUniforms {
    Vec4 params, params2, params3, params4;
};
struct AlphaTestUniforms {
    Vec4 uv, params;
};
// ADR-0067: casters drawn with the alpha-tested shadow pipeline.
bool shadow_alpha_tested(const Material& m) noexcept { return m.alpha_cutoff > 0.0f && static_cast<bool>(m.base_color_texture); }
// Shadow batches merge draws of one mesh across materials; vertex functions must match too.
bool same_vertex_stage(const Material& a, const Material& b) noexcept {
    if (a.vertex_stage != b.vertex_stage) return false;
    if (!a.vertex_stage) return true;
    if (a.shader_texture_count != b.shader_texture_count || a.shader_textures != b.shader_textures) return false;
    return std::equal(a.shader_params.begin(), a.shader_params.end(), b.shader_params.begin(),
                      [](const Vec4& x, const Vec4& y) { return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w; });
}
bool same_alpha_test(const Material& a, const Material& b) noexcept {
    const bool ta = shadow_alpha_tested(a), tb = shadow_alpha_tested(b);
    if (ta != tb) return false;
    if (!ta) return true;
    return a.base_color_texture == b.base_color_texture && a.alpha_cutoff == b.alpha_cutoff && a.base_color.a == b.base_color.a &&
           a.uv_scale.x == b.uv_scale.x && a.uv_scale.y == b.uv_scale.y && a.uv_offset.x == b.uv_offset.x &&
           a.uv_offset.y == b.uv_offset.y && a.nearest_filter == b.nearest_filter;
}
struct TaaUniforms {
    Mat4 reproject;
    Vec4 params, screen;
    Vec4 quant; // x: compact HDR frame + 1 (0 off), ADR-0129
    // ADR-0147: x: disocclusion strength (0: off or no depth history), y, z: linear depth
    // 1 / (y * d + z) from window depth d, w: separation tolerance per unit depth.
    Vec4 depth_clip;
    // ADR-0148: x: thin-feature lock strength (0 off), y: lifetime lost per frame (1 / jitter
    // period), z: FSR2's luma similarity ratio.
    Vec4 lock;
};
struct TaaUpscaleUniforms {
    Mat4 reproject;
    Vec4 params, screen, jitter, output;
    Vec4 quant; // x: compact HDR frame + 1 (0 off), ADR-0129
    Vec4 depth_clip; // as TaaUniforms::depth_clip
    Vec4 lock;       // as TaaUniforms::lock
};
static_assert(sizeof(LightingUniforms) == 38 * 16 + 64 * 5);

constexpr std::uint32_t kLuminanceSize = 64;
// Fraction at the far end of each cascade's slice where pbr.frag dithers into the next cascade.
constexpr float kCascadeTransition = 0.1f;
// Receiver normal offset in shadow texels (pbr.frag sample_cascade).
constexpr float kShadowNormalBiasTexels = 1.0f;

Vec4 rgb(Color c, float scale = 1.0f, float w = 0.0f) { return {c.r * scale, c.g * scale, c.b * scale, w}; }

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

// Fall velocity (m/s) and box half size of the scene's rain or snow (ADR-0113).
Vec3 precipitation_velocity(const Precipitation& p) {
    const bool snow = p.type == PrecipitationType::Snow;
    const float speed = std::clamp(finite_or(p.fall_speed, 0.0f) > 0.0f ? p.fall_speed : (snow ? 1.2f : 9.0f), 0.05f, 100.0f);
    return Vec3{finite_or(p.wind.x, 0.0f), finite_or(p.wind.y, 0.0f) - speed, finite_or(p.wind.z, 0.0f)};
}
float precipitation_radius(const Precipitation& p) { return std::clamp(finite_or(p.radius, 15.0f), 2.0f, 100.0f); }
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
// With `previous` >= 0 (the level drawn last frame), that level is kept while it lies between this
// pick and the pick for the budget shrunk by `lod_hysteresis` (ADR-0077).
std::uint8_t select_lod(const GpuMesh& mesh, const Mat4& model, const Aabb& world, Vec3 eye, float pixels_per_unit,
                        const RenderSettings& s, int previous = -1, std::uint8_t* stateless = nullptr) {
    if (stateless) *stateless = 0;
    if (mesh.lod_count <= 1 || (!s.lods && s.force_lod < 0)) return 0;
    const auto last = static_cast<int>(mesh.lod_count) - 1;
    if (s.force_lod >= 0) {
        const auto forced = static_cast<std::uint8_t>(std::min(s.force_lod, last));
        if (stateless) *stateless = forced;
        return forced;
    }
    const Vec3 nearest{std::clamp(eye.x, world.min.x, world.max.x), std::clamp(eye.y, world.min.y, world.max.y),
                       std::clamp(eye.z, world.min.z, world.max.z)};
    const float distance = length(nearest - eye);
    if (!(distance > 1e-4f)) return 0;
    const float scale = std::max({length(model.c[0].xyz()), length(model.c[1].xyz()), length(model.c[2].xyz())});
    const float k = scale * pixels_per_unit / distance; // pixels per unit of mesh error
    const auto pick = [&](float budget) {
        int lod = 0;
        while (lod < last && mesh.lods[static_cast<std::size_t>(lod) + 1].error * k <= budget) ++lod;
        return lod;
    };
    const int coarsest = pick(s.lod_error_pixels);
    if (stateless) *stateless = static_cast<std::uint8_t>(coarsest);
    if (previous < 0 || s.lod_hysteresis <= 0.0f) return static_cast<std::uint8_t>(coarsest);
    const int finest = pick(s.lod_error_pixels * (1.0f - s.lod_hysteresis));
    return static_cast<std::uint8_t>(std::clamp(previous, finest, coarsest));
}

// Per-cascade shadow level (ADR-0132): the coarsest level whose error, scaled to world units,
// spans at most `budget_texels` texels of an orthographic cascade whose texels are `texel` world
// units wide (no distance term: the cascade's texel size is the same everywhere in it). Never
// finer than `finest` (the main-view level plus shadow_lod_bias).
std::uint8_t cascade_shadow_lod(const GpuMesh& mesh, const Mat4& model, float texel, float budget_texels,
                                std::uint8_t finest) {
    if (mesh.lod_count <= 1 || !(budget_texels > 0.0f) || !(texel > 0.0f)) return finest;
    const auto last = static_cast<int>(mesh.lod_count) - 1;
    const float scale = std::max({length(model.c[0].xyz()), length(model.c[1].xyz()), length(model.c[2].xyz())});
    const float budget = budget_texels * texel / std::max(scale, 1e-6f); // in mesh units
    int lod = finest;
    while (lod < last && mesh.lods[static_cast<std::size_t>(lod) + 1].error <= budget) ++lod;
    return static_cast<std::uint8_t>(lod);
}

void draw_mesh_lod(rhi::RenderPass& pass, const GpuMesh& mesh, std::uint32_t lod, std::uint32_t instances) {
    const GpuMeshLod& l = mesh.lods[std::min(lod, mesh.lod_count - 1)];
    pass.draw_indexed(l.index_count, instances, l.first_index);
}

} // namespace

struct SceneRenderer::FrameScratch {
    struct MotionBatch {
        const Draw* first;
        std::uint32_t instance_offset, instance_count;
    };
    std::vector<const Draw*> visible, transparent;
    std::array<std::vector<const Draw*>, 4> casters;
    std::vector<std::vector<const Draw*>> local_casters;
    std::vector<MotionInstance> motion_instances;
    std::vector<MotionBatch> motion_batches;
    std::vector<std::pair<const Draw*, MotionInstance>> moving;
    std::vector<InstanceData> instances;
    std::vector<Batch> main_batches, transparent_batches;
    std::array<std::vector<Batch>, 4> shadow_batches;
    std::vector<const Draw*> rain_casters;
    std::vector<Batch> rain_batches;
    std::vector<std::vector<Batch>> local_batches;
    std::vector<Vec4> occl_bounds;
    std::vector<rhi::IndexedIndirectCommand> occl_args;

    // Empties every list but keeps its storage; nested lists keep their own storage too.
    void begin(std::size_t local_views) {
        visible.clear();
        transparent.clear();
        for (auto& c : casters) c.clear();
        if (local_casters.size() < local_views) local_casters.resize(local_views);
        for (auto& c : local_casters) c.clear();
        motion_instances.clear();
        motion_batches.clear();
        moving.clear();
        instances.clear();
        main_batches.clear();
        transparent_batches.clear();
        for (auto& b : shadow_batches) b.clear();
        rain_casters.clear();
        rain_batches.clear();
        if (local_batches.size() < local_views) local_batches.resize(local_views);
        for (auto& b : local_batches) b.clear();
        occl_bounds.clear();
        occl_args.clear();
    }
};

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

std::string_view decal_blend_name(DecalBlend b) noexcept {
    switch (b) {
    case DecalBlend::Alpha: return "alpha";
    case DecalBlend::Additive: return "additive";
    default: return "multiply";
    }
}

std::optional<DecalBlend> parse_decal_blend(std::string_view name) noexcept {
    if (name == "multiply") return DecalBlend::Multiply;
    if (name == "alpha") return DecalBlend::Alpha;
    if (name == "additive") return DecalBlend::Additive;
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
        s.contact_shadows = false;
        s.bloom = false;
        s.bloom_levels = 4;
        s.fxaa = true;
        s.taa = false;
        s.max_point_lights = 64;
        s.lod_error_pixels = 2.0f;
        s.shadow_lod_bias = 1;
        s.foliage_lod_bias = 1;
        s.msaa_samples = 4;
        s.foliage_distance_scale = 0.7f;
        s.foliage_shadow_fraction = 0.5f;
        s.max_anisotropy = 2;
        s.local_shadows = false;
        s.irradiance_volumes = false;
        s.probe_sh_bands = 1;
        s.dynamic_gi = false;
        s.volumetric_fog = false;
        s.volumetric_clouds = false;
        s.max_decals = 128;
        s.max_water_surfaces = 4;
        s.max_particles = 16384;
        s.max_precipitation = 2048;
        s.precipitation_occlusion = 256;
        break;
    case QualityTier::Medium:
        s.render_scale = 1.0f;
        s.shadow_cascades = 2;
        s.shadow_resolution = 1024;
        s.soft_shadows = true;
        s.ssao = false;
        s.contact_shadows = false; // like SSAO: the depth prepass it needs is the main cost
        s.bloom = true;
        s.bloom_levels = 4;
        s.fxaa = true;
        s.taa = false;
        s.max_point_lights = 256;
        s.lod_error_pixels = 1.5f;
        s.foliage_distance_scale = 0.85f;
        s.foliage_shadow_fraction = 0.75f;
        s.max_anisotropy = 4;
        s.local_shadow_atlas = 1024;
        s.local_shadow_tile = 256;
        s.dynamic_gi = false;
        s.volumetric_fog_slices = 48;
        s.volumetric_fog_shadow_steps = 2;
        s.cloud_downsample = 4;
        s.cloud_steps = 32;
        s.cloud_light_steps = 4;
        s.max_decals = 256;
        s.max_water_surfaces = 8;
        s.max_particles = 65536;
        s.max_precipitation = 8192;
        s.precipitation_occlusion = 512;
        break;
    case QualityTier::High: break; // struct defaults
    case QualityTier::Ultra:
        s.shadow_cascades = 4;
        s.shadow_resolution = 2048;
        s.ssao_half_resolution = false;
        s.ssao_samples = 16;
        s.contact_shadow_steps = 16;
        s.bloom_levels = 6;
        s.max_point_lights = 1024;
        s.lod_error_pixels = 0.5f;
        s.max_anisotropy = 16;
        s.local_shadow_atlas = 4096;
        s.volumetric_fog_tile = 8;
        s.volumetric_fog_slices = 96;
        s.volumetric_fog_shadow_steps = 6;
        s.cloud_steps = 64;
        s.cloud_light_steps = 8;
        s.max_decals = SceneRenderer::kMaxDecals;
        s.max_particles = 1u << 20;
        s.max_precipitation = 32768;
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
    s.lod_fade_frames = std::min(s.lod_fade_frames, 64u);
    s.contact_shadow_steps = std::clamp(s.contact_shadow_steps, 4u, 32u);
    s.bloom_levels = std::clamp(s.bloom_levels, 1u, 8u);
    s.sharpen = std::isfinite(s.sharpen) ? std::clamp(s.sharpen, 0.0f, 1.0f) : 0.0f;
    s.upscale_mip_bias = std::isfinite(s.upscale_mip_bias) ? std::clamp(s.upscale_mip_bias, -2.0f, 0.0f) : 0.0f;
    s.taa_blend = std::isfinite(s.taa_blend) ? std::clamp(s.taa_blend, 0.04f, 1.0f) : 0.1f;
    s.reactive_max = std::isfinite(s.reactive_max) ? std::clamp(s.reactive_max, 0.0f, 1.0f) : 0.5f;
    s.taa_disocclusion = std::isfinite(s.taa_disocclusion) ? std::clamp(s.taa_disocclusion, 0.0f, 1.0f) : 1.0f;
    s.taa_thin_features = std::isfinite(s.taa_thin_features) ? std::clamp(s.taa_thin_features, 0.0f, 1.0f) : 1.0f;
    s.max_point_lights = std::min(s.max_point_lights, SceneRenderer::kMaxLights);
    s.lod_error_pixels = std::isfinite(s.lod_error_pixels) ? std::clamp(s.lod_error_pixels, 0.25f, 16.0f) : 1.0f;
    s.lod_hysteresis = std::isfinite(s.lod_hysteresis) ? std::clamp(s.lod_hysteresis, 0.0f, 0.9f) : 0.0f;
    s.force_lod = std::clamp(s.force_lod, -1, static_cast<int>(kMaxMeshLods) - 1);
    s.shadow_lod_bias = std::min(s.shadow_lod_bias, 3u);
    s.foliage_lod_bias = std::min(s.foliage_lod_bias, 3u);
    s.msaa_samples = s.msaa_samples >= 8 ? 8u : s.msaa_samples >= 4 ? 4u : s.msaa_samples >= 2 ? 2u : 1u;
    s.foliage_distance_scale = std::isfinite(s.foliage_distance_scale) ? std::clamp(s.foliage_distance_scale, 0.25f, 1.0f) : 1.0f;
    s.foliage_shadow_fraction = std::isfinite(s.foliage_shadow_fraction) ? std::clamp(s.foliage_shadow_fraction, 0.0f, 1.0f) : 1.0f;
    s.shadow_lod_texels = std::isfinite(s.shadow_lod_texels) ? std::clamp(s.shadow_lod_texels, 0.0f, 8.0f) : 1.0f;
    if (s.debug_view != DebugView::Overdraw) s.debug_view = DebugView::None;
    {
        std::uint32_t level = 1;
        while (level < 16 && level * 2 <= s.max_anisotropy) level *= 2;
        s.max_anisotropy = level;
    }
    s.local_shadow_atlas = std::clamp(s.local_shadow_atlas, 256u, 8192u);
    s.local_shadow_tile = std::clamp(s.local_shadow_tile, std::max(64u, s.local_shadow_atlas / 8u), s.local_shadow_atlas);
    s.volumetric_fog_tile = std::clamp(s.volumetric_fog_tile, 4u, 64u);
    s.volumetric_fog_slices = std::clamp(s.volumetric_fog_slices, 16u, 128u);
    s.volumetric_fog_shadow_steps = std::min(s.volumetric_fog_shadow_steps, 16u);
    s.cloud_downsample = std::clamp(s.cloud_downsample, 1u, 8u);
    s.cloud_steps = std::clamp(s.cloud_steps, 8u, 128u);
    s.cloud_light_steps = std::clamp(s.cloud_light_steps, 1u, 16u);
    s.max_decals = std::min(s.max_decals, SceneRenderer::kMaxDecals);
    s.max_water_surfaces = std::min(s.max_water_surfaces, static_cast<std::uint32_t>(kMaxWaterSurfaces));
    s.max_particles = std::min(s.max_particles, 1u << 24);
    s.max_precipitation = std::min(s.max_precipitation, SceneRenderer::kMaxPrecipitation);
    if (s.precipitation_occlusion > 0) s.precipitation_occlusion = std::clamp(s.precipitation_occlusion, 64u, 4096u);
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

Result<std::unique_ptr<SceneRenderer>> SceneRenderer::create(GpuContext& ctx, const SceneRendererOptions& options) {
    std::unique_ptr<SceneRenderer> r(new SceneRenderer(ctx));
    auto& dev = ctx.device();
    r->depth_format_ = ctx.sampled_depth_format();
    // Packed HDR (ADR-0050): half the colour bandwidth of RGBA16F where the GPU can render and blend into it.
    constexpr std::uint32_t kHdrUsage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    r->compact_hdr_requested_ = options.compact_hdr;
    if (options.compact_hdr && dev.supports_format(rhi::Format::R11G11B10_FLOAT, kHdrUsage)) r->hdr_format_ = rhi::Format::R11G11B10_FLOAT;
    // ADR-0147/0148 depth history (R: linear depth, G: thin-feature lock lifetime): RG16F, else
    // RGBA16F; RGBA8 (always renderable) keeps the resolve's second target valid where neither renders
    // (ES 3.0 without float colour buffers) and turns both tests off.
    r->depth_history_format_ = dev.supports_format(rhi::Format::RG16_FLOAT, kHdrUsage)     ? rhi::Format::RG16_FLOAT
                               : dev.supports_format(rhi::Format::RGBA16_FLOAT, kHdrUsage) ? rhi::Format::RGBA16_FLOAT
                                                                                          : rhi::Format::RGBA8_UNORM;
    auto get = [&](const char* n) { return ctx.shader(n); };
    auto mesh_vs = get("mesh.vert");
    // Half-precision shading (ADR-0137) only where the device enabled shaderFloat16.
    r->half_precision_requested_ = options.half_precision;
    r->half_precision_ = options.half_precision && dev.capabilities().shader_float16 && find_builtin_shader("pbr.frag#fp16");
    auto pbr_fs = get(r->half_precision_ ? "pbr.frag#fp16" : "pbr.frag");
    auto shadow_vs = get("shadow.vert");
    auto shadow_fs = get("shadow.frag");
    auto shadow_alpha_vs = get("shadow_alpha.vert");
    auto shadow_alpha_fs = get("shadow_alpha.frag");
    auto full_vs = get("fullscreen.vert");
    auto sky_fs = get("sky.frag");
    auto line_vs = get("line.vert");
    auto line_fs = get("line.frag");
    auto depth_vs = get("depth.vert");
    auto skin_vs = get("mesh_skinned.vert");
    auto lightmap_vs = get("mesh_lightmap.vert");
    auto skin_depth_vs = get("depth_skinned.vert");
    auto skin_shadow_vs = get("shadow_skinned.vert");
    auto particle_vs = get("particle.vert");
    auto particle_fs = get("particle.frag");
    auto ribbon_vs = get("ribbon.vert");
    auto decal_vs = get("decal.vert");
    auto decal_fs = get("decal.frag");
    auto water_vs = get("water.vert");
    auto water_fs = get("water.frag");
    for (auto* s : {&water_vs, &water_fs, &mesh_vs, &pbr_fs, &shadow_vs, &shadow_fs, &shadow_alpha_vs, &shadow_alpha_fs, &full_vs, &sky_fs, &line_vs, &line_fs, &depth_vs, &skin_vs, &lightmap_vs,
                    &skin_depth_vs, &skin_shadow_vs, &particle_vs, &particle_fs, &ribbon_vs, &decal_vs, &decal_fs})
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
    opaque.color_targets = {{r->hdr_format_, rhi::BlendMode::Opaque}};
    opaque.depth_format = r->depth_format_;
    auto p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_cull_ = p.value();
    r->opaque_desc_ = opaque;
    r->remember_forward(r->opaque_cull_, opaque);
    opaque.name = "pbr_double_sided";
    opaque.cull = rhi::CullMode::None;
    p = dev.create_pipeline(opaque);
    if (!p) return std::move(p).error();
    r->opaque_double_ = p.value();
    r->remember_forward(r->opaque_double_, opaque);
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
    r->remember_forward(r->skin_cull_, skinned);
    r->skinned_desc_ = skinned;
    skinned.name = "pbr_skinned_double_sided";
    skinned.cull = rhi::CullMode::None;
    p = dev.create_pipeline(skinned);
    if (!p) return std::move(p).error();
    r->skin_double_ = p.value();
    r->remember_forward(r->skin_double_, skinned);
    // Lightmapped static meshes (ADR-0122): the lightmap uv stream in slot 3.
    rhi::PipelineDesc lightmapped = opaque;
    lightmapped.name = "pbr_lightmapped";
    lightmapped.vertex = lightmap_vs.value();
    lightmapped.vertex_buffers = {mesh_layout, instance_layout, rhi::VertexBufferLayout{3, sizeof(Vec2), false}};
    lightmapped.attributes = with(opaque.attributes, {{14, 3, rhi::VertexFormat::Float2, 0}});
    lightmapped.cull = rhi::CullMode::Back;
    p = dev.create_pipeline(lightmapped);
    if (!p) return std::move(p).error();
    r->lightmap_cull_ = p.value();
    r->remember_forward(r->lightmap_cull_, lightmapped);
    r->lightmap_desc_ = lightmapped;
    lightmapped.name = "pbr_lightmapped_double_sided";
    lightmapped.cull = rhi::CullMode::None;
    p = dev.create_pipeline(lightmapped);
    if (!p) return std::move(p).error();
    r->lightmap_double_ = p.value();
    r->remember_forward(r->lightmap_double_, lightmapped);

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
    // DebugView::Overdraw: the pre-pass geometry path, counting into an RGBA8 target additively
    // with depth testing off.
    auto overdraw_fs = get("overdraw.frag");
    if (!overdraw_fs) return std::move(overdraw_fs).error();
    for (int skin = 0; skin < 2; ++skin)
        for (int dbl = 0; dbl < 2; ++dbl) {
            rhi::PipelineDesc od = skin ? skin_prepass : prepass;
            od.name = std::format("overdraw{}{}", skin ? "_skinned" : "", dbl ? "_double_sided" : "");
            od.fragment = overdraw_fs.value();
            od.cull = dbl ? rhi::CullMode::None : rhi::CullMode::Back;
            od.depth_test = od.depth_write = false;
            od.depth_format = rhi::Format::Invalid;
            od.color_targets = {{rhi::Format::RGBA8_UNORM, rhi::BlendMode::Additive}};
            p = dev.create_pipeline(od);
            if (!p) return std::move(p).error();
            r->overdraw_[skin][dbl] = p.value();
        }

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
    r->shadow_desc_ = shadow;
    r->pbr_fragment_ = pbr_fs.value();
    r->shadow_fragment_ = shadow_fs.value();
    r->shadow_alpha_fragment_ = shadow_alpha_fs.value();
    shadow.name = "shadow_depth_skinned";
    shadow.vertex = skin_shadow_vs.value();
    shadow.vertex_buffers = {mesh_layout, instance_layout, skin_layout};
    shadow.attributes = with(with(with({{0, 0, rhi::VertexFormat::Float3, 0}}, model_attrs),
                                  {{8, 1, rhi::VertexFormat::Float4, 64}}),
                             skin_attrs);
    p = dev.create_pipeline(shadow);
    if (!p) return std::move(p).error();
    r->skin_shadow_ = p.value();
    // ADR-0067: alpha-tested casters cut their holes into the shadow map too.
    rhi::PipelineDesc shadow_alpha = shadow;
    shadow_alpha.name = "shadow_depth_alpha_test";
    shadow_alpha.vertex = shadow_alpha_vs.value();
    shadow_alpha.fragment = shadow_alpha_fs.value();
    shadow_alpha.vertex_buffers = {mesh_layout, instance_layout};
    shadow_alpha.attributes = with({{0, 0, rhi::VertexFormat::Float3, 0}, {2, 0, rhi::VertexFormat::Float2, 24}}, model_attrs);
    p = dev.create_pipeline(shadow_alpha);
    if (!p) return std::move(p).error();
    r->shadow_alpha_ = p.value();

    rhi::PipelineDesc sky;
    sky.name = "sky";
    sky.vertex = full_vs.value();
    sky.fragment = sky_fs.value();
    sky.cull = rhi::CullMode::None;
    sky.color_targets = {{r->hdr_format_, rhi::BlendMode::Opaque}};
    sky.depth_format = r->depth_format_;
    p = dev.create_pipeline(sky);
    if (!p) return std::move(p).error();
    r->sky_pipeline_ = p.value();
    r->remember_forward(r->sky_pipeline_, sky);

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
    lines.color_targets = {{r->hdr_format_, rhi::BlendMode::Alpha}};
    lines.depth_format = r->depth_format_;
    p = dev.create_pipeline(lines);
    if (!p) return std::move(p).error();
    r->line_pipeline_ = p.value();
    r->remember_forward(r->line_pipeline_, lines);

    // Particles: instanced billboards, no depth attachment (the shader samples scene depth).
    for (const bool additive : {false, true}) {
        rhi::PipelineDesc pd;
        pd.name = additive ? "particles_additive" : "particles_alpha";
        pd.vertex = particle_vs.value();
        pd.fragment = particle_fs.value();
        pd.vertex_buffers = {{0, sizeof(ParticleInstance), true}};
        pd.attributes = {{0, 0, rhi::VertexFormat::Float4, 0}, {1, 0, rhi::VertexFormat::Float4, 16}, {2, 0, rhi::VertexFormat::Float4, 32}};
        pd.cull = rhi::CullMode::None;
        pd.color_targets = {{r->hdr_format_, additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        p = dev.create_pipeline(pd);
        if (!p) return std::move(p).error();
        (additive ? r->particle_additive_ : r->particle_alpha_) = p.value();
        // Ribbons (trails): the same 48-byte layout read per vertex as a triangle list.
        pd.name = additive ? "ribbons_additive" : "ribbons_alpha";
        pd.vertex = ribbon_vs.value();
        pd.vertex_buffers = {{0, sizeof(ParticleInstance), false}};
        p = dev.create_pipeline(pd);
        if (!p) return std::move(p).error();
        (additive ? r->ribbon_additive_ : r->ribbon_alpha_) = p.value();
    }

    // Decals: unit boxes from gl_VertexIndex, no depth attachment (the shader samples scene depth);
    // front faces culled so each pixel is shaded once, also from inside the box.
    for (const DecalBlend blend : {DecalBlend::Multiply, DecalBlend::Alpha, DecalBlend::Additive}) {
        rhi::PipelineDesc pd;
        pd.name = std::string("decal_") + std::string(decal_blend_name(blend));
        pd.vertex = decal_vs.value();
        pd.fragment = decal_fs.value();
        pd.cull = rhi::CullMode::Front;
        pd.color_targets = {{r->hdr_format_, blend == DecalBlend::Multiply ? rhi::BlendMode::Multiply
                                             : blend == DecalBlend::Additive ? rhi::BlendMode::Additive
                                                                             : rhi::BlendMode::Alpha}};
        p = dev.create_pipeline(pd);
        if (!p) return std::move(p).error();
        r->decal_pipelines_[static_cast<std::size_t>(blend)] = p.value();
    }

    // Water (ADR-0095): grid from gl_VertexIndex, both faces (visible from below), alpha-blended
    // soft edges, manual depth test in the shader; then a depth-only draw of the same surface.
    {
        rhi::PipelineDesc pd;
        pd.name = "water";
        pd.vertex = water_vs.value();
        pd.fragment = water_fs.value();
        pd.cull = rhi::CullMode::None;
        pd.color_targets = {{r->hdr_format_, rhi::BlendMode::Alpha}};
        p = dev.create_pipeline(pd);
        if (!p) return std::move(p).error();
        r->water_pipeline_ = p.value();
        rhi::PipelineDesc dd;
        dd.name = "water_depth";
        dd.vertex = water_vs.value();
        dd.fragment = shadow_fs.value();
        dd.cull = rhi::CullMode::None;
        dd.depth_test = dd.depth_write = true;
        dd.depth_compare = rhi::CompareOp::Less;
        dd.depth_format = r->depth_format_;
        p = dev.create_pipeline(dd);
        if (!p) return std::move(p).error();
        r->water_depth_pipeline_ = p.value();
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

const rhi::PipelineDesc& SceneRenderer::variant_desc(std::uint32_t variant) const noexcept {
    return variant == kVertexSkinned ? skinned_desc_ : variant == kVertexLightmapped ? lightmap_desc_ : opaque_desc_;
}

rhi::PipelineHandle SceneRenderer::blend_pipeline(MaterialBlend blend, std::uint32_t variant, bool double_sided) {
    if (blend == MaterialBlend::Opaque || variant >= kVertexVariants) return {};
    const std::size_t i = (blend == MaterialBlend::Additive ? kVertexVariants * 2u : 0u) + variant * 2u + (double_sided ? 1u : 0u);
    if (!blend_pipelines_[i]) {
        rhi::PipelineDesc d = variant_desc(variant);
        constexpr const char* kVariantNames[kVertexVariants] = {"", "_skinned", "_lightmapped"};
        d.name = std::format("pbr_{}{}{}", material_blend_name(blend), kVariantNames[variant], double_sided ? "_double_sided" : "");
        d.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        d.depth_write = false;
        d.color_targets = {{hdr_format_, blend == MaterialBlend::Additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        auto p = ctx_.device().create_pipeline(d);
        if (!p) {
            VICTUS_LOG_ERROR("render", "blend pipeline: {}", p.error().message);
            return {};
        }
        blend_pipelines_[i] = *p;
        remember_forward(*p, d);
    }
    return blend_pipelines_[i];
}

namespace {
std::uint64_t shader_bits(rhi::ShaderHandle h) noexcept { return (std::uint64_t{h.generation} << 32) | h.index; }
} // namespace

rhi::PipelineHandle SceneRenderer::surface_pipeline(rhi::ShaderHandle fragment, const SurfaceVertexStage* vertex, std::uint32_t variant,
                                                    bool double_sided, MaterialBlend blend) {
    if (variant >= kVertexVariants) return {};
    if (!fragment) fragment = pbr_fragment_; // vertex function with the built-in surface
    // Vertex variants: Static, Skinned, Lightmapped match kVertexStatic/Skinned/Lightmapped.
    const rhi::ShaderHandle vs = vertex ? vertex->shaders[variant] : rhi::ShaderHandle{};
    if (vertex && !vs) return {};
    std::uint64_t key = shader_bits(fragment);
    if (vertex) key ^= (shader_bits(vertex->shaders[0]) + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
    auto it = surface_pipelines_.find(key);
    if (it == surface_pipelines_.end()) {
        // Hot reload creates new shader handles; drop the oldest sets instead of growing forever.
        if (surface_pipelines_.size() >= 64) {
            for (auto& [k, set] : surface_pipelines_)
                for (auto pl : set)
                    if (pl) {
                        forget_forward(pl);
                        ctx_.device().destroy(pl);
                    }
            surface_pipelines_.clear();
        }
        it = surface_pipelines_.emplace(key, std::array<rhi::PipelineHandle, 4 * kVertexVariants * 2>{}).first;
    }
    // Variants are created on first use: [blend][variant][double_sided].
    const std::size_t i = static_cast<std::size_t>(blend) * kVertexVariants * 2u + variant * 2u + (double_sided ? 1u : 0u);
    if (i >= it->second.size()) return {};
    if (!it->second[i]) {
        if (surface_failed_.contains(key)) return {};
        rhi::PipelineDesc d = variant_desc(variant);
        d.name = std::format("surface_{}_{}", fragment.index, i);
        d.fragment = fragment;
        if (vs) d.vertex = vs;
        d.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        if (blend != MaterialBlend::Opaque) {
            d.depth_write = false;
            d.color_targets = {{hdr_format_, blend == MaterialBlend::Additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha}};
        }
        auto p = ctx_.device().create_pipeline(d);
        if (!p) {
            VICTUS_LOG_ERROR("render", "surface shader pipeline: {}", p.error().message);
            surface_failed_.insert(key);
            return {};
        }
        it->second[i] = *p;
        remember_forward(*p, d);
    }
    return it->second[i];
}

void SceneRenderer::push_vertex_material(rhi::CommandBuffer& cmd, const Material& m, bool shadow) const {
    struct VertexMaterialUniforms {
        Vec4 time;
        std::array<Vec4, 4> params;
    } u{Vec4(shader_time_, shadow ? 1.0f : 0.0f, 0.0f, 0.0f), m.shader_params};
    cmd.push_uniforms(rhi::ShaderStage::Vertex, 1, u);
}

rhi::PipelineHandle SceneRenderer::surface_shadow_pipeline(const SurfaceVertexStage& stage, bool skinned, bool cut) {
    const rhi::ShaderHandle vs = stage.get(skinned ? SurfaceVertexVariant::ShadowSkinned : SurfaceVertexVariant::Shadow);
    if (!vs) return {};
    const std::uint64_t key = shader_bits(stage.get(SurfaceVertexVariant::Shadow));
    auto it = surface_shadow_pipelines_.find(key);
    if (it == surface_shadow_pipelines_.end()) {
        if (surface_shadow_pipelines_.size() >= 64) {
            for (auto& [k, set] : surface_shadow_pipelines_)
                for (auto pl : set)
                    if (pl) ctx_.device().destroy(pl);
            surface_shadow_pipelines_.clear();
        }
        it = surface_shadow_pipelines_.emplace(key, std::array<rhi::PipelineHandle, 4>{}).first;
    }
    const std::size_t i = (skinned ? 2u : 0u) + (cut ? 1u : 0u);
    if (!it->second[i]) {
        if (surface_failed_.contains(key ^ 1ull)) return {};
        rhi::PipelineDesc d = shadow_desc_;
        const rhi::PipelineDesc& forward = variant_desc(skinned ? kVertexSkinned : kVertexStatic);
        d.name = std::format("surface_shadow_{}_{}", vs.index, i);
        d.vertex = vs;
        d.fragment = cut ? shadow_alpha_fragment_ : shadow_fragment_;
        d.vertex_buffers = forward.vertex_buffers;
        d.attributes = forward.attributes;
        auto p = ctx_.device().create_pipeline(d);
        if (!p) {
            VICTUS_LOG_ERROR("render", "surface shader shadow pipeline: {}", p.error().message);
            surface_failed_.insert(key ^ 1ull);
            return {};
        }
        it->second[i] = *p;
    }
    return it->second[i];
}

namespace {
std::uint64_t pipeline_key(rhi::PipelineHandle h) { return (std::uint64_t{h.generation} << 32) | h.index; }
constexpr std::uint64_t kAlphaToCoverageBit = std::uint64_t{1} << 63;
} // namespace

void SceneRenderer::remember_forward(rhi::PipelineHandle pipeline, const rhi::PipelineDesc& desc) {
    if (pipeline) forward_descs_[pipeline_key(pipeline)] = desc;
}

void SceneRenderer::forget_forward(rhi::PipelineHandle pipeline) {
    const std::uint64_t key = pipeline_key(pipeline);
    forward_descs_.erase(key);
    for (const std::uint64_t k : {key, key | kAlphaToCoverageBit}) {
        if (auto it = msaa_pipelines_.find(k); it != msaa_pipelines_.end()) {
            if (it->second) ctx_.device().destroy(it->second);
            msaa_pipelines_.erase(it);
        }
    }
}

rhi::PipelineHandle SceneRenderer::multisampled(rhi::PipelineHandle base, std::uint32_t samples, bool alpha_to_coverage) {
    if (!base) return {};
    if (samples != msaa_pipeline_samples_) {
        for (auto& [k, pl] : msaa_pipelines_)
            if (pl) ctx_.device().destroy(pl);
        msaa_pipelines_.clear();
        msaa_pipeline_samples_ = samples;
    }
    const std::uint64_t key = pipeline_key(base) | (alpha_to_coverage ? kAlphaToCoverageBit : 0u);
    if (auto it = msaa_pipelines_.find(key); it != msaa_pipelines_.end()) return it->second;
    rhi::PipelineHandle made{};
    if (auto d = forward_descs_.find(pipeline_key(base)); d != forward_descs_.end()) {
        rhi::PipelineDesc desc = d->second;
        desc.name += alpha_to_coverage ? std::format("_msaa{}_a2c", samples) : std::format("_msaa{}", samples);
        desc.sample_count = samples;
        desc.alpha_to_coverage = alpha_to_coverage;
        auto p = ctx_.device().create_pipeline(desc);
        if (p) made = *p;
        else VICTUS_LOG_ERROR("render", "multisampled pipeline: {}", p.error().message);
    }
    msaa_pipelines_.emplace(key, made); // failures are cached too (logged once)
    return made;
}

std::uint32_t SceneRenderer::supported_msaa(std::uint32_t samples) {
    while (samples > 1) {
        std::int8_t& known = msaa_support_[std::min<std::uint32_t>(samples, 8u)];
        if (known == 0) {
            const auto& dev = ctx_.device();
            known = dev.supports_samples(hdr_format_, samples) && dev.supports_samples(depth_format_, samples) ? 1 : -1;
        }
        if (known > 0) return samples;
        samples /= 2;
    }
    return 1;
}

SceneRenderer::SceneRenderer(GpuContext& ctx) : ctx_(ctx), pool_(ctx.device()) {}

SceneRenderer::~SceneRenderer() {
    auto& d = ctx_.device();
    for (auto pl : blend_pipelines_)
        if (pl) d.destroy(pl);
    for (auto pl : {opaque_cull_, opaque_double_, shadow_pipeline_, shadow_alpha_, sky_pipeline_, line_pipeline_, prepass_cull_,
                    prepass_double_, skin_cull_, skin_double_, lightmap_cull_, lightmap_double_, skin_prepass_cull_, skin_prepass_double_, skin_shadow_,
                    particle_alpha_, particle_additive_, ribbon_alpha_, ribbon_additive_, motion_cull_, motion_double_, skin_motion_cull_, skin_motion_double_})
        if (pl) d.destroy(pl);
    for (auto pl : decal_pipelines_)
        if (pl) d.destroy(pl);
    for (auto pl : {water_pipeline_, water_depth_pipeline_})
        if (pl) d.destroy(pl);
    for (const auto& row : overdraw_)
        for (auto pl : row)
            if (pl) d.destroy(pl);
    if (overdraw_tex_) d.destroy(overdraw_tex_);
    if (motion_buffer_) d.destroy(motion_buffer_);
    if (palette_buffer_) d.destroy(palette_buffer_);
    if (light_buffer_) d.destroy(light_buffer_);
    if (cluster_buffer_) d.destroy(cluster_buffer_);
    for (auto& [k, pl] : post_) d.destroy(pl);
    for (auto& [k, set] : surface_pipelines_)
        for (auto pl : set)
            if (pl) d.destroy(pl);
    for (auto& [k, set] : surface_shadow_pipelines_)
        for (auto pl : set)
            if (pl) d.destroy(pl);
    for (auto& [k, pl] : msaa_pipelines_)
        if (pl) d.destroy(pl);
    if (no_shadow_) d.destroy(no_shadow_);
    for (auto t : exposure_)
        if (t) d.destroy(t);
    for (auto t : history_)
        if (t) d.destroy(t);
    for (auto t : depth_history_)
        if (t) d.destroy(t);
    for (auto t : reactive_)
        if (t) d.destroy(t);
    if (line_buffer_) d.destroy(line_buffer_);
    if (instance_buffer_) d.destroy(instance_buffer_);
    for (auto& g : groups_)
        if (g.live && g.buffer) d.destroy(g.buffer);
    groups_.clear();
    if (particle_buffer_) d.destroy(particle_buffer_);
    for (auto& [key, st] : gpu_particle_states_) destroy_gpu_particle_state(st);
    gpu_particle_states_.clear();
    if (particle_sim_) d.destroy(particle_sim_);
    for (auto pl : {fog_inject_, fog_integrate_})
        if (pl) d.destroy(pl);
    for (auto b : {fog_froxels_[0], fog_froxels_[1], fog_integrated_})
        if (b) d.destroy(b);
    for (auto pl : {hiz_depth_, hiz_reduce_, occlusion_cull_})
        if (pl) d.destroy(pl);
    for (auto b : {hiz_buffer_, occl_bounds_, occl_args_, occl_instances_})
        if (b) d.destroy(b);
    pool_.clear();
}

bool SceneRenderer::ensure_occlusion_pipelines() {
    if (occlusion_cull_) return true;
    if (occlusion_failed_) return false;
    auto& d = ctx_.device();
    auto depth = create_builtin_compute_pipeline(d, "hiz_depth.comp");
    auto reduce = depth ? create_builtin_compute_pipeline(d, "hiz_reduce.comp") : Result<rhi::ComputePipelineHandle>(depth.error());
    auto cull = reduce ? create_builtin_compute_pipeline(d, "occlusion_cull.comp") : Result<rhi::ComputePipelineHandle>(reduce.error());
    if (!cull) {
        VICTUS_LOG_WARN("render", "GPU occlusion culling disabled: {}", cull.error().message);
        if (depth) d.destroy(depth.value());
        if (reduce) d.destroy(reduce.value());
        occlusion_failed_ = true;
        return false;
    }
    hiz_depth_ = depth.value();
    hiz_reduce_ = reduce.value();
    occlusion_cull_ = cull.value();
    return true;
}

bool SceneRenderer::ensure_fog_pipelines() {
    if (fog_integrate_) return true;
    if (fog_failed_) return false;
    auto& d = ctx_.device();
    auto inject = create_builtin_compute_pipeline(d, "fog_inject.comp");
    auto integrate = inject ? create_builtin_compute_pipeline(d, "fog_integrate.comp") : Result<rhi::ComputePipelineHandle>(inject.error());
    if (!integrate) {
        VICTUS_LOG_WARN("render", "volumetric fog disabled: {}", integrate.error().message);
        if (inject) d.destroy(inject.value());
        fog_failed_ = true;
        return false;
    }
    fog_inject_ = inject.value();
    fog_integrate_ = integrate.value();
    return true;
}

bool SceneRenderer::ensure_gpu_particle_pipeline() {
    if (particle_sim_) return true;
    if (particle_sim_failed_) return false;
    auto pl = create_builtin_compute_pipeline(ctx_.device(), "particle_sim.comp");
    if (!pl) {
        VICTUS_LOG_WARN("render", "GPU particles disabled: {}", pl.error().message);
        particle_sim_failed_ = true;
        return false;
    }
    particle_sim_ = pl.value();
    if (auto sort = create_builtin_compute_pipeline(ctx_.device(), "particle_sort.comp")) particle_sort_ = sort.value();
    else VICTUS_LOG_WARN("render", "GPU particle sorting disabled: {}", sort.error().message);
    return true;
}

void SceneRenderer::destroy_gpu_particle_state(GpuParticleState& s) {
    auto& d = ctx_.device();
    for (rhi::BufferHandle* b : {&s.state, &s.instances, &s.spawns, &s.pairs, &s.sorted, &s.trail, &s.ribbon, &s.ribbon_index})
        if (*b) {
            d.destroy(*b);
            *b = {};
        }
    s.capacity = s.spawn_capacity = s.sort_size = s.trail_points = 0;
    s.sorted_last = false;
}

std::uint32_t SceneRenderer::gpu_trail_points(std::uint32_t capacity, std::uint32_t requested) noexcept {
    if (requested == 0 || capacity == 0) return 0;
    const std::uint32_t fit = kMaxGpuTrailSegments / capacity;
    const std::uint32_t points = std::min(std::clamp<std::uint32_t>(requested, 2, 64), fit);
    return points >= 2 ? points : 0;
}

std::uint32_t SceneRenderer::gpu_particle_trail_points(std::uint64_t key) const {
    const auto it = gpu_particle_states_.find(key);
    return it == gpu_particle_states_.end() ? 0 : it->second.trail_points;
}

Result<std::vector<ParticleInstance>> SceneRenderer::read_gpu_particle_trails(std::uint64_t key) {
    std::vector<ParticleInstance> out;
    const auto it = gpu_particle_states_.find(key);
    if (it == gpu_particle_states_.end() || !it->second.ribbon || it->second.trail_points == 0) return out;
    const GpuParticleState& st = it->second;
    auto bytes = ctx_.device().read_buffer(st.ribbon, 0, st.capacity * (st.trail_points + 1) * 2 * static_cast<std::uint32_t>(sizeof(ParticleInstance)));
    if (!bytes) return std::move(bytes).error();
    out.resize(bytes.value().size() / sizeof(ParticleInstance));
    std::memcpy(out.data(), bytes.value().data(), out.size() * sizeof(ParticleInstance));
    return out;
}

void SceneRenderer::submit_gpu_particles(const GpuParticleEmitter& emitter) {
    if (emitter.capacity == 0 || emitter.key == 0) return;
    GpuParticleEmitter e = emitter;
    e.capacity = std::min(e.capacity, kMaxGpuParticles);
    gpu_emitters_.push_back(e);
}

Result<std::vector<ParticleInstance>> SceneRenderer::read_gpu_particle_draw(std::uint64_t key) {
    std::vector<ParticleInstance> out;
    const auto it = gpu_particle_states_.find(key);
    if (it == gpu_particle_states_.end() || !it->second.instances) return out;
    const GpuParticleState& st = it->second;
    auto bytes = ctx_.device().read_buffer(st.sorted_last ? st.sorted : st.instances, 0,
                                           st.capacity * static_cast<std::uint32_t>(sizeof(ParticleInstance)));
    if (!bytes) return std::move(bytes).error();
    out.resize(bytes.value().size() / sizeof(ParticleInstance));
    std::memcpy(out.data(), bytes.value().data(), out.size() * sizeof(ParticleInstance));
    return out;
}

Result<std::vector<GpuParticleSpawn>> SceneRenderer::read_gpu_particles(std::uint64_t key) {
    std::vector<GpuParticleSpawn> out;
    const auto it = gpu_particle_states_.find(key);
    if (it == gpu_particle_states_.end() || !it->second.state) return out;
    auto bytes = ctx_.device().read_buffer(it->second.state, 0, it->second.capacity * static_cast<std::uint32_t>(sizeof(GpuParticleSpawn)));
    if (!bytes) return std::move(bytes).error();
    out.resize(bytes.value().size() / sizeof(GpuParticleSpawn));
    std::memcpy(out.data(), bytes.value().data(), out.size() * sizeof(GpuParticleSpawn));
    return out;
}

bool SceneRenderer::ensure_motion_pipelines() {
    if (motion_cull_) return true;
    if (motion_failed_) return false;
    auto& d = ctx_.device();
    auto vs = ctx_.shader("motion.vert");
    auto skin_vs = vs ? ctx_.shader("motion_skinned.vert") : Result<rhi::ShaderHandle>(vs.error());
    auto fs = skin_vs ? ctx_.shader("motion.frag") : Result<rhi::ShaderHandle>(skin_vs.error());
    std::array<rhi::PipelineHandle, 4> made{};
    std::optional<Error> failure;
    if (!fs) failure = fs.error();
    if (!failure) {
        rhi::PipelineDesc desc;
        desc.vertex = vs.value();
        desc.fragment = fs.value();
        desc.vertex_buffers = {{0, sizeof(Vertex3D), false}, {1, sizeof(MotionInstance), true}};
        desc.attributes = {{0, 0, rhi::VertexFormat::Float3, 0}};
        for (std::uint32_t i = 0; i < 4; ++i) desc.attributes.push_back({4 + i, 1, rhi::VertexFormat::Float4, i * 16});
        for (std::uint32_t i = 0; i < 3; ++i) desc.attributes.push_back({8 + i, 1, rhi::VertexFormat::Float4, 64 + i * 16});
        // Test against the finished scene depth without writing it: only visible surfaces get motion.
        desc.depth_test = true;
        desc.depth_write = false;
        desc.depth_compare = rhi::CompareOp::LessEqual;
        desc.depth_format = depth_format_;
        desc.color_targets = {{rhi::Format::RG16_FLOAT, rhi::BlendMode::Opaque}};
        rhi::PipelineDesc skin = desc;
        skin.vertex = skin_vs.value();
        skin.vertex_buffers.push_back({2, sizeof(SkinVertex), false});
        skin.attributes.push_back({11, 2, rhi::VertexFormat::UShort4, 0});
        skin.attributes.push_back({12, 2, rhi::VertexFormat::Float4, 8});
        skin.attributes.push_back({13, 1, rhi::VertexFormat::Float4, 112});
        const rhi::PipelineDesc* descs[4] = {&desc, &desc, &skin, &skin};
        const char* names[4] = {"motion_vectors", "motion_vectors_double_sided", "motion_vectors_skinned",
                                "motion_vectors_skinned_double_sided"};
        for (int i = 0; i < 4 && !failure; ++i) {
            rhi::PipelineDesc pd = *descs[i];
            pd.name = names[i];
            pd.cull = (i & 1) ? rhi::CullMode::None : rhi::CullMode::Back;
            auto p = d.create_pipeline(pd);
            if (!p) failure = p.error();
            else made[i] = p.value();
        }
    }
    if (failure) {
        VICTUS_LOG_WARN("render", "object motion vectors disabled: {}", failure->message);
        for (auto p : made)
            if (p) d.destroy(p);
        motion_failed_ = true;
        return false;
    }
    motion_cull_ = made[0];
    motion_double_ = made[1];
    skin_motion_cull_ = made[2];
    skin_motion_double_ = made[3];
    return true;
}

Status SceneRenderer::ensure_buffer(rhi::BufferHandle& buffer, std::uint32_t& capacity, std::uint32_t bytes,
                                    std::uint32_t usage, const char* name) {
    if (buffer && bytes <= capacity) return {};
    auto& d = ctx_.device();
    if (buffer) d.destroy(buffer);
    buffer = {};
    capacity = std::max({bytes, capacity * 2, 4096u});
    auto b = d.create_buffer({name, capacity, usage});
    if (!b) {
        capacity = 0;
        return std::move(b).error();
    }
    buffer = b.value();
    return {};
}

Result<std::uint32_t> SceneRenderer::read_occlusion_survivors() {
    if (occl_batches_ == 0 || !occl_args_) return 0u;
    auto bytes = ctx_.device().read_buffer(occl_args_, 0, occl_batches_ * sizeof(rhi::IndexedIndirectCommand));
    if (!bytes) return std::move(bytes).error();
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < occl_batches_; ++i) {
        rhi::IndexedIndirectCommand c;
        std::memcpy(&c, bytes.value().data() + i * sizeof(c), sizeof(c));
        total += c.instance_count;
    }
    return total;
}

Result<OverdrawStats> SceneRenderer::read_overdraw() {
    OverdrawStats s;
    if (!overdraw_valid_ || !overdraw_tex_) return s;
    auto px = ctx_.device().read_texture(overdraw_tex_);
    if (!px) return std::move(px).error();
    s.width = overdraw_w_;
    s.height = overdraw_h_;
    const auto& bytes = px.value();
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        const std::uint32_t n = bytes[i];
        if (n == 0) continue;
        ++s.covered_pixels;
        s.layers += n;
        s.max_layers = std::max(s.max_layers, n);
    }
    return s;
}

Result<rhi::PipelineHandle> SceneRenderer::precipitation_pipeline(bool splash, bool reactive) {
    const std::string key = std::string(splash ? "precipitation_splash" : "precipitation") + (reactive ? "#reactive" : "");
    if (auto it = post_.find(key); it != post_.end()) return it->second;
    auto vs = ctx_.shader(splash ? "splash.vert" : "precipitation.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx_.shader(reactive ? "precipitation.frag#reactive" : "precipitation.frag");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc d;
    d.name = key;
    d.vertex = vs.value();
    d.fragment = fs.value();
    d.cull = rhi::CullMode::None;
    d.color_targets = {{hdr_format_, rhi::BlendMode::Alpha}};
    if (reactive) d.color_targets.push_back({rhi::Format::R8_UNORM, rhi::BlendMode::Alpha});
    auto p = ctx_.device().create_pipeline(d);
    if (!p) return p;
    post_.emplace(key, p.value());
    return p;
}

Result<rhi::PipelineHandle> SceneRenderer::reactive_pipeline(int kind, bool additive) {
    static constexpr const char* names[3][2] = {{"particles_alpha#reactive", "particles_additive#reactive"},
                                                {"ribbons_alpha#reactive", "ribbons_additive#reactive"},
                                                {"water#reactive", "water#reactive"}};
    const std::string key = names[kind][additive ? 1 : 0];
    if (auto it = post_.find(key); it != post_.end()) return it->second;
    auto vs = ctx_.shader(kind == 0 ? "particle.vert" : kind == 1 ? "ribbon.vert" : "water.vert");
    if (!vs) return std::move(vs).error();
    auto fs = ctx_.shader(kind == 2 ? "water.frag#reactive" : "particle.frag#reactive");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc d;
    d.name = key;
    d.vertex = vs.value();
    d.fragment = fs.value();
    d.cull = rhi::CullMode::None;
    if (kind < 2) {
        // Same layout as the plain particle and ribbon pipelines (create()).
        d.vertex_buffers = {{0, sizeof(ParticleInstance), kind == 0}};
        d.attributes = {{0, 0, rhi::VertexFormat::Float4, 0}, {1, 0, rhi::VertexFormat::Float4, 16}, {2, 0, rhi::VertexFormat::Float4, 32}};
    }
    // The mask blends like the colour: "over" accumulates coverage, additive adds brightness.
    const rhi::BlendMode blend = additive ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha;
    d.color_targets = {{hdr_format_, blend}, {rhi::Format::R8_UNORM, blend}};
    auto p = ctx_.device().create_pipeline(d);
    if (!p) return p;
    post_.emplace(key, p.value());
    return p;
}

Result<rhi::PipelineHandle> SceneRenderer::reactive_mesh_pipeline(std::uint32_t variant, bool double_sided) {
    if (variant >= kVertexVariants) return Error{ErrorCode::InvalidArgument, "reactive mesh pipeline: bad vertex variant"};
    std::string key = std::format("reactive_mesh/{}/{}", variant, double_sided ? 1 : 0);
    if (auto it = post_.find(key); it != post_.end()) return it->second;
    auto fs = ctx_.shader("reactive_mesh.frag");
    if (!fs) return std::move(fs).error();
    // The forward variant's vertex stage and layouts; one R8 target, no depth attachment (the
    // shader tests against the sampled opaque depth), single-sampled like the mask.
    rhi::PipelineDesc d = variant_desc(variant);
    d.name = key;
    d.fragment = fs.value();
    d.cull = double_sided ? rhi::CullMode::None : rhi::CullMode::Back;
    d.depth_test = false;
    d.depth_write = false;
    d.depth_format = rhi::Format::Invalid;
    d.sample_count = 1;
    d.alpha_to_coverage = false;
    d.color_targets = {{rhi::Format::R8_UNORM, rhi::BlendMode::Alpha}};
    auto p = ctx_.device().create_pipeline(d);
    if (!p) return p;
    post_.emplace(std::move(key), p.value());
    return p;
}

Result<rhi::PipelineHandle> SceneRenderer::post_pipeline(const char* fragment, rhi::Format format, rhi::BlendMode blend,
                                                         rhi::Format second) {
    std::string key = std::format("{}/{}/{}/{}", fragment, static_cast<int>(format), static_cast<int>(blend), static_cast<int>(second));
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
    if (second != rhi::Format::Invalid) d.color_targets.push_back({second, rhi::BlendMode::Opaque});
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
    rhi::TextureDesc d = color_desc(hdr_format_, width, height);
    d.name = index == 0 ? "taa_history_a" : "taa_history_b";
    auto t = ctx_.device().create_texture(d);
    if (!t) return std::move(t).error();
    history_[index] = t.value();
    history_w_[index] = width;
    history_h_[index] = height;
    if (depth_history_[index]) ctx_.device().destroy(depth_history_[index]);
    depth_history_[index] = {};
    rhi::TextureDesc dd = color_desc(depth_history_format_, width, height);
    dd.name = index == 0 ? "taa_depth_history_a" : "taa_depth_history_b";
    auto dt = ctx_.device().create_texture(dd);
    if (!dt) return std::move(dt).error();
    depth_history_[index] = dt.value();
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
    probe_requests_ = probe_unbaked_ = probe_over_budget_ = 0;
    volume_ = nullptr;
    lines_.clear();
    palette_.clear();
    particles_.clear();
    particle_batches_.clear();
    gpu_emitters_.clear();
    decals_.clear();
    waters_.clear();
    fog_volumes_.clear();
    stats_ = {};
    // Cached instance groups nobody asked for in the last 120 frames are released.
    ++group_frame_;
    for (auto it = group_cache_.begin(); it != group_cache_.end();) {
        const InstanceGroup* g = find_group(groups_, it->second);
        if (!g || g->last_used + 120 < group_frame_) {
            destroy_instance_group(it->second);
            it = group_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void SceneRenderer::add_point_light(const PointLight& light) { points_.push_back(light); }
void SceneRenderer::add_spot_light(const SpotLight& light) { spots_.push_back(light); }

void SceneRenderer::set_irradiance_volume(const IrradianceVolume* volume, float intensity, float normal_offset, bool baked) {
    volume_ = volume;
    volume_baked_ = volume && baked;
    volume_intensity_ = std::isfinite(intensity) ? std::max(intensity, 0.0f) : 1.0f;
    volume_normal_offset_ = std::isfinite(normal_offset) ? std::clamp(normal_offset, 0.0f, 4.0f) : 0.0f;
}

void SceneRenderer::add_reflection_probe(const ReflectionProbe& probe) {
    ++probe_requests_;
    if (!probe.map || !probe.map->cube() || !probe.box.valid()) {
        ++probe_unbaked_;
        return;
    }
    if (probes_.size() >= kMaxReflectionProbes) {
        ++probe_over_budget_;
        return;
    }
    probes_.push_back(probe);
}

void SceneRenderer::add_decal(const Decal& decal) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(decal.transform[c][r])) return;
    if (decal.color.a <= 0.0f || !std::isfinite(decal.color.a)) return;
    decals_.push_back(decal);
}

void SceneRenderer::add_fog_volume(const FogVolume& volume) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(volume.transform[c][r])) return;
    if (!(volume.density > 0.0f) || !std::isfinite(volume.density)) return;
    fog_volumes_.push_back(volume);
}

void SceneRenderer::add_water(const WaterSurface& water) {
    auto finite3 = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite3(water.center) || !(water.half_extent.x > 0) || !(water.half_extent.y > 0) ||
        !std::isfinite(water.half_extent.x * water.half_extent.y))
        return;
    WaterSurface w = water;
    for (WaterWave& wave : w.waves)
        if (!std::isfinite(wave.k) || !std::isfinite(wave.amplitude) || !std::isfinite(wave.q) || !std::isfinite(wave.phase) ||
            !std::isfinite(wave.direction.x) || !std::isfinite(wave.direction.y) || wave.k <= 0)
            wave = WaterWave{{1, 0}, 1, 0, 0, 0};
    w.resolution = std::clamp(w.resolution, 16u, 512u);
    w.clarity = std::isfinite(w.clarity) ? std::max(w.clarity, 0.05f) : 4.0f;
    w.foam = std::isfinite(w.foam) ? std::clamp(w.foam, 0.0f, 1.0f) : 0.0f;
    w.refraction = std::isfinite(w.refraction) ? std::clamp(w.refraction, 0.0f, 0.2f) : 0.0f;
    w.roughness = std::isfinite(w.roughness) ? std::clamp(w.roughness, 0.01f, 1.0f) : 0.06f;
    w.time = std::isfinite(w.time) ? w.time : 0.0f;
    waters_.push_back(w);
}

void SceneRenderer::submit(const GpuMesh& mesh, const Material& material, const Mat4& model, std::uint64_t motion_id,
                           Vec2 lightmap_uv_offset) {
    if (!mesh.valid()) return;
    ++stats_.submitted;
    Draw d{&mesh, material, model, 0.0f, mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh))};
    d.motion_id = motion_id;
    d.lightmap_offset = lightmap_uv_offset;
    draws_.push_back(d);
}

void SceneRenderer::submit_instances(const GpuMesh& mesh, const Material& material, std::span<const Mat4> models) {
    if (!mesh.valid() || models.empty()) return;
    stats_.submitted += static_cast<std::uint32_t>(models.size());
    const std::uint64_t key = mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh));
    draws_.reserve(draws_.size() + models.size());
    for (const Mat4& model : models) draws_.push_back(Draw{&mesh, material, model, 0.0f, key});
}

namespace {
// 10 bits per axis interleaved (z y x), for spatially coherent clusters.
std::uint32_t morton3(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    const auto spread = [](std::uint32_t v) {
        v &= 0x3FFu;
        v = (v | (v << 16)) & 0x030000FFu;
        v = (v | (v << 8)) & 0x0300F00Fu;
        v = (v | (v << 4)) & 0x030C30C3u;
        v = (v | (v << 2)) & 0x09249249u;
        return v;
    };
    return spread(x) | (spread(y) << 1) | (spread(z) << 2);
}
float max_axis_scale(const Mat4& m) noexcept {
    return std::max({length(m.c[0].xyz()), length(m.c[1].xyz()), length(m.c[2].xyz())});
}
} // namespace

Result<std::uint32_t> SceneRenderer::create_instance_group(const InstanceGroupDesc& desc) {
    if (!desc.mesh || !desc.mesh->valid()) return Error{ErrorCode::InvalidArgument, "instance group: invalid mesh"};
    if (desc.models.empty()) return Error{ErrorCode::InvalidArgument, "instance group: no instances"};
    if (!desc.lightmap_offsets.empty() && desc.lightmap_offsets.size() != desc.models.size())
        return Error{ErrorCode::InvalidArgument, "instance group: lightmap_offsets must be empty or one per model"};
    if (desc.models.size() > (1u << 24)) return Error{ErrorCode::InvalidArgument, "instance group: more than 2^24 instances"};
    for (const Mat4& m : desc.models)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m.c[c][r])) return Error{ErrorCode::InvalidArgument, "instance group: non-finite transform"};
    const auto n = static_cast<std::uint32_t>(desc.models.size());
    const std::uint32_t cluster_size = std::clamp(desc.cluster_size, 1u, 4096u);
    std::vector<Aabb> boxes(n);
    Aabb all = desc.mesh->bounds.transformed(desc.models[0]);
    for (std::uint32_t i = 0; i < n; ++i) {
        boxes[i] = desc.mesh->bounds.transformed(desc.models[i]);
        all.min = min(all.min, boxes[i].min);
        all.max = max(all.max, boxes[i].max);
    }
    const Vec3 extent = all.max - all.min;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order(n); // (morton code, index)
    for (std::uint32_t i = 0; i < n; ++i) {
        const Vec3 c = boxes[i].center();
        const auto q = [](float v, float lo, float e) {
            return e > 0.0f ? static_cast<std::uint32_t>(std::clamp((v - lo) / e, 0.0f, 1.0f) * 1023.0f) : 0u;
        };
        order[i] = {morton3(q(c.x, all.min.x, extent.x), q(c.y, all.min.y, extent.y), q(c.z, all.min.z, extent.z)), i};
    }
    std::sort(order.begin(), order.end()); // ties broken by index: deterministic
    InstanceGroup g;
    g.mesh = desc.mesh;
    g.material = desc.material;
    g.key = mix(material_key(desc.material), reinterpret_cast<std::uintptr_t>(desc.mesh));
    g.cluster_size = cluster_size;
    g.pending.resize(std::size_t{n} * sizeof(InstanceData));
    auto* data = reinterpret_cast<InstanceData*>(g.pending.data());
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t i = order[k].second;
        data[k] = make_instance(desc.models[i], 0, desc.lightmap_offsets.empty() ? Vec2{} : desc.lightmap_offsets[i]);
        if (k % cluster_size == 0) g.clusters.push_back({boxes[i], desc.models[i], k, 0});
        InstanceCluster& cl = g.clusters.back();
        cl.bounds.min = min(cl.bounds.min, boxes[i].min);
        cl.bounds.max = max(cl.bounds.max, boxes[i].max);
        if (max_axis_scale(desc.models[i]) > max_axis_scale(cl.representative)) cl.representative = desc.models[i];
        ++cl.count;
    }
    auto buffer = ctx_.device().create_buffer(
        {"instance_group", static_cast<std::uint32_t>(g.pending.size()), rhi::BufferUsageVertex});
    if (!buffer) return std::move(buffer).error();
    g.buffer = buffer.value();
    g.live = true;
    std::size_t slot = 0;
    while (slot < groups_.size() && groups_[slot].live) ++slot;
    if (slot >= 0xFFFFu) {
        ctx_.device().destroy(g.buffer);
        return Error{ErrorCode::LimitExceeded, "instance group: too many groups"};
    }
    if (slot == groups_.size()) groups_.emplace_back();
    g.generation = (groups_[slot].generation + 1u) & 0xFFFFu;
    if (g.generation == 0) g.generation = 1;
    groups_[slot] = std::move(g);
    return (groups_[slot].generation << 16) | static_cast<std::uint32_t>(slot + 1);
}

void SceneRenderer::destroy_instance_group(std::uint32_t id) {
    InstanceGroup* g = find_group(groups_, id);
    if (!g) return;
    if (g->buffer) ctx_.device().destroy(g->buffer);
    const std::uint32_t generation = g->generation;
    *g = InstanceGroup{};
    g->generation = generation; // the next group in this slot gets a new id
}

std::uint32_t SceneRenderer::instance_group_clusters(std::uint32_t id) const noexcept {
    const InstanceGroup* g = find_group(groups_, id);
    return g ? static_cast<std::uint32_t>(g->clusters.size()) : 0u;
}

std::uint32_t SceneRenderer::submit_instance_group(std::uint32_t id, const InstanceGroupDraw& opts) {
    InstanceGroup* g = find_group(groups_, id);
    if (!g) return 0;
    if (g->cached) g->last_used = group_frame_;
    const auto slot = static_cast<std::int32_t>((id & 0xFFFFu) - 1u);
    const float max_d = std::isnan(opts.max_distance) ? 0.0f : opts.max_distance;
    const float shadow_d = std::isnan(opts.shadow_distance) ? 0.0f : opts.shadow_distance;
    const bool limited = max_d < std::numeric_limits<float>::infinity() || shadow_d < std::numeric_limits<float>::infinity();
    const float max2 = max_d * max_d, shadow2 = shadow_d * shadow_d;
    const Vec3 eye = camera_.position;
    std::uint32_t submitted = 0;
    draws_.reserve(draws_.size() + g->clusters.size());
    for (const InstanceCluster& cl : g->clusters) {
        bool shadows = g->material.cast_shadows;
        if (limited) {
            const Vec3 nearest{std::clamp(eye.x, cl.bounds.min.x, cl.bounds.max.x),
                               std::clamp(eye.y, cl.bounds.min.y, cl.bounds.max.y),
                               std::clamp(eye.z, cl.bounds.min.z, cl.bounds.max.z)};
            const Vec3 v = nearest - eye;
            const float d2 = dot(v, v);
            if (!(max_d >= 0.0f) || d2 > max2) continue;
            if (!(shadow_d >= 0.0f) || d2 > shadow2) shadows = false;
        }
        Draw d{g->mesh, g->material, cl.representative, 0.0f, g->key};
        d.material.cast_shadows = shadows;
        d.group = slot;
        d.cluster_first = cl.first;
        d.cluster_count = cl.count;
        d.world_bounds = cl.bounds;
        draws_.push_back(d);
        submitted += cl.count;
        ++stats_.group_clusters;
    }
    stats_.submitted += submitted;
    return submitted;
}

std::uint32_t SceneRenderer::submit_instance_group(std::uint32_t id) { return submit_instance_group(id, InstanceGroupDraw{}); }

std::uint32_t SceneRenderer::cached_instance_group(std::uint64_t key, std::uint64_t revision, const GpuMesh& mesh,
                                                   const Material& material,
                                                   const std::function<void(std::vector<Mat4>&)>& build,
                                                   std::uint32_t cluster_size) {
    if (!mesh.valid()) return 0;
    const std::uint64_t material_hash = mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh));
    if (const auto it = group_cache_.find(key); it != group_cache_.end()) {
        InstanceGroup* g = find_group(groups_, it->second);
        if (g && g->revision == revision && g->key == material_hash && g->cluster_size == std::clamp(cluster_size, 1u, 4096u)) {
            g->last_used = group_frame_;
            return it->second;
        }
        destroy_instance_group(it->second);
        group_cache_.erase(it);
    }
    group_build_.clear();
    if (build) build(group_build_);
    if (group_build_.empty()) return 0;
    InstanceGroupDesc desc;
    desc.mesh = &mesh;
    desc.material = material;
    desc.models = group_build_;
    desc.cluster_size = cluster_size;
    auto id = create_instance_group(desc);
    group_build_.clear();
    if (!id) return 0;
    InstanceGroup* g = find_group(groups_, id.value());
    g->cached = true;
    g->cache_key = key;
    g->revision = revision;
    g->last_used = group_frame_;
    group_cache_.emplace(key, id.value());
    return id.value();
}

void SceneRenderer::submit_skinned(const GpuMesh& mesh, const Material& material, const Mat4& model,
                                   std::span<const Mat4> joints, const Aabb& world_bounds, std::uint64_t motion_id) {
    if (!mesh.skinned() || joints.empty()) {
        submit(mesh, material, model, motion_id);
        return;
    }
    if (!mesh.valid()) return;
    ++stats_.submitted;
    ++stats_.skinned_draws;
    Draw d{&mesh, material, model, 0.0f, mix(mix(material_key(material), reinterpret_cast<std::uintptr_t>(&mesh)), 0x5c1du)};
    d.palette = static_cast<std::uint32_t>(palette_.size());
    d.skinned = true;
    d.world_bounds = world_bounds;
    d.motion_id = motion_id;
    d.joint_count = static_cast<std::uint32_t>(joints.size());
    palette_.insert(palette_.end(), joints.begin(), joints.end());
    draws_.push_back(d);
}

void SceneRenderer::submit_ribbons(std::span<const RibbonPoint> points, std::span<const std::uint32_t> strips,
                                   const ParticleMaterial& material, const math::Aabb& bounds) {
    if (points.empty() || strips.empty() || !bounds.valid()) return;
    const auto first = static_cast<std::uint32_t>(particles_.size());
    // Without a texture the soft disc is sampled along its middle row: a soft edge across the
    // trail and no fade along it (colour and width carry that).
    const bool textured = static_cast<bool>(material.texture);
    std::size_t at = 0;
    for (const std::uint32_t n : strips) {
        if (at + n > points.size()) break;
        const RibbonPoint* s = points.data() + at;
        at += n;
        if (n < 2) continue;
        const auto tangent = [&](std::uint32_t k) {
            const Vec3 d = s[std::min(k + 1, n - 1)].position - s[k > 0 ? k - 1 : 0].position;
            const float len = length(d);
            return len > 1e-8f ? d / len : Vec3{0, 1, 0};
        };
        const auto edge = [&](std::uint32_t k, float sign) {
            const RibbonPoint& p = s[k];
            const float v = textured ? p.v : 0.5f;
            return ParticleInstance{Vec4(p.position, sign * 0.5f * std::max(0.0f, p.width)),
                                    Vec4(p.color.r, p.color.g, p.color.b, p.color.a), Vec4(tangent(k), v)};
        };
        for (std::uint32_t k = 0; k + 1 < n; ++k) {
            const ParticleInstance a0 = edge(k, -1), a1 = edge(k, 1), b0 = edge(k + 1, -1), b1 = edge(k + 1, 1);
            particles_.insert(particles_.end(), {a0, a1, b1, a0, b1, b0});
        }
    }
    const auto count = static_cast<std::uint32_t>(particles_.size()) - first;
    if (count == 0) return;
    ParticleBatch batch{material, first, count, bounds, 0.0f};
    batch.ribbon = true;
    particle_batches_.push_back(batch);
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
    float prev_near = n; // near depth of the previous cascade (its transition band feeds this one)
    cascade_light_view_ = light_view;
    cascade_sun_dir_ = d;
    for (std::uint32_t i = 0; i < 4; ++i) {
        cascade_vp_[i] = Mat4::identity();
        cascade_receivers_[i] = {};
    }
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
        // Receivers: view depths [prev - band * (prev - prev_near), split] (pbr.frag's dithered
        // transition samples this cascade from the last `band` of the previous slice), in light
        // space, widened by the 3x3 PCF footprint, the normal offset and a texel of snapping.
        {
            const float from = i == 0 ? n : std::max(n, prev - kCascadeTransition * (prev - prev_near));
            math::Aabb rec;
            for (float z : {from, split})
                for (float sy : {-1.0f, 1.0f})
                    for (float sx : {-1.0f, 1.0f}) {
                        const Vec3 corner = inv_view.transform_point({sx * tan_x * z, sy * tan_y * z, -z});
                        rec.expand(light_view.transform_point(corner));
                    }
            const float margin = texel * (kShadowNormalBiasTexels + 3.0f);
            rec.min = rec.min - Vec3{margin, margin, margin};
            rec.max = rec.max + Vec3{margin, margin, margin};
            cascade_receivers_[i] = rec;
            const Mat4 slice_proj = Mat4::perspective(camera_.fov_y, aspect, from, split);
            math::Frustum slice = math::Frustum::from_matrix(slice_proj * camera_.view);
            for (auto& p : slice.planes) p.w += margin;
            cascade_slices_[i] = slice;
        }
        prev_near = prev;
        prev = split;
    }
}

Status SceneRenderer::render(rhi::CommandBuffer& cmd, rhi::TextureHandle target, std::uint32_t width, std::uint32_t height,
                             rhi::Format target_format) {
    VICTUS_PROFILE_SCOPE("SceneRenderer::render");
    std::optional<profiler::ScopedZone> stage; // sequential CPU stages, each closes the previous
    stage.emplace("render.cpu.setup");
    if (width == 0 || height == 0) return {};
    auto& dev = ctx_.device();
    const RenderSettings& S = settings_;
    auto material_linear = ctx_.material_sampler(S.max_anisotropy);
    if (!material_linear) return std::move(material_linear).error();
    stats_.anisotropy = ctx_.effective_anisotropy(S.max_anisotropy);

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
    if (!taa) {
        history_valid_ = false;
        reactive_valid_[0] = reactive_valid_[1] = false;
    }
    // Temporal upscaling (ADR-0055): below output resolution the TAA resolve reconstructs and
    // accumulates at the output size, with a longer jitter cycle so every output pixel is visited.
    const bool upscale = taa && S.temporal_upscale && (rw < width || rh < height);
    // MSAA (ADR-0152) replaces TAA's anti-aliasing; never both.
    const std::uint32_t msaa = !taa && S.msaa_samples > 1 && S.debug_view == DebugView::None ? supported_msaa(S.msaa_samples) : 1u;
    stats_.msaa_samples = msaa;
    facts_.msaa_samples = msaa;
    facts_.alpha_to_coverage = msaa > 1 && S.alpha_to_coverage;
    const float upscale_ratio = std::max(static_cast<float>(width) / static_cast<float>(rw),
                                         static_cast<float>(height) / static_cast<float>(rh));
    // Compact HDR (ADR-0129): shaders writing the R11G11B10 scene colour round stochastically to
    // representable values; the frame number (+1, 0 = off) decorrelates the noise between frames.
    const float hdr_quant =
        hdr_format_ == rhi::Format::R11G11B10_FLOAT ? static_cast<float>(1u + frame_index_ % 64u) : 0.0f;
    const math::Vec2 jitter = taa ? taa_jitter(frame_index_, upscale ? temporal_jitter_period(upscale_ratio) : 8u) : math::Vec2{};
    const Mat4 proj = taa ? jitter_projection(proj_unjittered, jitter, rw, rh) : proj_unjittered;
    const Mat4 view_proj = proj * camera_.view;
    // ADR-0141: upscaled frames pick material mips for the output resolution.
    const float texture_mip_bias =
        upscale ? std::clamp(-std::log2(upscale_ratio) + S.upscale_mip_bias, -4.0f, 0.0f) : 0.0f;
    stats_.texture_mip_bias = texture_mip_bias;
    stats_.taa = taa;
    stats_.taa_history = taa && history_valid_;
    stats_.taa_disocclusion = false;
    stats_.taa_thin_features = false;
    stats_.temporal_upscale = upscale;
    stats_.reactive_mask = false;
    stats_.jitter = jitter;
    facts_ = {};
    facts_.render_width = rw;
    facts_.render_height = rh;
    facts_.output_width = width;
    facts_.output_height = height;
    facts_.capture_mode = capture_mode_;
    facts_.compute = dev.capabilities().compute;
    facts_.compact_hdr_requested = compact_hdr_requested_;
    facts_.compact_hdr = hdr_format_ == rhi::Format::R11G11B10_FLOAT;
    facts_.half_precision_requested = half_precision_requested_;
    facts_.half_precision = half_precision_;
    facts_.taa = taa;
    facts_.taa_history = stats_.taa_history;
    facts_.temporal_upscale = upscale;
    const bool shadows = sun_.shadows && sun_.intensity > 0.0f;
    const std::uint32_t shadow_res = std::max(64u, std::min(sun_.shadow_resolution, S.shadow_resolution));
    facts_.sun_shadows_requested = shadows;
    facts_.cascade_resolution = shadows ? shadow_res : 0;
    if (shadows) compute_cascades(aspect, shadow_res);
    else {
        cascade_count_ = 0;
        cascade_splits_ = {};
        for (auto& m : cascade_vp_) m = Mat4::identity();
    }
    stats_.shadow_cascades = cascade_count_;
    std::array<Frustum, 4> cascade_frustum{};
    for (std::uint32_t c = 0; c < cascade_count_; ++c) cascade_frustum[c] = Frustum::from_matrix(cascade_vp_[c]);

    stage.emplace("render.cpu.shadow_setup");
    // Rain occlusion (ADR-0114): a depth map of the opaque scene seen along the fall direction,
    // centred on the camera and snapped to whole texels, so drops and wetness stop under roofs.
    const Precipitation& weather = env_.precipitation;
    const std::uint32_t rain_res = S.precipitation_occlusion;
    const bool rain_occlusion = weather.type != PrecipitationType::None && finite_or(weather.intensity, 0.0f) > 0.0f &&
                                S.precipitation && S.max_precipitation > 0 && rain_res > 0 && !capture_mode_;
    Mat4 rain_vp = Mat4::identity();
    Frustum rain_frustum{};
    float rain_bias = 0.0f;
    if (rain_occlusion) {
        const Vec3 d = normalize(precipitation_velocity(weather));
        const Vec3 up = std::fabs(d.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Mat4 rain_view = Mat4::look_at({0, 0, 0}, d, up);
        const float half = std::ceil(precipitation_radius(weather) * 1.5f);
        const float texel = 2.0f * half / static_cast<float>(rain_res);
        Vec3 c = rain_view.transform_point(camera_.position);
        c.x = std::floor(c.x / texel) * texel;
        c.y = std::floor(c.y / texel) * texel;
        const float above = kRainOcclusionHeight, below = 2.0f * half + 10.0f;
        rain_vp = Mat4::orthographic(c.x - half, c.x + half, c.y - half, c.y + half, -c.z - above, -c.z + below) * rain_view;
        rain_frustum = Frustum::from_matrix(rain_vp);
        rain_bias = 0.1f / (above + below); // 10 cm in depth units
        stats_.precipitation_occlusion = rain_res;
    }

    // Local light shadows: rank shadow-casting lights by how much of the view they can cover and
    // hand out atlas tiles (spot 1, point 6) until the budget runs out.
    struct LocalShadowView {
        Mat4 view_proj; // rendering (tile viewport)
        Mat4 sample;    // world -> atlas uv and depth
        Frustum frustum;
    };
    std::vector<LocalShadowView> local_views;
    std::vector<std::pair<int, float>> point_shadow(points_.size(), {-1, 0.0f}), spot_shadow(spots_.size(), {-1, 0.0f});
    const std::uint32_t local_tiles_per_row = S.local_shadows ? std::min(S.local_shadow_atlas / S.local_shadow_tile, 8u) : 0u;
    {
        const std::uint32_t max_tiles = local_tiles_per_row * local_tiles_per_row;
        struct Candidate {
            float score;
            bool spot;
            std::uint32_t index;
        };
        std::vector<Candidate> candidates;
        const auto consider = [&](Vec3 pos, float range, float intensity, bool spot, std::uint32_t index) {
            if (!std::isfinite(range) || range <= 0.01f || !std::isfinite(intensity) || intensity <= 0.0f) return;
            if (!frustum.intersects(Aabb{pos - Vec3{range, range, range}, pos + Vec3{range, range, range}})) return;
            const float gap = std::max(length(pos - camera_.position) - range, 0.1f);
            candidates.push_back({range / gap, spot, index});
        };
        if (max_tiles > 0) {
            for (std::uint32_t i = 0; i < points_.size(); ++i)
                if (points_[i].cast_shadows) consider(points_[i].position, points_[i].range, points_[i].intensity, false, i);
            for (std::uint32_t i = 0; i < spots_.size(); ++i)
                if (spots_[i].cast_shadows && std::isfinite(spots_[i].outer_angle) && spots_[i].outer_angle <= 1.4835f)
                    consider(spots_[i].position, spots_[i].range, spots_[i].intensity, true, i);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        facts_.local_shadow_candidates = static_cast<std::uint32_t>(candidates.size());
        const float tile_scale = 1.0f / static_cast<float>(std::max(local_tiles_per_row, 1u));
        const auto add_view = [&](Vec3 eye, Vec3 dir, Vec3 up, float fov, float range) {
            const auto tile = static_cast<std::uint32_t>(local_views.size());
            const Mat4 vp = Mat4::perspective(fov, 1.0f, std::max(0.05f, range * 0.005f), range) * Mat4::look_at(eye, eye + dir, up);
            // NDC -> atlas uv of this tile (y down), depth unchanged.
            const float ox = static_cast<float>(tile % local_tiles_per_row) * tile_scale;
            const float oy = static_cast<float>(tile / local_tiles_per_row) * tile_scale;
            Mat4 bias = Mat4::identity();
            bias.c[0].x = 0.5f * tile_scale;
            bias.c[1].y = -0.5f * tile_scale;
            bias.c[3] = {ox + 0.5f * tile_scale, oy + 0.5f * tile_scale, 0.0f, 1.0f};
            local_views.push_back({vp, bias * vp, Frustum::from_matrix(vp)});
        };
        for (const Candidate& c : candidates) {
            const std::uint32_t need = c.spot ? 1u : 6u;
            if (local_views.size() + need > max_tiles) continue;
            const auto first = static_cast<int>(local_views.size());
            const float px = static_cast<float>(S.local_shadow_tile);
            if (c.spot) {
                const SpotLight& l = spots_[c.index];
                const Vec3 dir = length(l.direction) > 1e-6f ? normalize(l.direction) : Vec3{0, -1, 0};
                const Vec3 up = std::abs(dir.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
                const float outer = std::clamp(l.outer_angle, 0.01f, 1.4835f);
                add_view(l.position, dir, up, 2.0f * outer, l.range);
                spot_shadow[c.index] = {first, 2.0f * std::tan(outer) / px};
            } else {
                const PointLight& l = points_[c.index];
                const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int f = 0; f < 6; ++f) add_view(l.position, dirs[f], f == 2 || f == 3 ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, kPi * 0.5f, l.range);
                point_shadow[c.index] = {first, 2.0f / px};
            }
            ++stats_.local_shadow_lights;
        }
        stats_.local_shadow_tiles = static_cast<std::uint32_t>(local_views.size());
    }

    stage.emplace("render.cpu.visibility");
    // Visibility + sorting (front-to-back within pipeline and base texture runs).
    if (!scratch_) scratch_ = std::make_unique<FrameScratch>();
    FrameScratch& F = *scratch_;
    F.begin(local_views.size());
    auto& visible = F.visible;
    auto& transparent = F.transparent;
    auto& casters = F.casters;
    visible.reserve(draws_.size());
    // Pixels per unit of geometric error at distance 1 (perspective, render resolution).
    const float lod_pixels_per_unit = static_cast<float>(rh) / (2.0f * std::tan(std::max(camera_.fov_y, 1e-3f) * 0.5f));
    // LOD hysteresis history only carries over between consecutive frames of the same output size.
    // Frames with LODs off or forced record nothing, so they do not pin the next frame's levels.
    const bool lod_record = (S.lod_hysteresis > 0.0f || S.lod_fade_frames > 0) && S.lods && S.force_lod < 0;
    const bool lod_history = lod_record && lod_history_valid_ && lod_history_w_ == width && lod_history_h_ == height;
    lod_next_.clear(lod_record ? draws_.size() : 0);
    const auto lod_key = [](const Draw& d) { return mix(d.motion_id, reinterpret_cast<std::uintptr_t>(d.mesh)); };
    // Outgoing-level copies of cross-fading draws (ADR-0184); reserved so pointers stay valid.
    lod_fade_out_.clear();
    lod_fade_out_.reserve(S.lod_fade_frames > 0 ? draws_.size() : 0);
    const auto foliage_biased = [&](const Draw& d, std::uint8_t lod) {
        if (S.foliage_lod_bias > 0 && d.material.alpha_cutoff > 0.0f && S.force_lod < 0 && S.lods && d.mesh->lod_count > 1)
            return static_cast<std::uint8_t>(std::min<std::uint32_t>(lod + S.foliage_lod_bias, d.mesh->lod_count - 1u));
        return lod;
    };
    for (Draw& d : draws_) {
        const Aabb world = d.has_bounds() ? d.world_bounds : d.mesh->bounds.transformed(d.model);
        const bool blended = d.material.blend != MaterialBlend::Opaque;
        LodHistory::Entry previous;
        if (lod_history && d.motion_id != 0) previous = lod_prev_.entry(lod_key(d));
        std::uint8_t stateless = 0;
        d.lod = select_lod(*d.mesh, d.model, world, camera_.position, lod_pixels_per_unit, S, previous.lod, &stateless);
        if (previous.lod >= 0 && d.lod != stateless) ++stats_.lod_held;
        // Cross-fade: a level change starts a fade from the previous level; a fade in progress
        // advances one step per frame and ends after lod_fade_frames.
        std::uint8_t fade_from = 0, fade_step = 0;
        d.lod_fade = 0.0f;
        if (S.lod_fade_frames > 0 && previous.lod >= 0 && !blended && d.group < 0 && !d.material.shadow_only) {
            if (d.lod != previous.lod) {
                fade_from = static_cast<std::uint8_t>(previous.lod);
                fade_step = 1;
            } else if (previous.step > 0 && previous.step < S.lod_fade_frames) {
                fade_from = previous.from;
                fade_step = static_cast<std::uint8_t>(previous.step + 1);
            }
            if (fade_step > 0 && fade_from == d.lod) fade_step = 0;
        }
        if (lod_record && d.motion_id != 0) lod_next_.insert(lod_key(d), d.lod, fade_from, fade_step);
        // The history keeps the unbiased pick, so the hysteresis bands stay those of the budget.
        d.lod = foliage_biased(d, d.lod);
        std::uint8_t fade_lod = d.lod;
        if (fade_step > 0) {
            fade_lod = foliage_biased(d, fade_from);
            if (fade_lod != d.lod)
                d.lod_fade = static_cast<float>(fade_step) / static_cast<float>(S.lod_fade_frames + 1u);
        }
        d.shadow_lod = d.lod;
        if (S.force_lod < 0 && S.lods && d.mesh->lod_count > 1)
            d.shadow_lod = static_cast<std::uint8_t>(std::min<std::uint32_t>(d.lod + S.shadow_lod_bias, d.mesh->lod_count - 1u));
        if (d.material.cast_shadows && !blended) {
            bool any = false;
            bool have_light_box = false;
            math::Aabb light_box;
            for (std::uint32_t c = 0; c < cascade_count_; ++c) {
                if (!cascade_frustum[c].intersects(world)) continue;
                if (S.shadow_receiver_culling) {
                    if (!have_light_box) {
                        light_box = world.transformed(cascade_light_view_);
                        have_light_box = true;
                    }
                    // Its shadow falls along -Z in light view space: it can only darken receivers
                    // it overlaps in x/y and that lie no further upstream (+Z) than its top.
                    const math::Aabb& r = cascade_receivers_[c];
                    bool reaches = !(light_box.max.x < r.min.x || light_box.min.x > r.max.x ||
                                     light_box.max.y < r.min.y || light_box.min.y > r.max.y ||
                                     light_box.max.z < r.min.z);
                    if (reaches) {
                        // Shadow volume: the box swept along the sun by its height above the
                        // lowest receiver. Outside a slice plane, and not swept back in, is culled.
                        const float sweep = light_box.max.z - r.min.z;
                        const Vec3 bc = world.center(), be = world.extents();
                        for (const auto& p : cascade_slices_[c].planes) {
                            const float top = p.x * bc.x + p.y * bc.y + p.z * bc.z + p.w + be.x * std::fabs(p.x) +
                                              be.y * std::fabs(p.y) + be.z * std::fabs(p.z);
                            const Vec3& sd = cascade_sun_dir_;
                            const float toward = p.x * sd.x + p.y * sd.y + p.z * sd.z;
                            if (std::max(top, top + sweep * toward) < 0.0f) {
                                reaches = false;
                                break;
                            }
                        }
                    }
                    if (!reaches) {
                        ++stats_.shadow_receiver_culled;
                        continue;
                    }
                }
                d.cascade_lod[c] = d.shadow_lod;
                if (S.force_lod < 0 && S.lods)
                    d.cascade_lod[c] =
                        cascade_shadow_lod(*d.mesh, d.model, cascade_texel_[c], S.shadow_lod_texels, d.shadow_lod);
                casters[c].push_back(&d);
                any = true;
            }
            if (any) ++stats_.shadow_casters;
        }
        if (rain_occlusion && !blended && rain_frustum.intersects(world)) F.rain_casters.push_back(&d);
        if (d.material.shadow_only) continue; // casts, never drawn
        if (!frustum.intersects(world)) {
            ++stats_.culled;
            continue;
        }
        if (d.group >= 0) {
            ++stats_.group_clusters_visible;
            stats_.group_instances_drawn += d.cluster_count;
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
        const std::uint32_t tris = d.mesh->lods[d.lod].index_count / 3;
        stats_.triangles += tris;
        // Shading-cost counters (H2): kept apart from geometry so a fill/shading bottleneck is
        // visible as such.
        if (blended) stats_.transparent_triangles += tris;
        else if (d.material.alpha_cutoff > 0.0f) ++stats_.alpha_tested_draws;
        if (d.material.surface_shader) ++stats_.surface_shader_draws;
        (blended ? transparent : visible).push_back(&d);
        if (d.lod_fade > 0.0f) {
            // The outgoing level fills exactly the pixels the incoming one leaves out. It casts no
            // shadow (the shadow passes switch at once) and is not drawn in the depth pre-pass.
            Draw& out = lod_fade_out_.emplace_back(d);
            out.lod = fade_lod;
            out.lod_fade = -d.lod_fade;
            ++stats_.lod_fading;
            stats_.triangles += d.mesh->lods[out.lod].index_count / 3;
            visible.push_back(&out);
        }
    }
    std::swap(lod_prev_, lod_next_);
    lod_history_w_ = width;
    lod_history_h_ = height;
    lod_history_valid_ = true;
    std::stable_sort(transparent.begin(), transparent.end(), [](const Draw* a, const Draw* b) { return a->distance > b->distance; });
    auto& local_casters = F.local_casters; // at least local_views.size() lists, all empty
    if (!local_views.empty())
        for (const Draw& d : draws_) {
            if (!d.material.cast_shadows || d.material.blend != MaterialBlend::Opaque) continue;
            const Aabb world = d.has_bounds() ? d.world_bounds : d.mesh->bounds.transformed(d.model);
            for (std::size_t v = 0; v < local_views.size(); ++v)
                if (local_views[v].frustum.intersects(world)) local_casters[v].push_back(&d);
        }
    std::sort(visible.begin(), visible.end(), [](const Draw* a, const Draw* b) {
        // Retained-group clusters last: per-frame draws own instances [0, n) of the frame buffer
        // in order, which GPU occlusion culling relies on (ADR-0162).
        if ((a->group >= 0) != (b->group >= 0)) return b->group >= 0;
        if (a->material.surface_shader.index != b->material.surface_shader.index)
            return a->material.surface_shader.index < b->material.surface_shader.index;
        if (a->material.double_sided != b->material.double_sided) return !a->material.double_sided;
        if (a->material.base_color_texture.index != b->material.base_color_texture.index)
            return a->material.base_color_texture.index < b->material.base_color_texture.index;
        if (a->batch_key != b->batch_key) return a->batch_key < b->batch_key;
        if (a->lod != b->lod) return a->lod < b->lod;
        if (a->lod_fade != b->lod_fade) return a->lod_fade < b->lod_fade;
        if (a->probe != b->probe) return a->probe < b->probe;
        // Clusters of one group in buffer order, so neighbours merge into one draw.
        if (a->group >= 0 && a->group == b->group) return a->cluster_first < b->cluster_first;
        return a->distance < b->distance;
    });
    stats_.visible = static_cast<std::uint32_t>(visible.size());

    stage.emplace("render.cpu.motion");
    // Object motion vectors: visible opaque draws whose transform or joints changed since the last
    // frame. Previous joints are appended to this frame's palette so one storage buffer serves both.
    auto& motion_instances = F.motion_instances;
    auto& motion_batches = F.motion_batches;
    const bool track_motion = taa && S.motion_vectors;
    if (track_motion && history_valid_ && !motion_prev_.empty()) {
        auto& moving = F.moving;
        for (const Draw* d : visible) {
            // Alpha-tested draws would write motion through their holes (no discard here).
            // Vertex functions animate in the shader, which the motion pass does not run.
            if (d->motion_id == 0 || d->material.alpha_cutoff > 0.0f || d->material.vertex_stage) continue;
            const auto it = motion_prev_.find(d->motion_id);
            if (it == motion_prev_.end()) continue;
            const MotionState& prev = it->second;
            // Bitwise on purpose: any change, including -0/+0, counts as motion (Mat4 has no padding).
            const bool moved = std::memcmp(&prev.model, &d->model, sizeof(Mat4)) != 0; // NOLINT(bugprone-suspicious-memory-comparison)
            const bool same_skeleton = d->skinned && prev.joint_count == d->joint_count;
            const bool posed = same_skeleton && std::memcmp(motion_joints_.data() + prev.joint_offset, palette_.data() + d->palette,
                                                            d->joint_count * sizeof(Mat4)) != 0;
            if (!moved && !posed) continue;
            MotionInstance mi{};
            mi.model = d->model;
            for (int r = 0; r < 3; ++r) mi.prev_rows[r] = {prev.model.c[0][r], prev.model.c[1][r], prev.model.c[2][r], prev.model.c[3][r]};
            std::uint32_t prev_base = d->palette;
            if (posed) {
                prev_base = static_cast<std::uint32_t>(palette_.size());
                palette_.insert(palette_.end(), motion_joints_.begin() + prev.joint_offset,
                                motion_joints_.begin() + prev.joint_offset + prev.joint_count);
            }
            mi.params = {static_cast<float>(d->palette), static_cast<float>(prev_base), 0.0f, 0.0f};
            moving.emplace_back(d, mi);
        }
        if (!moving.empty() && ensure_motion_pipelines()) {
            std::stable_sort(moving.begin(), moving.end(), [](const auto& a, const auto& b) {
                if (a.first->skinned != b.first->skinned) return !a.first->skinned;
                if (a.first->material.double_sided != b.first->material.double_sided) return !a.first->material.double_sided;
                if (a.first->mesh != b.first->mesh) return a.first->mesh < b.first->mesh;
                return a.first->lod < b.first->lod;
            });
            for (const auto& [d, mi] : moving) {
                const auto offset = static_cast<std::uint32_t>(motion_instances.size());
                motion_instances.push_back(mi);
                if (!motion_batches.empty()) {
                    const Draw* f = motion_batches.back().first;
                    if (f->mesh == d->mesh && f->lod == d->lod && f->skinned == d->skinned &&
                        f->material.double_sided == d->material.double_sided) {
                        ++motion_batches.back().instance_count;
                        continue;
                    }
                }
                motion_batches.push_back({d, offset, 1});
            }
            stats_.motion_draws = static_cast<std::uint32_t>(motion_instances.size());
            stats_.motion_batches = static_cast<std::uint32_t>(motion_batches.size());
        }
    }
    // This frame's state becomes the next frame's "previous" once the frame is recorded.
    motion_next_.clear();
    motion_joints_next_.clear();
    if (track_motion)
        for (const Draw& d : draws_) {
            if (d.motion_id == 0) continue;
            MotionState st{d.model, 0, 0};
            if (d.skinned) {
                st.joint_offset = static_cast<std::uint32_t>(motion_joints_next_.size());
                st.joint_count = d.joint_count;
                motion_joints_next_.insert(motion_joints_next_.end(), palette_.begin() + d.palette,
                                           palette_.begin() + d.palette + d.joint_count);
            }
            motion_next_.emplace(d.motion_id, st); // the first submission of an id wins
        }
    if (!motion_instances.empty()) {
        const auto needed = static_cast<std::uint32_t>(motion_instances.size());
        if (needed > motion_capacity_) {
            if (motion_buffer_) dev.destroy(motion_buffer_);
            motion_buffer_ = {};
            motion_capacity_ = std::max({needed, motion_capacity_ * 2, 64u});
            auto b = dev.create_buffer({"motion_instances", motion_capacity_ * static_cast<std::uint32_t>(sizeof(MotionInstance)),
                                        rhi::BufferUsageVertex});
            if (!b) {
                motion_capacity_ = 0;
                return std::move(b).error();
            }
            motion_buffer_ = b.value();
        }
        cmd.upload_buffer(motion_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(motion_instances.data()),
                           motion_instances.size() * sizeof(MotionInstance)},
                          true);
    }

    stage.emplace("render.cpu.batches");
    // Instancing: consecutive draws with the same mesh and identical material become one batch.
    // Every pass reads its own contiguous range of the per-frame instance buffer.
    auto& instances = F.instances;
    instances.reserve(visible.size() + stats_.shadow_casters);
    // Main and transparent passes draw `lod`; shadow passes (match_material false) draw `shadow_lod`.
    // Directional cascade c (cascade >= 0) draws `cascade_lod[c]`.
    auto build_batches = [&](const std::vector<const Draw*>& list, bool match_material, std::vector<Batch>& out,
                             int cascade = -1) {
        for (const Draw* d : list) {
            const auto offset = static_cast<std::uint32_t>(instances.size());
            const std::uint8_t lod = match_material ? d->lod
                                     : cascade >= 0 ? d->cascade_lod[static_cast<std::size_t>(cascade)]
                                                    : d->shadow_lod;
            if (d->group >= 0) {
                // Retained cluster: its instances are already in the group buffer (ADR-0162).
                const rhi::BufferHandle buf = groups_[static_cast<std::size_t>(d->group)].buffer;
                if (!out.empty()) {
                    Batch& p = out.back();
                    if (p.buffer == buf && p.lod == lod && p.first->probe == d->probe &&
                        p.instance_offset + p.instance_count == d->cluster_first) {
                        p.instance_count += d->cluster_count;
                        continue;
                    }
                }
                out.push_back({d, d->cluster_first, d->cluster_count, lod, buf});
                continue;
            }
            instances.push_back(make_instance(d->model, d->palette, d->lightmap_offset));
            if (S.instancing && !out.empty()) {
                const Draw* f = out.back().first;
                if (f->mesh == d->mesh && out.back().lod == lod && f->skinned == d->skinned &&
                    (!match_material || (same_material(f->material, d->material) && f->probe == d->probe && f->lod_fade == d->lod_fade)) &&
                    (match_material || (same_alpha_test(f->material, d->material) && same_vertex_stage(f->material, d->material)))) {
                    ++out.back().instance_count;
                    continue;
                }
            }
            out.push_back({d, offset, 1, lod});
        }
    };
    const std::vector<Batch>& main_batches = F.main_batches;
    const std::vector<Batch>& transparent_batches = F.transparent_batches;
    build_batches(visible, true, F.main_batches);
    build_batches(transparent, true, F.transparent_batches);
    stats_.transparent_draws = static_cast<std::uint32_t>(transparent.size());
    auto& shadow_batches = F.shadow_batches;
    for (std::uint32_t c = 0; c < cascade_count_; ++c) {
        std::sort(casters[c].begin(), casters[c].end(), [c](const Draw* a, const Draw* b) {
            if (a->skinned != b->skinned) return !a->skinned;
            if (shadow_alpha_tested(a->material) != shadow_alpha_tested(b->material)) return !shadow_alpha_tested(a->material);
            if (a->mesh != b->mesh) return a->mesh < b->mesh;
            if (a->cascade_lod[c] != b->cascade_lod[c]) return a->cascade_lod[c] < b->cascade_lod[c];
            if (a->material.base_color_texture.index != b->material.base_color_texture.index)
                return a->material.base_color_texture.index < b->material.base_color_texture.index;
            if (a->group != b->group) return a->group < b->group;
            return a->cluster_first < b->cluster_first;
        });
        build_batches(casters[c], false, shadow_batches[c], static_cast<int>(c));
        stats_.shadow_batches += static_cast<std::uint32_t>(shadow_batches[c].size());
        for (const Batch& b : shadow_batches[c]) stats_.shadow_triangles += std::uint64_t{b.first->mesh->lods[b.lod].index_count / 3} * b.instance_count;
    }
    if (rain_occlusion) {
        std::sort(F.rain_casters.begin(), F.rain_casters.end(), [](const Draw* a, const Draw* b) {
            if (a->skinned != b->skinned) return !a->skinned;
            if (shadow_alpha_tested(a->material) != shadow_alpha_tested(b->material)) return !shadow_alpha_tested(a->material);
            if (a->mesh != b->mesh) return a->mesh < b->mesh;
            if (a->shadow_lod != b->shadow_lod) return a->shadow_lod < b->shadow_lod;
            if (a->group != b->group) return a->group < b->group;
            return a->cluster_first < b->cluster_first;
        });
        build_batches(F.rain_casters, false, F.rain_batches);
        stats_.precipitation_occluders = static_cast<std::uint32_t>(F.rain_casters.size());
    }
    auto& local_batches = F.local_batches; // at least local_views.size() lists, all empty
    for (std::size_t v = 0; v < local_views.size(); ++v) {
        std::sort(local_casters[v].begin(), local_casters[v].end(), [](const Draw* a, const Draw* b) {
            if (a->skinned != b->skinned) return !a->skinned;
            if (shadow_alpha_tested(a->material) != shadow_alpha_tested(b->material)) return !shadow_alpha_tested(a->material);
            if (a->mesh != b->mesh) return a->mesh < b->mesh;
            if (a->shadow_lod != b->shadow_lod) return a->shadow_lod < b->shadow_lod;
            if (a->material.base_color_texture.index != b->material.base_color_texture.index)
                return a->material.base_color_texture.index < b->material.base_color_texture.index;
            if (a->group != b->group) return a->group < b->group;
            return a->cluster_first < b->cluster_first;
        });
        build_batches(local_casters[v], false, local_batches[v]);
        for (const Batch& b : local_batches[v]) stats_.shadow_triangles += std::uint64_t{b.first->mesh->lods[b.lod].index_count / 3} * b.instance_count;
        stats_.local_shadow_draws += static_cast<std::uint32_t>(local_casters[v].size());
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
    for (InstanceGroup& g : groups_)
        if (g.live && !g.pending.empty()) {
            cmd.upload_buffer(g.buffer, 0, g.pending, false);
            g.pending.clear();
            g.pending.shrink_to_fit();
        }
    if (!instances.empty()) {
        const auto needed = static_cast<std::uint32_t>(instances.size());
        if (needed > instance_capacity_) {
            if (instance_buffer_) dev.destroy(instance_buffer_);
            // 1/8 headroom: per-cascade culling makes the count vary by a few instances between frames,
            // and growing to exactly the first frame's need would double the buffer for a few bytes.
            instance_capacity_ = std::max({needed + needed / 8, instance_capacity_ * 2, 256u});
            auto b = dev.create_buffer(
                {"scene_instances", instance_capacity_ * static_cast<std::uint32_t>(sizeof(InstanceData)),
                 rhi::BufferUsageVertex | rhi::BufferUsageComputeRead});
            if (!b) return std::move(b).error();
            instance_buffer_ = b.value();
        }
        cmd.upload_buffer(instance_buffer_, 0,
                          {reinterpret_cast<const std::uint8_t*>(instances.data()), instances.size() * sizeof(InstanceData)}, true);
    }
    constexpr auto kInstanceStride = static_cast<std::uint32_t>(sizeof(InstanceData));

    stage.emplace("render.cpu.occlusion_setup");
    // GPU occlusion culling setup: per-instance world bounds (tagged with batch index and the
    // batch's instance offset) and one indirect command per main batch with zero instances; the
    // cull shader appends survivors. Main batches own instances [0, visible.size()) in order.
    struct HiZLevel {
        std::int32_t offset, width, height, pad;
    };
    std::array<HiZLevel, 16> hiz_levels{};
    std::uint32_t hiz_level_count = 0, occl_instance_count = 0;
    const bool will_prepass = S.depth_prepass || (S.ssao && env_.ao_intensity > 0.0f && env_.ao_radius > 0.0f) ||
                              (S.contact_shadows && env_.contact_shadow_length > 0.0f && sun_.intensity > 0.0f);
    std::size_t occl_candidates = 0; // per-frame main batches; retained-group batches follow them
    while (occl_candidates < main_batches.size() && !main_batches[occl_candidates].buffer) ++occl_candidates;
    const bool occlusion = S.occlusion_culling && will_prepass && dev.capabilities().compute && occl_candidates > 0 &&
                           ensure_occlusion_pipelines();
    facts_.prepass = will_prepass;
    facts_.occlusion = occlusion;
    facts_.nothing_drawn = main_batches.empty();
    occl_batches_ = 0;
    if (occlusion) {
        std::int32_t w = static_cast<std::int32_t>((rw + 7) / 8), h = static_cast<std::int32_t>((rh + 7) / 8), offset = 0;
        for (;;) {
            hiz_levels[hiz_level_count++] = {offset, w, h, 0};
            offset += w * h;
            if ((w == 1 && h == 1) || hiz_level_count == hiz_levels.size()) break;
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        occl_instance_count = 0;
        for (std::size_t bi = 0; bi < occl_candidates; ++bi) occl_instance_count += main_batches[bi].instance_count;
        auto& bounds = F.occl_bounds;
        bounds.reserve(occl_instance_count * 2u);
        auto& args = F.occl_args;
        args.reserve(occl_candidates);
        for (std::size_t bi = 0; bi < occl_candidates; ++bi) {
            const Batch& b = main_batches[bi];
            const GpuMeshLod& l = b.first->mesh->lods[std::min<std::uint32_t>(b.lod, b.first->mesh->lod_count - 1)];
            args.push_back({l.index_count, 0, l.first_index, 0, 0});
            for (std::uint32_t k = 0; k < b.instance_count; ++k) {
                const Draw* d = visible[b.instance_offset + k];
                const Aabb world = d->skinned ? d->world_bounds : d->mesh->bounds.transformed(d->model);
                bounds.emplace_back(world.min, static_cast<float>(bi));
                bounds.emplace_back(world.max, static_cast<float>(b.instance_offset));
            }
        }
        const auto hiz_bytes = static_cast<std::uint32_t>(hiz_levels[hiz_level_count - 1].offset + 1) * 4u;
        VICTUS_TRY_STATUS(ensure_buffer(hiz_buffer_, hiz_capacity_, hiz_bytes, rhi::BufferUsageStorageWrite, "hiz_pyramid"));
        VICTUS_TRY_STATUS(ensure_buffer(occl_bounds_, occl_bounds_capacity_, static_cast<std::uint32_t>(bounds.size() * sizeof(Vec4)),
                                        rhi::BufferUsageComputeRead, "occlusion_bounds"));
        VICTUS_TRY_STATUS(ensure_buffer(occl_args_, occl_args_capacity_,
                                        static_cast<std::uint32_t>(args.size() * sizeof(rhi::IndexedIndirectCommand)),
                                        rhi::BufferUsageIndirect | rhi::BufferUsageStorageWrite, "occlusion_args"));
        VICTUS_TRY_STATUS(ensure_buffer(occl_instances_, occl_instances_capacity_, occl_instance_count * kInstanceStride,
                                        rhi::BufferUsageVertex | rhi::BufferUsageStorageWrite, "occlusion_instances"));
        cmd.upload_buffer(occl_bounds_, 0, {reinterpret_cast<const std::uint8_t*>(bounds.data()), bounds.size() * sizeof(Vec4)}, true);
        cmd.upload_buffer(occl_args_, 0,
                          {reinterpret_cast<const std::uint8_t*>(args.data()), args.size() * sizeof(rhi::IndexedIndirectCommand)},
                          true);
        occl_batches_ = static_cast<std::uint32_t>(occl_candidates);
        stats_.occlusion = true;
        stats_.occlusion_tested = occl_instance_count;
    }
    for (std::uint32_t c = 0; c < cascade_count_; ++c) stats_.shadow_draws += static_cast<std::uint32_t>(casters[c].size());

    stage.emplace("render.cpu.lights");
    // Local lights: frustum-cull bounding spheres, keep the nearest when over budget, then
    // assign them to view-space clusters and upload both storage buffers.
    std::vector<GpuLight> local_lights;
    local_lights.reserve(points_.size() + spots_.size());
    std::uint32_t spot_count = 0;
    {
        const auto finite_or = [](float v, float d) { return std::isfinite(v) ? v : d; };
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const PointLight& pl = points_[i];
            GpuLight g{};
            g.position_range = Vec4(pl.position, std::max(finite_or(pl.range, 0.01f), 0.01f));
            g.color_intensity = rgb(pl.color, 1.0f, std::max(finite_or(pl.intensity, 0.0f), 0.0f));
            if (point_shadow[i].first >= 0) g.spot = {0.0f, 0.0f, static_cast<float>(point_shadow[i].first + 1), point_shadow[i].second};
            local_lights.push_back(g);
        }
        for (std::size_t i = 0; i < spots_.size(); ++i) {
            const SpotLight& sl = spots_[i];
            GpuLight g{};
            const float outer = std::clamp(finite_or(sl.outer_angle, 0.5f), 0.001f, 3.1f);
            const float inner = std::clamp(finite_or(sl.inner_angle, 0.35f), 0.0f, outer);
            const Vec3 dir = length(sl.direction) > 1e-6f ? normalize(sl.direction) : Vec3{0, -1, 0};
            g.position_range = Vec4(sl.position, std::max(finite_or(sl.range, 0.01f), 0.01f));
            g.color_intensity = rgb(sl.color, 1.0f, std::max(finite_or(sl.intensity, 0.0f), 0.0f));
            g.direction_cos_outer = Vec4(dir, std::cos(outer));
            g.spot = {std::cos(inner), 1.0f, 0.0f, 0.0f};
            if (spot_shadow[i].first >= 0) {
                g.spot.z = static_cast<float>(spot_shadow[i].first + 1);
                g.spot.w = spot_shadow[i].second;
            }
            local_lights.push_back(g);
        }
        std::erase_if(local_lights, [&](const GpuLight& g) {
            const Vec4 b = light_bounding_sphere(g);
            return g.color_intensity.w <= 0.0f ||
                   !frustum.intersects(Aabb{{b.x - b.w, b.y - b.w, b.z - b.w}, {b.x + b.w, b.y + b.w, b.z + b.w}});
        });
        // Nearest first: the budget and the per-cluster cap then drop the farthest lights.
        const std::size_t budget = std::min<std::size_t>(S.max_point_lights, local_lights.size());
        facts_.local_lights_visible = static_cast<std::uint32_t>(local_lights.size());
        facts_.local_lights_kept = static_cast<std::uint32_t>(budget);
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
                // Also read by the volumetric fog compute pass (ADR-0098).
                auto b = dev.create_buffer({name, capacity, rhi::BufferUsageStorageRead | rhi::BufferUsageComputeRead});
                if (!b) return std::move(b).error();
                buf = b.value();
            }
            static const std::array<std::uint8_t, 64> zeros{};
            if (bytes == 0) cmd.upload_buffer(buf, 0, {zeros.data(), zeros.size()}, true);
            else cmd.upload_buffer(buf, 0, {static_cast<const std::uint8_t*>(data), bytes}, true);
            return {};
        };
        // Shadow sampling matrices follow the lights (same 64-byte stride), one per atlas tile.
        std::vector<GpuLight> light_data = local_lights;
        for (const LocalShadowView& v : local_views) {
            light_data.push_back({v.sample.c[0], v.sample.c[1], v.sample.c[2], v.sample.c[3]});
        }
        static_assert(sizeof(GpuLight) == sizeof(Mat4));
        VICTUS_TRY_STATUS(upload(light_buffer_, light_capacity_, "local_lights", light_data.data(),
                                 light_data.size() * sizeof(GpuLight)));
        VICTUS_TRY_STATUS(upload(cluster_buffer_, cluster_capacity_, "light_clusters", clusters.data.data(),
                                 clusters.data.size() * sizeof(std::uint32_t)));
    }

    stage.emplace("render.cpu.uploads");
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

    stage.emplace("render.cpu.gpu_particles");
    // GPU particles (ADR-0097): bring every submitted emitter's GPU state up to its clock. New
    // or reset emitters start from zeroed (dead) slots; spawn records the GPU has not seen yet
    // (and whose particle is still alive) are written into their slots, pre-aged to the clock.
    struct ParticleSimUniforms {
        Mat4 view_proj, inv_view_proj, world, view;
        Vec4 gravity_drag, turb, size, color_start, color_end, flip, collide, eye_dt, screen;
        std::array<std::uint32_t, 4> meta;
        Vec4 trail;
        std::array<std::uint32_t, 4> trail_meta;
    };
    static_assert(sizeof(ParticleSimUniforms) == 448);
    struct GpuParticleJob {
        const GpuParticleState* state;
        std::uint32_t spawns;
        ParticleSimUniforms u;
    };
    std::vector<GpuParticleJob> gpu_jobs;
    struct ParticleSortUniforms {
        Vec4 eye;
        std::array<std::uint32_t, 4> meta, step;
    };
    static_assert(sizeof(ParticleSortUniforms) == 48);
    struct GpuSortJob {
        const GpuParticleState* state;
        ParticleSortUniforms u;
    };
    std::vector<GpuSortJob> sort_jobs;
    std::vector<ParticleBatch> gpu_batches;
    ++gpu_particle_frame_;
    if (!gpu_emitters_.empty() && dev.capabilities().compute && ensure_gpu_particle_pipeline()) {
        std::vector<GpuParticleSpawn> spawns;
        for (const GpuParticleEmitter& e : gpu_emitters_) {
            GpuParticleState& st = gpu_particle_states_[e.key];
            st.last_frame = gpu_particle_frame_;
            const auto bytes = e.capacity * static_cast<std::uint32_t>(sizeof(GpuParticleSpawn));
            if (st.capacity != e.capacity) {
                destroy_gpu_particle_state(st);
                auto sb = dev.create_buffer({"gpu_particle_state", bytes, rhi::BufferUsageStorageWrite});
                if (!sb) return std::move(sb).error();
                st.state = sb.value();
                auto ib = dev.create_buffer({"gpu_particle_instances", e.capacity * static_cast<std::uint32_t>(sizeof(ParticleInstance)),
                                             rhi::BufferUsageStorageWrite | rhi::BufferUsageComputeRead | rhi::BufferUsageVertex});
                if (!ib) return std::move(ib).error();
                st.instances = ib.value();
                st.capacity = e.capacity;
                st.instance = e.instance + 1; // force the reset below
            }
            // Trails (ADR-0110): point rings and ribbon vertices sized for this capacity; emitters
            // without trails bind one-element buffers (the shader declares them either way).
            const std::uint32_t trail_points = gpu_trail_points(e.capacity, e.params.trail_points);
            if (e.params.trail_points > 0) {
                ++stats_.gpu_particle_trail_emitters;
                if (trail_points < std::clamp<std::uint32_t>(e.params.trail_points, 2, 64)) ++stats_.gpu_particle_trails_reduced;
            }
            if (!st.trail || st.trail_points != trail_points) {
                for (rhi::BufferHandle* h : {&st.trail, &st.ribbon, &st.ribbon_index})
                    if (*h) {
                        dev.destroy(*h);
                        *h = {};
                    }
                st.trail_points = trail_points;
                const std::uint32_t slots = trail_points > 0 ? e.capacity : 1;
                auto tb = dev.create_buffer({"gpu_particle_trail", slots * std::max(trail_points, 1u) * 16u, rhi::BufferUsageStorageWrite});
                if (!tb) return std::move(tb).error();
                st.trail = tb.value();
                const std::uint32_t ribbon_bytes = slots * (trail_points + 1) * 2 * static_cast<std::uint32_t>(sizeof(ParticleInstance));
                auto rb = dev.create_buffer({"gpu_particle_ribbon", ribbon_bytes, rhi::BufferUsageStorageWrite | rhi::BufferUsageVertex});
                if (!rb) return std::move(rb).error();
                st.ribbon = rb.value();
                const std::vector<std::uint8_t> zeros(ribbon_bytes, 0);
                cmd.upload_buffer(st.ribbon, 0, zeros, false);
                if (trail_points > 0) {
                    // Two triangles per segment between the edge pairs of consecutive points.
                    std::vector<std::uint32_t> idx;
                    idx.reserve(static_cast<std::size_t>(e.capacity) * trail_points * 6);
                    for (std::uint32_t slot = 0; slot < e.capacity; ++slot)
                        for (std::uint32_t k = 0; k < trail_points; ++k) {
                            const std::uint32_t a0 = (slot * (trail_points + 1) + k) * 2, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
                            idx.insert(idx.end(), {a0, a1, b1, a0, b1, b0});
                        }
                    auto ix = dev.create_buffer({"gpu_particle_ribbon_index", static_cast<std::uint32_t>(idx.size() * 4), rhi::BufferUsageIndex});
                    if (!ix) return std::move(ix).error();
                    st.ribbon_index = ix.value();
                    cmd.upload_buffer(st.ribbon_index, 0, {reinterpret_cast<const std::uint8_t*>(idx.data()), idx.size() * 4}, false);
                }
            }
            const bool fresh = st.instance != e.instance || st.generation != e.generation || e.time < st.time;
            if (fresh && !capture_mode_) {
                const std::vector<std::uint8_t> zeros(bytes, 0);
                cmd.upload_buffer(st.state, 0, zeros, false);
                cmd.upload_buffer(st.instances, 0, {zeros.data(), e.capacity * sizeof(ParticleInstance)}, false);
                if (st.trail_points > 0) {
                    const std::vector<std::uint8_t> ribbon_zeros(static_cast<std::size_t>(e.capacity) * (st.trail_points + 1) * 2 * sizeof(ParticleInstance), 0);
                    cmd.upload_buffer(st.ribbon, 0, ribbon_zeros, false);
                }
                st.instance = e.instance;
                st.generation = e.generation;
                st.time = e.time;
                st.next_index = e.records.empty() ? e.spawned : e.records.front().index;
            }
            if (!capture_mode_ && e.alive == 0 && !fresh) {
                // Nothing alive (on the CPU's books): skip the dispatches but keep the clock.
                st.time = std::max(st.time, e.time);
                st.next_index = e.spawned;
            } else if (!capture_mode_ && (e.time > st.time || st.next_index < e.spawned)) {
                spawns.clear();
                for (const GpuParticleSpawnRecord& r : e.records) {
                    if (r.index < st.next_index) continue;
                    if (r.time + static_cast<double>(r.particle.velocity_life.w) <= e.time) continue; // already dead
                    if (r.particle.meta[0] >= e.capacity) continue;
                    GpuParticleSpawn p = r.particle;
                    p.position_age.w = static_cast<float>(std::max(0.0, e.time - r.time));
                    spawns.push_back(p);
                }
                st.next_index = e.spawned;
                const float dt = static_cast<float>(e.time - st.time);
                st.time = e.time;
                ++st.serial;
                {
                    const auto need = std::max(1u, static_cast<std::uint32_t>(spawns.size()));
                    if (need > st.spawn_capacity || !st.spawns) {
                        if (st.spawns) dev.destroy(st.spawns);
                        st.spawn_capacity = std::max(need, std::min(e.capacity, std::max(64u, st.spawn_capacity * 2)));
                        auto b = dev.create_buffer({"gpu_particle_spawns", st.spawn_capacity * static_cast<std::uint32_t>(sizeof(GpuParticleSpawn)),
                                                    rhi::BufferUsageComputeRead});
                        if (!b) return std::move(b).error();
                        st.spawns = b.value();
                    }
                    if (!spawns.empty())
                        cmd.upload_buffer(st.spawns, 0,
                                          {reinterpret_cast<const std::uint8_t*>(spawns.data()), spawns.size() * sizeof(GpuParticleSpawn)}, true);
                    stats_.gpu_particle_spawns += static_cast<std::uint32_t>(spawns.size());
                }
                const GpuParticleParams& g = e.params;
                GpuParticleJob job{&st, static_cast<std::uint32_t>(spawns.size()), {}};
                ParticleSimUniforms& u = job.u;
                u.view_proj = view_proj;
                u.inv_view_proj = inverse(view_proj);
                u.world = g.world;
                u.view = camera_.view;
                u.gravity_drag = g.gravity_drag;
                u.turb = g.turbulence;
                u.size = g.size;
                u.color_start = g.color_start;
                u.color_end = g.color_end;
                u.flip = g.flipbook;
                u.collide = g.collision;
                u.eye_dt = Vec4(camera_.position, dt);
                u.screen = {static_cast<float>(rw), static_cast<float>(rh), proj[3].z, proj[2].z};
                std::uint32_t flags = g.world_space ? 1u : 0u;
                if (g.collide && g.world_space) flags |= 2u;
                if (g.flipbook.x * g.flipbook.y > 1.0f) flags |= 4u;
                if (g.size.w > 0.0f) flags |= 8u;
                if (e.material.texture) flags |= 16u;
                u.meta = {0u, 0u, st.serial, flags};
                const auto fin = [](float x) { return std::isfinite(x) ? x : 0.0f; };
                u.collide.w = std::clamp(fin(g.trail_alpha_end), 0.0f, 1.0f);
                u.trail = {std::max(1e-4f, fin(g.trail.x)), std::max(0.0f, fin(g.trail.y)), std::max(0.0f, fin(g.trail.z)), std::max(0.0f, fin(g.trail.w))};
                u.trail_meta = {st.trail_points, 0u, 0u, 0u};
                gpu_jobs.push_back(job);
            }
            ++stats_.gpu_particle_emitters;
            st.sorted_last = false;
            if (e.alive > 0 && st.state && st.generation == e.generation && st.instance == e.instance && e.bounds.valid() && frustum.intersects(e.bounds)) {
                if (st.trail_points > 0) {
                    // Trails first, so alpha heads blend over the ribbons they lead.
                    ParticleBatch t{e.material, 0, e.capacity * st.trail_points * 6, e.bounds, 0.0f};
                    t.ribbon = true;
                    t.buffer = st.ribbon;
                    t.index = st.ribbon_index;
                    gpu_batches.push_back(t);
                    stats_.gpu_particle_trail_vertices += e.capacity * (st.trail_points + 1) * 2;
                }
                if (st.trail_points > 0 && !e.params.trail_billboards) {
                    stats_.gpu_particles += e.alive;
                    continue; // trails only
                }
                ParticleBatch b{e.material, 0, e.capacity, e.bounds, 0.0f};
                b.buffer = st.instances;
                // Alpha blending depends on order: sort the drawn slots back to front on the GPU
                // (additive blending does not). Bitonic sort over a power-of-two pair array.
                if (e.material.blend == ParticleBlend::Alpha && particle_sort_ && !capture_mode_) {
                    const std::uint32_t n = std::max(256u, std::bit_ceil(e.capacity));
                    if (st.sort_size != n || !st.sorted) {
                        for (rhi::BufferHandle* h : {&st.pairs, &st.sorted})
                            if (*h) dev.destroy(*h);
                        auto pb = dev.create_buffer({"gpu_particle_sort_pairs", n * 8u, rhi::BufferUsageStorageWrite});
                        if (!pb) return std::move(pb).error();
                        st.pairs = pb.value();
                        auto sb = dev.create_buffer({"gpu_particle_sorted", e.capacity * static_cast<std::uint32_t>(sizeof(ParticleInstance)),
                                                     rhi::BufferUsageStorageWrite | rhi::BufferUsageVertex});
                        if (!sb) return std::move(sb).error();
                        st.sorted = sb.value();
                        st.sort_size = n;
                    }
                    GpuSortJob job{&st, {}};
                    job.u.eye = Vec4(camera_.position, 1.0f);
                    job.u.meta = {0u, e.capacity, n, 0u};
                    sort_jobs.push_back(job);
                    b.buffer = st.sorted;
                    st.sorted_last = true;
                    ++stats_.gpu_particle_sorted_emitters;
                }
                gpu_batches.push_back(b);
                stats_.gpu_particles += e.alive;
            }
        }
    }
    // Drop the GPU state of emitters that stopped being submitted.
    for (auto it = gpu_particle_states_.begin(); it != gpu_particle_states_.end();) {
        if (gpu_particle_frame_ - it->second.last_frame > 300) {
            destroy_gpu_particle_state(it->second);
            it = gpu_particle_states_.erase(it);
        } else {
            ++it;
        }
    }

    stage.emplace("render.cpu.particles");
    // Particles: cull batches, order them back to front, sort alpha batches' particles back to
    // front, then upload everything as one instance stream.
    std::vector<ParticleBatch> particle_draws;
    particle_upload_.clear();
    for (ParticleBatch b : particle_batches_) {
        if (!frustum.intersects(b.bounds)) continue;
        b.distance = length(b.bounds.center() - camera_.position);
        particle_draws.push_back(b);
    }
    for (ParticleBatch b : gpu_batches) {
        b.distance = length(b.bounds.center() - camera_.position);
        particle_draws.push_back(b);
    }
    std::stable_sort(particle_draws.begin(), particle_draws.end(),
                     [](const ParticleBatch& a, const ParticleBatch& b) { return a.distance > b.distance; });
    // CPU particle budget (ADR-0109): keep whole batches from the nearest; cut the nearest batch of
    // billboards when it alone is over. GPU batches are budgeted by their emitter's capacity.
    facts_.particle_batches_in_view = 0;
    facts_.particles_in_view = 0;
    {
        std::uint64_t used = 0;
        bool kept_any = false;
        std::vector<ParticleBatch> kept;
        kept.reserve(particle_draws.size());
        for (auto it = particle_draws.rbegin(); it != particle_draws.rend(); ++it) {
            ParticleBatch b = *it;
            if (!b.buffer) {
                ++facts_.particle_batches_in_view;
                facts_.particles_in_view += b.count;
                if (used + b.count > settings_.max_particles) {
                    if (kept_any || b.ribbon || settings_.max_particles == 0) continue;
                    b.count = settings_.max_particles; // nearest batch alone is over: draw part of it
                }
                used += b.count;
                kept_any = true;
            }
            kept.push_back(b);
        }
        particle_draws.assign(kept.rbegin(), kept.rend());
    }
    {
        const Vec3 fwd = camera_.forward();
        std::vector<std::pair<float, std::uint32_t>> order;
        for (ParticleBatch& b : particle_draws) {
            if (b.buffer) continue; // GPU particles: drawn straight from their instance buffer
            const auto first = static_cast<std::uint32_t>(particle_upload_.size());
            if (b.ribbon) {
                // Strip order is the triangle order; batches are still ordered back to front.
                particle_upload_.insert(particle_upload_.end(), particles_.begin() + b.offset, particles_.begin() + b.offset + b.count);
            } else if (b.material.blend == ParticleBlend::Alpha) {
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
    stage.emplace("render.cpu.decals_water");
    // Decals: frustum cull the boxes, keep the nearest kMaxDecals, draw by sort order then distance.
    std::vector<const Decal*> decal_draws;
    {
        static const Aabb kUnit{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        std::vector<std::pair<float, const Decal*>> candidates;
        for (const Decal& d : decals_) {
            const Aabb world = kUnit.transformed(d.transform);
            if (!world.valid() || !frustum.intersects(world)) continue;
            candidates.emplace_back(length(world.center() - camera_.position), &d);
        }
        facts_.decals_in_view = static_cast<std::uint32_t>(candidates.size());
        const std::size_t max_decals = settings_.max_decals;
        if (candidates.size() > max_decals) {
            std::nth_element(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(max_decals), candidates.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            candidates.resize(max_decals);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            if (a.second->sort_order != b.second->sort_order) return a.second->sort_order < b.second->sort_order;
            return a.first > b.first;
        });
        for (const auto& c : candidates) decal_draws.push_back(c.second);
    }
    stats_.decals = static_cast<std::uint32_t>(decal_draws.size());
    // Water: cull by the displaced bounds, keep the nearest kMaxWaterSurfaces, draw far to near.
    std::vector<const WaterSurface*> water_draws;
    {
        std::vector<std::pair<float, const WaterSurface*>> candidates;
        for (const WaterSurface& w : waters_) {
            float amp = 0;
            for (const WaterWave& wave : w.waves) amp += std::abs(wave.amplitude) * (1.0f + std::abs(wave.q));
            const Aabb box{{w.center.x - w.half_extent.x - amp, w.center.y - amp, w.center.z - w.half_extent.y - amp},
                           {w.center.x + w.half_extent.x + amp, w.center.y + amp, w.center.z + w.half_extent.y + amp}};
            if (!frustum.intersects(box)) continue;
            const Vec3 c = camera_.position;
            const Vec3 nearest{std::clamp(c.x, box.min.x, box.max.x), std::clamp(c.y, box.min.y, box.max.y),
                               std::clamp(c.z, box.min.z, box.max.z)};
            candidates.emplace_back(length(nearest - c), &w);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        facts_.water_in_view = static_cast<std::uint32_t>(candidates.size());
        if (candidates.size() > settings_.max_water_surfaces) candidates.resize(settings_.max_water_surfaces);
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) water_draws.push_back(it->second);
    }
    stats_.water_surfaces = static_cast<std::uint32_t>(water_draws.size());
    stats_.particles = 0;
    stats_.ribbon_vertices = 0;
    for (const ParticleBatch& b : particle_draws)
        if (!b.buffer) (b.ribbon ? stats_.ribbon_vertices : stats_.particles) += b.count;
    stats_.particles += stats_.gpu_particles;
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
    // Sun contact shadows (ADR-0183) share the SSAO pass and its target (green channel).
    const bool contact_shadows = S.contact_shadows && std::isfinite(env_.contact_shadow_length) &&
                                 env_.contact_shadow_length > 0.0f && sun_.intensity > 0.0f;
    const bool screen_occlusion = ssao || contact_shadows;
    const bool user_prepass = S.depth_prepass || screen_occlusion;
    const bool bloom = S.bloom && env_.bloom_intensity > 0.0f;
    const bool auto_exposure = S.auto_exposure && env_.auto_exposure;
    const bool linear_out = env_.linear_output || capture_mode_;
    const bool fxaa = S.fxaa && !linear_out && !taa; // captures keep float precision
    facts_.ao_requested = env_.ao_intensity > 0.0f && env_.ao_radius > 0.0f;
    facts_.ssao = ssao;
    facts_.contact_shadows_requested = env_.contact_shadow_length > 0.0f;
    facts_.contact_shadows = contact_shadows;
    facts_.bloom_requested = env_.bloom_intensity > 0.0f;
    facts_.bloom = bloom;
    facts_.auto_exposure_requested = env_.auto_exposure;
    facts_.auto_exposure = auto_exposure;
    facts_.linear_output = linear_out;
    facts_.fxaa = fxaa;
    if (auto_exposure) VICTUS_TRY_STATUS(ensure_exposure_textures());

    const std::uint32_t atlas_tiles = cascade_count_ > 1 ? 2u : 1u;
    const std::uint32_t atlas_size = shadow_res * atlas_tiles;

    LightingUniforms lighting{};
    lighting.camera_pos = Vec4(camera_.position, contact_shadows ? 2.0f : (ssao ? 1.0f : 0.0f));
    lighting.camera_forward = Vec4(camera_.forward(), texture_mip_bias);
    lighting.sun_dir = Vec4(normalize(sun_.direction), shadows ? 1.0f : 0.0f);
    lighting.sun_color = rgb(sun_.color, sun_.intensity);
    lighting.sky_color = rgb(env_.sky_zenith, env_.ambient_intensity, env_.exposure);
    lighting.horizon_color = rgb(env_.sky_horizon, env_.ambient_intensity);
    lighting.ground_color = rgb(env_.ground, env_.ambient_intensity, static_cast<float>(local_lights.size()));
    lighting.cluster_grid = {static_cast<float>(clusters.grid.tiles_x), static_cast<float>(clusters.grid.tiles_y),
                             static_cast<float>(clusters.grid.slices), static_cast<float>(clusters.grid.max_lights_per_cluster)};
    lighting.cluster_depth = {clusters.z_near, static_cast<float>(clusters.grid.slices) / std::log(clusters.z_far / clusters.z_near), 0, 0};
    lighting.shadow_params = {1.0f / static_cast<float>(atlas_size), 0.0004f, kShadowNormalBiasTexels,
                              static_cast<float>(cascade_count_)};
    // w: the lightmap alpha is a shadowmask for this sun (ADR-0131).
    const bool shadowmask = env_.lightmap && sun_.shadows && sun_.intensity > 0.0f &&
                            math::length(env_.lightmap_shadowmask_direction) > 0.5f &&
                            math::dot(normalize(env_.lightmap_shadowmask_direction), normalize(sun_.direction)) > 0.99985f;
    lighting.cascade_params = {S.soft_shadows ? (S.fast_soft_shadows ? 2.0f : 1.0f) : 0.0f,
                               1.0f / static_cast<float>(atlas_tiles), kCascadeTransition, shadowmask ? 1.0f : 0.0f};
    lighting.cascade_splits = {cascade_splits_[0], cascade_splits_[1], cascade_splits_[2], cascade_splits_[3]};
    lighting.cascade_texel = {cascade_texel_[0], cascade_texel_[1], cascade_texel_[2], cascade_texel_[3]};
    {
        double t = time_ ? *time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - render_epoch()).count();
        t = std::fmod(std::max(t, 0.0), 3600.0); // keeps float precision for sin(time) and scrolling
        lighting.screen = inv_size(rw, rh, static_cast<float>(t), hdr_quant);
        shader_time_ = static_cast<float>(t);
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
    const bool volume_allowed = volume_baked_ ? S.baked_probes : S.irradiance_volumes;
    const IrradianceVolume* gi = volume_allowed && volume_ && volume_->texture() && volume_->valid_count() > 0 ? volume_ : nullptr;
    stats_.irradiance_probes = gi ? gi->valid_count() : 0;
    facts_.irradiance_available = volume_ && volume_->texture() && volume_->valid_count() > 0;
    facts_.irradiance = gi != nullptr;
    facts_.irradiance_probes = stats_.irradiance_probes;
    facts_.irradiance_baked = volume_baked_;
    if (gi) {
        const IrradianceGrid& g = gi->grid();
        lighting.gi_min = Vec4(g.bounds.min, S.probe_sh_bands <= 1 ? 2.0f : 1.0f); // 2: L1 only (ADR-0138)
        lighting.gi_max = Vec4(g.bounds.max, volume_intensity_);
        lighting.gi_counts = {static_cast<float>(g.counts[0]), static_cast<float>(g.counts[1]), static_cast<float>(g.counts[2]),
                              volume_normal_offset_};
    }
    const rhi::TextureHandle gi_tex = gi ? gi->texture() : ctx_.black();
    const bool local_shadows = !local_views.empty();
    if (local_shadows)
        lighting.local_shadow = {1.0f / static_cast<float>(S.local_shadow_atlas), static_cast<float>(local_tiles_per_row),
                                 static_cast<float>(stats_.point_lights), 1.0f};

    stage.emplace("render.cpu.fog_setup");
    // Volumetric fog (ADR-0098): froxel grid setup; the passes run after the blended geometry.
    struct FogUniforms {
        Mat4 inv_view_proj, prev_view_proj;
        Mat4 cascade_vp[4];
        Vec4 camera_pos, camera_forward, prev_camera_pos, prev_camera_forward, grid;
        Vec4 sun_dir, sun_color, ambient, albedo, height_fog;
        Vec4 shadow_params, cascade_params, cascade_splits, cluster_grid, cluster_depth, local_shadow;
        Vec4 noise, media; // ADR-0186: noise offset (xyz) and frequency (w); amount, shadow steps/distance/strength
        Vec4 volumes[kMaxFogVolumes * 5];
    };
    static_assert(sizeof(FogUniforms) == 1312);
    struct FogApplyUniforms {
        Vec4 proj, screen, grid;
    };
    const VolumetricFog& vf = env_.volumetric_fog;
    const bool fog = S.volumetric_fog && vf.enabled && !capture_mode_ && dev.capabilities().compute && std::isfinite(vf.density) &&
                     (vf.density > 0.0f || !fog_volumes_.empty()) && ensure_fog_pipelines();
    facts_.fog_requested = vf.enabled && std::isfinite(vf.density) && (vf.density > 0.0f || !fog_volumes_.empty());
    facts_.fog = fog;
    FogUniforms fog_u{};
    FogApplyUniforms fog_apply_u{};
    std::array<std::uint32_t, 3> fog_dims{};
    float fog_far = 0.0f;
    const std::uint32_t fog_cur = fog_index_, fog_prev = fog_index_ ^ 1u;
    if (fog) {
        const std::uint32_t tile = S.volumetric_fog_tile;
        fog_dims = {std::clamp((rw + tile - 1) / tile, 1u, 160u), std::clamp((rh + tile - 1) / tile, 1u, 90u), S.volumetric_fog_slices};
        fog_far = std::min(std::isfinite(vf.distance) ? std::clamp(vf.distance, 8.0f, 1000.0f) : 80.0f, std::max(camera_.z_far, 1.0f));
        const std::uint32_t bytes = fog_dims[0] * fog_dims[1] * fog_dims[2] * static_cast<std::uint32_t>(sizeof(Vec4));
        VICTUS_TRY_STATUS(ensure_buffer(fog_froxels_[0], fog_froxel_capacity_[0], bytes,
                                        rhi::BufferUsageStorageWrite | rhi::BufferUsageComputeRead, "fog_froxels_a"));
        VICTUS_TRY_STATUS(ensure_buffer(fog_froxels_[1], fog_froxel_capacity_[1], bytes,
                                        rhi::BufferUsageStorageWrite | rhi::BufferUsageComputeRead, "fog_froxels_b"));
        VICTUS_TRY_STATUS(ensure_buffer(fog_integrated_, fog_integrated_capacity_, bytes,
                                        rhi::BufferUsageStorageWrite | rhi::BufferUsageComputeRead | rhi::BufferUsageStorageRead,
                                        "fog_integrated"));
        if (fog_dims != fog_dims_ || fog_far != fog_far_) fog_history_valid_ = false;
        const float temporal = std::isfinite(vf.temporal) ? std::clamp(vf.temporal, 0.0f, 0.98f) : 0.9f;
        const bool history = fog_history_valid_ && temporal > 0.0f;
        // R1 low-discrepancy sequence over the slice depth; the centre without temporal blending.
        const float jitter_z =
            temporal > 0.0f ? static_cast<float>(std::fmod(0.5 + static_cast<double>(fog_frame_) * 0.6180339887498949, 1.0)) : 0.5f;
        fog_u.inv_view_proj = inverse(view_proj_unjittered);
        fog_u.prev_view_proj = fog_prev_view_proj_;
        for (std::uint32_t c = 0; c < 4; ++c) fog_u.cascade_vp[c] = cascade_vp_[c];
        fog_u.camera_pos = Vec4(camera_.position, fog_far);
        fog_u.camera_forward = Vec4(camera_.forward(), jitter_z);
        fog_u.prev_camera_pos = Vec4(fog_prev_position_, history ? 1.0f : 0.0f);
        fog_u.prev_camera_forward = Vec4(fog_prev_forward_, temporal);
        const auto finite_or = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        fog_u.sun_dir = lighting.sun_dir;
        fog_u.sun_color = sun_.intensity > 0.0f ? rgb(sun_.color, sun_.intensity * std::max(finite_or(vf.sun_intensity, 1.0f), 0.0f))
                                                : Vec4{};
        // Mean sky radiance (the isotropic phase function integrates a uniform sky to its radiance).
        Vec3 ambient{};
        if (env_map && env_map->cube()) ambient = env_map->sh().coefficients[0] * (env_intensity * 0.2820948f);
        else {
            const Color& z = env_.sky_zenith;
            const Color& h = env_.sky_horizon;
            const Color& g = env_.ground;
            ambient = Vec3{z.r + 2.0f * h.r + g.r, z.g + 2.0f * h.g + g.g, z.b + 2.0f * h.b + g.b} * (0.25f * env_.ambient_intensity);
        }
        ambient = ambient * std::max(finite_or(vf.ambient_intensity, 1.0f), 0.0f);
        fog_u.ambient = Vec4(ambient, std::max(finite_or(vf.local_light_intensity, 1.0f), 0.0f));
        fog_u.albedo = {std::clamp(finite_or(vf.albedo.r, 1.0f), 0.0f, 1.0f), std::clamp(finite_or(vf.albedo.g, 1.0f), 0.0f, 1.0f),
                        std::clamp(finite_or(vf.albedo.b, 1.0f), 0.0f, 1.0f), std::clamp(finite_or(vf.anisotropy, 0.0f), -0.95f, 0.95f)};
        fog_u.height_fog = {std::max(vf.density, 0.0f), std::max(finite_or(vf.height_falloff, 0.0f), 0.0f), finite_or(vf.height, 0.0f),
                            static_cast<float>(local_lights.size())};
        fog_u.shadow_params = lighting.shadow_params;
        fog_u.cascade_params = lighting.cascade_params;
        fog_u.cascade_splits = lighting.cascade_splits;
        fog_u.cluster_grid = lighting.cluster_grid;
        fog_u.cluster_depth = lighting.cluster_depth;
        fog_u.local_shadow = lighting.local_shadow;
        // Heterogeneous media and self-shadowing (ADR-0186). The noise drifts with the wind over
        // the shader clock (wrapped at an hour, like surface shader time).
        {
            const float amount = std::clamp(finite_or(vf.noise, 0.0f), 0.0f, 1.0f);
            const float scale = std::clamp(finite_or(vf.noise_scale, 6.0f), 0.25f, 1000.0f);
            const Vec3 wind{finite_or(vf.wind.x, 0.0f), finite_or(vf.wind.y, 0.0f), finite_or(vf.wind.z, 0.0f)};
            const Vec3 drift = wind * shader_time_;
            fog_u.noise = {drift.x, drift.y, drift.z, 1.0f / scale};
            const float strength = std::clamp(finite_or(vf.self_shadow, 0.0f), 0.0f, 4.0f);
            const std::uint32_t steps = strength > 0.0f && sun_.intensity > 0.0f ? S.volumetric_fog_shadow_steps : 0u;
            fog_u.media = {amount, static_cast<float>(steps), std::clamp(finite_or(vf.self_shadow_distance, 16.0f), 1.0f, 200.0f),
                           strength};
            facts_.fog_noise = amount > 0.0f;
            facts_.fog_self_shadow = steps > 0;
        }
        // Fog volumes inside the view frustum, nearest first.
        std::vector<std::pair<float, const FogVolume*>> vols;
        {
            static const Aabb kUnit{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
            for (const FogVolume& v : fog_volumes_) {
                const Aabb world = kUnit.transformed(v.transform);
                if (!world.valid() || !frustum.intersects(world)) continue;
                vols.emplace_back(length(world.center() - camera_.position), &v);
            }
            std::stable_sort(vols.begin(), vols.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        std::uint32_t nvol = 0;
        for (const auto& [dist, v] : vols) {
            if (nvol == kMaxFogVolumes) break;
            const Mat4 inv = inverse(v->transform);
            bool ok = true;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 3; ++r) ok = ok && std::isfinite(inv[c][r]);
            if (!ok) continue;
            Vec4* out = &fog_u.volumes[nvol * 5];
            for (int r = 0; r < 3; ++r) out[r] = {inv[0][r], inv[1][r], inv[2][r], inv[3][r]};
            out[3] = {std::clamp(finite_or(v->albedo.r, 1.0f), 0.0f, 1.0f), std::clamp(finite_or(v->albedo.g, 1.0f), 0.0f, 1.0f),
                      std::clamp(finite_or(v->albedo.b, 1.0f), 0.0f, 1.0f), v->density};
            out[4] = {std::clamp(finite_or(v->edge_fade, 0.1f), 0.0f, 0.5f), 0.0f, 0.0f, 0.0f};
            ++nvol;
        }
        fog_u.grid = {static_cast<float>(fog_dims[0]), static_cast<float>(fog_dims[1]), static_cast<float>(fog_dims[2]),
                      static_cast<float>(nvol)};
        fog_apply_u.proj = {proj[3].z, proj[2].z, 0.0f, 0.0f};
        fog_apply_u.screen = inv_size(rw, rh);
        fog_apply_u.grid = {fog_u.grid.x, fog_u.grid.y, fog_u.grid.z, fog_far};
        stats_.volumetric_fog = true;
        stats_.fog_froxels_x = fog_dims[0];
        stats_.fog_froxels_y = fog_dims[1];
        stats_.fog_slices = fog_dims[2];
        stats_.fog_volumes = nvol;
        // Blended geometry, particles and precipitation are drawn after the fog composite and
        // fetch the grid at their own depth (ADR-0119).
        lighting.volumetric = fog_apply_u.grid;
    }
    // Fragment storage buffer for that lookup; a bound placeholder when fog is off (never read).
    const auto fog_grid_buffer = [this, fog] { return fog ? fog_integrated_ : light_buffer_; };

    // Volumetric clouds (ADR-0111): layer setup; the passes run after the blended geometry,
    // before the volumetric fog is composited over everything.
    struct CloudUniforms {
        Mat4 inv_view_proj;
        Vec4 camera, sun_dir, sun_color, ambient, layer, noise, wind, screen;
    };
    struct CloudApplyUniforms {
        Mat4 inv_view_proj;
        Vec4 camera, layer, screen;
    };
    // Average sky radiance (DC term of the environment, or the analytic gradient).
    const auto sky_ambient = [&]() {
        if (env_map && env_map->cube()) return env_map->sh().coefficients[0] * (env_intensity * 0.2820948f);
        const Color& z = env_.sky_zenith;
        const Color& h = env_.sky_horizon;
        const Color& g = env_.ground;
        return Vec3{z.r + 2.0f * h.r + g.r, z.g + 2.0f * h.g + g.g, z.b + 2.0f * h.b + g.b} * (0.25f * env_.ambient_intensity);
    };
    const VolumetricClouds& vc = env_.clouds;
    const bool clouds_valid = vc.enabled && std::isfinite(vc.coverage) && vc.coverage > 0.0f && std::isfinite(vc.density) &&
                              vc.density > 0.0f && std::isfinite(vc.bottom) && std::isfinite(vc.top) && vc.top > vc.bottom;
    facts_.clouds_requested = clouds_valid;
    CloudUniforms cloud_u{};
    CloudApplyUniforms cloud_apply_u{};
    std::uint32_t cloud_w = 0, cloud_h = 0;
    rhi::PipelineHandle cloud_march_pl{}, cloud_apply_pl{};
    if (clouds_valid && S.volumetric_clouds && !capture_mode_ && !clouds_failed_) {
        auto march = post_pipeline("clouds.frag", rhi::Format::RGBA16_FLOAT);
        auto apply = march ? post_pipeline("clouds_apply.frag", hdr_format_, rhi::BlendMode::PremultipliedAlpha)
                           : Result<rhi::PipelineHandle>(march.error());
        if (!apply) {
            VICTUS_LOG_WARN("render", "volumetric clouds disabled: {}", apply.error().message);
            clouds_failed_ = true;
        } else {
            cloud_march_pl = march.value();
            cloud_apply_pl = apply.value();
        }
    }
    const bool clouds = cloud_apply_pl.valid();
    facts_.clouds = clouds;
    if (clouds) {
        const auto finite_or = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        cloud_w = std::max(1u, (rw + S.cloud_downsample - 1) / S.cloud_downsample);
        cloud_h = std::max(1u, (rh + S.cloud_downsample - 1) / S.cloud_downsample);
        const Mat4 inv_vp = inverse(view_proj);
        const float max_dist = std::clamp(finite_or(vc.distance, 30000.0f), 100.0f, 200000.0f);
        cloud_u.inv_view_proj = inv_vp;
        cloud_u.camera = Vec4(camera_.position, max_dist);
        cloud_u.sun_dir = Vec4(-normalize(sun_.direction), std::clamp(finite_or(vc.anisotropy, 0.6f), 0.0f, 0.95f));
        const float sun_mul = std::max(finite_or(vc.sun_intensity, 1.0f), 0.0f);
        cloud_u.sun_color = sun_.intensity > 0.0f ? rgb(sun_.color, sun_.intensity * sun_mul, static_cast<float>(S.cloud_light_steps))
                                                  : Vec4{0, 0, 0, static_cast<float>(S.cloud_light_steps)};
        const Vec3 ambient = sky_ambient() * std::max(finite_or(vc.ambient_intensity, 1.0f), 0.0f);
        cloud_u.ambient = Vec4(ambient, static_cast<float>(S.cloud_steps));
        cloud_u.layer = {vc.bottom, vc.top, std::clamp(vc.coverage, 0.0f, 1.0f), std::min(vc.density, 10.0f)};
        const float shape = std::clamp(finite_or(vc.shape_size, 4000.0f), 10.0f, 1e6f);
        const float detail = std::clamp(finite_or(vc.detail_size, 600.0f), 1.0f, 1e6f);
        cloud_u.noise = {1.0f / shape, 1.0f / detail, std::clamp(finite_or(vc.detail, 0.35f), 0.0f, 1.0f),
                         S.taa ? static_cast<float>(cloud_frame_ % 64u) : 0.0f};
        // Wind drift; wrapped every 200 km so the offset keeps float precision.
        const double t = time_ ? *time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - render_epoch()).count();
        const auto drift = [&](float w) {
            return static_cast<float>(std::fmod(static_cast<double>(std::isfinite(w) ? w : 0.0f) * std::max(t, 0.0), 200000.0));
        };
        cloud_u.wind = {-drift(vc.wind.x), -drift(vc.wind.y), -drift(vc.wind.z), 0.0f};
        cloud_u.screen = {1.0f / static_cast<float>(cloud_w), 1.0f / static_cast<float>(cloud_h),
                          0.25f / static_cast<float>(cloud_w), 0.25f / static_cast<float>(cloud_h)};
        const float shadow = std::clamp(finite_or(vc.shadow_strength, 1.0f), 0.0f, 1.0f);
        if (shadow > 0.0f) {
            lighting.cloud_layer = cloud_u.layer;
            lighting.cloud_shadow = {cloud_u.noise.x, shadow, 1.0f, 0.0f};
            lighting.cloud_wind = cloud_u.wind;
            stats_.cloud_shadows = true;
        }
        cloud_apply_u.inv_view_proj = inv_vp;
        cloud_apply_u.camera = Vec4(camera_.position, 0.0f);
        cloud_apply_u.layer = {vc.bottom, vc.top, 0.0f, 0.0f};
        cloud_apply_u.screen = inv_size(rw, rh);
        stats_.volumetric_clouds = true;
        stats_.cloud_width = cloud_w;
        stats_.cloud_height = cloud_h;
        stats_.cloud_steps = S.cloud_steps;
        facts_.cloud_width = cloud_w;
        facts_.cloud_height = cloud_h;
        facts_.cloud_steps = S.cloud_steps;
    }

    // Wet surfaces and precipitation (ADR-0113).
    if (std::isfinite(env_.wetness) && env_.wetness > 0.0f) {
        lighting.weather = {std::min(env_.wetness, 1.0f), 0.0f, 0.0f, 0.0f};
        stats_.surface_wetness = true;
    }
    if (rain_occlusion) {
        lighting.weather.y = 1.0f;
        lighting.weather.z = rain_bias;
        lighting.rain_vp = rain_vp;
    }
    const Precipitation& pr = env_.precipitation;
    const auto fin = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    const float precip_intensity = std::clamp(fin(pr.intensity, 0.0f), 0.0f, 1.0f);
    // Puddles and rain ripples (ADR-0117). Ripples follow drawn rain (not snow, not probe captures).
    {
        const float puddles = std::clamp(fin(env_.puddles, 0.0f), 0.0f, 1.0f);
        const float ripples = pr.type == PrecipitationType::Rain && S.precipitation && S.max_precipitation > 0 && !capture_mode_
                                  ? precip_intensity * std::clamp(fin(pr.ripples, 0.0f), 0.0f, 1.0f)
                                  : 0.0f;
        lighting.weather.w = puddles;
        lighting.puddles = {1.0f / std::clamp(fin(env_.puddle_size, 3.0f), 0.25f, 100.0f), ripples, 1.0f / 0.35f, 1.3f};
        stats_.surface_puddles = puddles > 0.0f;
        stats_.rain_ripples = ripples > 0.0f;
    }
    const bool snow = pr.type == PrecipitationType::Snow;
    facts_.precipitation_requested = pr.type != PrecipitationType::None && precip_intensity > 0.0f;
    struct PrecipitationFragmentUniforms {
        Vec4 proj;   // xy view depth = x / (depth + y), z rain occlusion on, w its depth bias
        Vec4 screen; // xy 1 / render size, z 1 / softness, w type
        Mat4 rain_vp;
        Vec4 fog_color, fog_params, view; // distance fog (ADR-0116)
        Vec4 volumetric;                  // volumetric fog grid (ADR-0119)
    };
    struct SplashUniforms {
        Mat4 view_proj;
        Vec4 eye, right, up;
        Vec4 params; // x radius, y crown size, z 1 / lifetime, w pixel size at 1 m
        Vec4 color;
        Mat4 rain_vp, rain_inv;
    };
    PrecipitationUniforms precip_u{};
    PrecipitationFragmentUniforms precip_fu{};
    SplashUniforms splash_u{};
    PrecipitationFragmentUniforms splash_fu{};
    std::uint32_t splash_count = 0;
    rhi::PipelineHandle splash_pl{};
    std::uint32_t precip_count = 0;
    rhi::PipelineHandle precip_pl{};
    if (facts_.precipitation_requested && S.precipitation && !capture_mode_ && !precipitation_failed_) {
        precip_count = S.max_precipitation == 0 ? 0u : std::max(1u, static_cast<std::uint32_t>(std::lround(precip_intensity * static_cast<float>(S.max_precipitation))));
        if (precip_count > 0) {
            auto pl = precipitation_pipeline();
            if (!pl) {
                VICTUS_LOG_WARN("render", "precipitation disabled: {}", pl.error().message);
                precipitation_failed_ = true;
                precip_count = 0;
            } else precip_pl = pl.value();
        }
    }
    if (precip_count > 0) {
        const double t = time_ ? *time_ : std::chrono::duration<double>(std::chrono::steady_clock::now() - render_epoch()).count();
        const float radius = precipitation_radius(pr);
        const float drop_width = std::clamp(fin(pr.size, 0.0f) > 0.0f ? pr.size : (snow ? 0.025f : 0.006f), 0.0005f, 1.0f);
        const float opacity = std::clamp(fin(pr.opacity, 0.0f) > 0.0f ? pr.opacity : (snow ? 0.9f : 0.35f), 0.0f, 1.0f);
        // Lit like a small diffuse scatterer: sky ambient plus a share of the sun (cloud shadows
        // and the shadow map are not applied).
        Vec3 light = sky_ambient();
        if (sun_.intensity > 0.0f) {
            const Vec4 sc = rgb(sun_.color, sun_.intensity, 0.0f);
            light = light + Vec3{sc.x, sc.y, sc.z} * (snow ? 0.3f : 0.15f);
        }
        light = light * std::max(fin(pr.brightness, 1.0f), 0.0f);
        precip_u.view_proj = view_proj;
        // Wrapped so drop positions keep float precision (one jump every ~68 minutes).
        precip_u.eye = Vec4(camera_.position, static_cast<float>(std::fmod(std::max(t, 0.0), 4096.0)));
        precip_u.velocity = Vec4(precipitation_velocity(pr), snow ? 1.0f : 0.0f);
        precip_u.volume = {2.0f * radius, drop_width, std::clamp(fin(pr.streak, 0.03f), 0.0f, 0.5f),
                           2.0f / (std::max(std::abs(proj[1].y), 1e-4f) * static_cast<float>(rh))};
        precip_u.color = {light.x * pr.color.r, light.y * pr.color.g, light.z * pr.color.b, opacity * std::clamp(fin(pr.color.a, 1.0f), 0.0f, 1.0f)};
        const Mat4 cam_world = inverse(camera_.view);
        precip_u.right = Vec4(normalize(cam_world.transform_vector({1, 0, 0})), 0);
        precip_u.up = Vec4(normalize(cam_world.transform_vector({0, 1, 0})), 0);
        precip_fu.proj = {proj[3].z, proj[2].z, rain_occlusion ? 1.0f : 0.0f, rain_bias};
        precip_fu.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 1.0f / 0.1f, snow ? 1.0f : 0.0f};
        precip_fu.rain_vp = rain_vp;
        precip_fu.fog_color = lighting.fog_color;
        precip_fu.fog_params = lighting.fog_params;
        precip_fu.view = {1.0f / proj[0].x, 1.0f / proj[1].y, 0, 0};
        precip_fu.volumetric = lighting.volumetric;
        stats_.precipitation_drops = precip_count;
        // Rain splashes (ADR-0118) need the occlusion map to find where drops land.
        const float splashes = std::clamp(fin(pr.splashes, 0.0f), 0.0f, 1.0f);
        if (!snow && rain_occlusion && splashes > 0.0f && !splash_failed_) {
            splash_count = static_cast<std::uint32_t>(std::lround(static_cast<float>(precip_count) * 0.25f * splashes));
            if (splash_count > 0) {
                auto pl = precipitation_pipeline(true);
                if (!pl) {
                    VICTUS_LOG_WARN("render", "rain splashes disabled: {}", pl.error().message);
                    splash_failed_ = true;
                    splash_count = 0;
                } else splash_pl = pl.value();
            }
        }
        if (splash_count > 0) {
            splash_u.view_proj = view_proj;
            splash_u.eye = precip_u.eye;
            splash_u.right = precip_u.right;
            splash_u.up = precip_u.up;
            splash_u.params = {radius, 0.12f, 1.0f / 0.3f, precip_u.volume.w};
            splash_u.color = precip_u.color;
            splash_u.color.w = std::min(1.0f, precip_u.color.w * 1.6f);
            splash_u.rain_vp = rain_vp;
            splash_u.rain_inv = inverse(rain_vp);
            splash_fu = precip_fu;
            splash_fu.proj.z = 0.0f; // splashes sit on the occluders: no occlusion test
            splash_fu.screen.w = 2.0f;
            stats_.precipitation_splashes = splash_count;
        }
    }
    facts_.precipitation_drops = precip_count;

    stage.emplace("render.cpu.graph_build");
    RenderGraph graph;
    rhi::TextureDesc target_desc;
    target_desc.name = "scene_target";
    target_desc.format = target_format;
    target_desc.width = width;
    target_desc.height = height;
    target_desc.usage = rhi::TextureUsageColorTarget;
    const RGTexture out = graph.import_texture("target", target, target_desc);
    graph.mark_output(out);

    // Textures a surface shader declares (ADR-0171): fragment slots 12.. and vertex slots 0...
    const auto bind_shader_textures = [this, linear = material_linear.value()](rhi::RenderPass& pass, const Material& m, bool fragment) {
        if (m.shader_texture_count == 0) return;
        const rhi::SamplerHandle smp = m.nearest_filter ? ctx_.nearest_repeat() : linear;
        for (std::uint32_t i = 0; i < m.shader_texture_count; ++i) {
            const rhi::TextureHandle t = m.shader_textures[i] ? m.shader_textures[i] : ctx_.white();
            if (fragment) pass.bind_texture(rhi::ShaderStage::Fragment, 12 + i, t, smp);
            if (m.vertex_stage) pass.bind_texture(rhi::ShaderStage::Vertex, i, t, smp);
        }
    };
    // ADR-0067: texture and cutoff of an alpha-tested caster (shadow_alpha.frag).
    const auto bind_alpha_test = [this, linear = material_linear.value()](rhi::RenderPass& pass, rhi::CommandBuffer& cb,
                                                                          const Material& m) {
        const AlphaTestUniforms au{Vec4(m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y),
                                   Vec4(m.alpha_cutoff, m.base_color.a, 0.0f, 0.0f)};
        cb.push_uniforms(rhi::ShaderStage::Fragment, 0, au);
        pass.bind_texture(rhi::ShaderStage::Fragment, 0, m.base_color_texture,
                          m.nearest_filter ? ctx_.nearest_repeat() : linear);
    };
    RGTexture local_shadow_map;
    if (local_shadows) {
        rhi::TextureDesc sd;
        sd.format = depth_format_;
        sd.width = sd.height = S.local_shadow_atlas;
        sd.usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
        local_shadow_map = graph.create_texture("local_shadow_atlas", sd);
        graph.add_pass(
            "local_shadows", [&](PassBuilder& b) { b.write_depth(local_shadow_map, rhi::LoadOp::Clear, 1.0f); },
            [&](PassContext& ctx) {
                const auto tile = static_cast<float>(S.local_shadow_tile);
                for (std::size_t v = 0; v < local_views.size(); ++v) {
                    const auto t = static_cast<std::uint32_t>(v);
                    ctx.pass->set_viewport({static_cast<float>(t % local_tiles_per_row) * tile,
                                            static_cast<float>(t / local_tiles_per_row) * tile, tile, tile, 0, 1});
                    ShadowUniforms su{local_views[v].view_proj};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, su);
                    rhi::PipelineHandle bound{};
                    for (const Batch& b : local_batches[v]) {
                        const bool cut = !b.first->skinned && shadow_alpha_tested(b.first->material);
                        rhi::PipelineHandle want = b.first->skinned ? skin_shadow_ : cut ? shadow_alpha_ : shadow_pipeline_;
                        if (const SurfaceVertexStage* vx = b.first->material.vertex_stage)
                            if (const auto custom = surface_shadow_pipeline(*vx, b.first->skinned, cut)) want = custom;
                        if (!(bound == want)) {
                            ctx.pass->bind_pipeline(want);
                            if (b.first->skinned) ctx.pass->bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                            bound = want;
                        }
                        if (b.first->material.vertex_stage) {
                            push_vertex_material(ctx.cmd, b.first->material, true);
                            bind_shader_textures(*ctx.pass, b.first->material, false);
                        }
                        if (cut) bind_alpha_test(*ctx.pass, ctx.cmd, b.first->material);
                        if (b.first->skinned) ctx.pass->bind_vertex_buffer(2, b.first->mesh->skin);
                        ctx.pass->bind_vertex_buffer(0, b.first->mesh->vertices);
                        ctx.pass->bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                        ctx.pass->bind_index_buffer(b.first->mesh->indices, rhi::IndexType::U32);
                        draw_mesh_lod(*ctx.pass, *b.first->mesh, b.lod, b.instance_count);
                    }
                }
            });
    }

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
                        const bool cut = !b.first->skinned && shadow_alpha_tested(b.first->material);
                        rhi::PipelineHandle want = b.first->skinned ? skin_shadow_ : cut ? shadow_alpha_ : shadow_pipeline_;
                        if (const SurfaceVertexStage* vx = b.first->material.vertex_stage)
                            if (const auto custom = surface_shadow_pipeline(*vx, b.first->skinned, cut)) want = custom;
                        if (!(bound == want)) {
                            ctx.pass->bind_pipeline(want);
                            if (b.first->skinned) ctx.pass->bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                            bound = want;
                        }
                        if (b.first->material.vertex_stage) {
                            push_vertex_material(ctx.cmd, b.first->material, true);
                            bind_shader_textures(*ctx.pass, b.first->material, false);
                        }
                        if (cut) bind_alpha_test(*ctx.pass, ctx.cmd, b.first->material);
                        if (b.first->skinned) ctx.pass->bind_vertex_buffer(2, b.first->mesh->skin);
                        ctx.pass->bind_vertex_buffer(0, b.first->mesh->vertices);
                        ctx.pass->bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                        ctx.pass->bind_index_buffer(b.first->mesh->indices, rhi::IndexType::U32);
                        draw_mesh_lod(*ctx.pass, *b.first->mesh, b.lod, b.instance_count);
                    }
                }
            });
    }

    RGTexture rain_map;
    if (rain_occlusion) {
        rhi::TextureDesc sd;
        sd.format = depth_format_;
        sd.width = sd.height = rain_res;
        sd.usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
        rain_map = graph.create_texture("rain_occlusion", sd);
        graph.add_pass(
            "rain_occlusion", [&](PassBuilder& b) { b.write_depth(rain_map, rhi::LoadOp::Clear, 1.0f); },
            [&](PassContext& ctx) {
                ShadowUniforms su{rain_vp};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, su);
                rhi::PipelineHandle bound{};
                for (const Batch& b : F.rain_batches) {
                    const bool cut = !b.first->skinned && shadow_alpha_tested(b.first->material);
                    rhi::PipelineHandle want = b.first->skinned ? skin_shadow_ : cut ? shadow_alpha_ : shadow_pipeline_;
                    if (const SurfaceVertexStage* vx = b.first->material.vertex_stage)
                        if (const auto custom = surface_shadow_pipeline(*vx, b.first->skinned, cut)) want = custom;
                    if (!(bound == want)) {
                        ctx.pass->bind_pipeline(want);
                        if (b.first->skinned) ctx.pass->bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                        bound = want;
                    }
                    if (b.first->material.vertex_stage) {
                        push_vertex_material(ctx.cmd, b.first->material, true);
                        bind_shader_textures(*ctx.pass, b.first->material, false);
                    }
                    if (cut) bind_alpha_test(*ctx.pass, ctx.cmd, b.first->material);
                    if (b.first->skinned) ctx.pass->bind_vertex_buffer(2, b.first->mesh->skin);
                    ctx.pass->bind_vertex_buffer(0, b.first->mesh->vertices);
                    ctx.pass->bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                    ctx.pass->bind_index_buffer(b.first->mesh->indices, rhi::IndexType::U32);
                    draw_mesh_lod(*ctx.pass, *b.first->mesh, b.lod, b.instance_count);
                }
            });
    }

    rhi::TextureDesc hdr_desc = color_desc(hdr_format_, rw, rh);
    const RGTexture hdr = graph.create_texture("hdr_color", hdr_desc);
    rhi::TextureDesc depth_desc = hdr_desc;
    depth_desc.format = depth_format_;
    const bool depth_sampled = screen_occlusion || taa || occlusion || fog || clouds || precip_count > 0 || !particle_draws.empty() ||
                               !gpu_jobs.empty() || !decal_draws.empty() || !water_draws.empty();
    depth_desc.usage = rhi::TextureUsageDepthTarget | (depth_sampled ? rhi::TextureUsageSampled : 0u);
    // With MSAA the forward pass has its own multisampled depth, so the pre-pass provides the
    // single-sample scene depth - needed only when a later pass samples or depth-tests it.
    const bool prepass = user_prepass || (msaa > 1 && depth_sampled);
    if (prepass && !will_prepass) facts_.prepass = true;
    const RGTexture depth = graph.create_texture("scene_depth", depth_desc);
    // ADR-0152: the opaque forward pass renders into multisampled targets resolved into `hdr`;
    // its depth is discarded (the pre-pass `depth` serves every later pass).
    RGTexture hdr_ms{}, depth_ms{};
    if (msaa > 1) {
        rhi::TextureDesc ms = hdr_desc;
        ms.sample_count = msaa;
        ms.usage = rhi::TextureUsageColorTarget;
        hdr_ms = graph.create_texture("hdr_color_msaa", ms);
        ms.format = depth_format_;
        ms.usage = rhi::TextureUsageDepthTarget;
        depth_ms = graph.create_texture("scene_depth_msaa", ms);
    }

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
                    // With MSAA the main pass depth is multisampled and not sampled later, so surface-shaded
                    // draws without an alpha test (terrain) are written here; alpha-tested ones are left out.
                    // Vertex functions move vertices: their depth always comes from the main pass.
                    // Cross-fading draws (ADR-0184) dither in the main pass, so they are left out too.
                    if (d->material.alpha_cutoff > 0.0f || d->material.vertex_stage || (d->material.surface_shader && msaa <= 1) ||
                        d->lod_fade != 0.0f)
                        continue;
                    const rhi::PipelineHandle want = d->skinned ? (d->material.double_sided ? skin_prepass_double_ : skin_prepass_cull_)
                                                                : (d->material.double_sided ? prepass_double_ : prepass_cull_);
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                        bound = want;
                    }
                    if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                    pass.bind_vertex_buffer(0, d->mesh->vertices);
                    pass.bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                    pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                    draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                    stats_.prepass_draws += b.instance_count;
                    ++stats_.prepass_batches;
                }
            });
    }

    if (occlusion) {
        graph.add_pass(
            "occlusion_cull",
            [&](PassBuilder& b) {
                b.read(depth);
                b.no_raster();
                b.side_effect();
            },
            [&](PassContext& ctx) {
                const rhi::BufferHandle hiz_rw[] = {hiz_buffer_};
                {
                    rhi::ComputePass cp = ctx.cmd.begin_compute_pass(hiz_rw);
                    if (!cp.active()) return;
                    const HiZLevel& l0 = hiz_levels[0];
                    const std::array<std::int32_t, 4> params{static_cast<std::int32_t>(rw), static_cast<std::int32_t>(rh), l0.width,
                                                             l0.height};
                    cp.bind_pipeline(hiz_depth_);
                    cp.bind_texture(0, ctx.texture(depth), ctx_.nearest_clamp());
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, params);
                    cp.dispatch(static_cast<std::uint32_t>(l0.width + 7) / 8, static_cast<std::uint32_t>(l0.height + 7) / 8);
                    cp.end();
                }
                // One compute pass per level so each reduction sees the previous level's writes.
                for (std::uint32_t i = 1; i < hiz_level_count; ++i) {
                    rhi::ComputePass cp = ctx.cmd.begin_compute_pass(hiz_rw);
                    if (!cp.active()) return;
                    const HiZLevel& s = hiz_levels[i - 1];
                    const HiZLevel& t = hiz_levels[i];
                    const std::array<std::int32_t, 8> params{s.offset, s.width, s.height, 0, t.offset, t.width, t.height, 0};
                    cp.bind_pipeline(hiz_reduce_);
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, params);
                    cp.dispatch(static_cast<std::uint32_t>(t.width + 7) / 8, static_cast<std::uint32_t>(t.height + 7) / 8);
                    cp.end();
                }
                struct CullUniforms {
                    Mat4 view_proj;
                    Vec4 viewport;
                    std::array<std::int32_t, 4> meta;
                    std::array<HiZLevel, 16> levels;
                } u{};
                static_assert(sizeof(CullUniforms) == 352);
                u.view_proj = view_proj;
                // Depth tolerance against quantisation of the pre-pass depth buffer.
                const float eps = depth_format_ == rhi::Format::D16_UNORM ? 2e-5f : 1e-6f;
                u.viewport = {static_cast<float>(rw), static_cast<float>(rh), eps, static_cast<float>(occl_instance_count)};
                u.meta = {static_cast<std::int32_t>(hiz_level_count), 0, 0, 0};
                u.levels = hiz_levels;
                const rhi::BufferHandle cull_rw[] = {occl_args_, occl_instances_};
                rhi::ComputePass cp = ctx.cmd.begin_compute_pass(cull_rw);
                if (!cp.active()) return;
                cp.bind_pipeline(occlusion_cull_);
                cp.bind_storage_buffer(0, instance_buffer_);
                cp.bind_storage_buffer(1, occl_bounds_);
                cp.bind_storage_buffer(2, hiz_buffer_);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, u);
                cp.dispatch((occl_instance_count + 63) / 64);
                cp.end();
            });
    }

    RGTexture ao;
    if (screen_occlusion) {
        const std::uint32_t aw = S.ssao_half_resolution ? std::max(1u, rw / 2) : rw;
        const std::uint32_t ah = S.ssao_half_resolution ? std::max(1u, rh / 2) : rh;
        // R: ambient occlusion, G: sun contact shadow (ADR-0183).
        const RGTexture ao_raw = graph.create_texture("ssao_raw", color_desc(rhi::Format::RG8_UNORM, aw, ah));
        ao = graph.create_texture("ssao", color_desc(rhi::Format::RG8_UNORM, aw, ah));
        auto ssao_pl = post_pipeline("ssao.frag", rhi::Format::RG8_UNORM);
        if (!ssao_pl) return std::move(ssao_pl).error();
        auto blur_pl = post_pipeline("ssao_blur.frag", rhi::Format::RG8_UNORM);
        if (!blur_pl) return std::move(blur_pl).error();
        SsaoUniforms su{};
        su.inv_proj = inverse(proj);
        su.params = {env_.ao_radius, ssao ? env_.ao_intensity : 0.0f, 0.002f, static_cast<float>(S.ssao_samples)};
        su.screen = inv_size(rw, rh, 0.5f * static_cast<float>(rh) / std::tan(camera_.fov_y * 0.5f),
                             std::max(env_.ao_fade_distance, 1.0f));
        su.proj = proj;
        if (contact_shadows) {
            const Vec3 to_sun = normalize(camera_.view.transform_vector(-normalize(sun_.direction)));
            su.light = Vec4(to_sun, std::min(env_.contact_shadow_length, 10.0f));
            su.contact = {static_cast<float>(S.contact_shadow_steps), std::clamp(env_.contact_shadow_thickness, 0.01f, 10.0f),
                          1.0f, std::max(env_.ao_fade_distance, 1.0f)};
        }
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

    // Opaque geometry, then (with decals) the decal pass, then blended geometry and debug lines.
    // Without decals one forward pass draws both phases.
    auto forward_body = [&](PassContext& ctx, bool opaque_phase, bool blended_phase) {
            rhi::RenderPass& pass = *ctx.pass;
            // Only the opaque forward pass is multisampled (and the blended phase when it shares it).
            const bool ms = opaque_phase && msaa > 1;
            const auto ms_pipe = [&](rhi::PipelineHandle p, bool a2c) { return ms ? multisampled(p, msaa, a2c) : p; };
            const auto a2c_of = [&](const Material& m) {
                return ms && S.alpha_to_coverage && m.alpha_cutoff > 0.0f && m.blend == MaterialBlend::Opaque;
            };
            if (opaque_phase && env_.draw_sky) {
                SkyUniforms sky{};
                sky.inv_view_proj = inverse(view_proj);
                sky.zenith = rgb(env_.sky_background ? env_.background_zenith : env_.sky_zenith);
                sky.horizon = rgb(env_.sky_background ? env_.background_horizon : env_.sky_horizon);
                sky.ground = rgb(env_.sky_background ? env_.background_ground : env_.ground);
                sky.sun_dir = Vec4(-normalize(sun_.direction), hdr_quant);
                sky.sun_color = rgb(sun_.color, sun_.intensity * 4.0f, 0.9995f);
                const bool has_env = env_map && env_map->cube();
                if (has_env) {
                    const float blur = std::isfinite(env_.sky_blur) ? std::clamp(env_.sky_blur, 0.0f, 1.0f) : 0.0f;
                    sky.env = {env_.environment_sun_disc ? 2.0f : 1.0f, blur * static_cast<float>(env_map->mip_count() - 1), env_intensity, env_rotation};
                }
                pass.bind_pipeline(ms_pipe(sky_pipeline_, false));
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, sky);
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, has_env ? env_map->cube() : ctx_.black_cube(), ctx_.linear_clamp());
                pass.draw(3);
            }
            const rhi::TextureHandle shadow_tex = shadow_map.valid() ? ctx.texture(shadow_map) : no_shadow_;
            const rhi::TextureHandle local_shadow_tex = local_shadow_map.valid() ? ctx.texture(local_shadow_map) : no_shadow_;
            const rhi::TextureHandle rain_tex = rain_map.valid() ? ctx.texture(rain_map) : no_shadow_;
            const rhi::TextureHandle ao_tex = ao.valid() ? ctx.texture(ao) : ctx_.white();
            const rhi::TextureHandle lightmap_tex = env_.lightmap ? env_.lightmap : ctx_.black();
            const rhi::TextureHandle lightmap_dir_tex =
                env_.lightmap && env_.lightmap_direction ? env_.lightmap_direction : ctx_.no_light_direction();
            // Lightmapped meshes need a lightmap to sample; without one they draw like any static mesh.
            const auto vertex_variant = [&](const Draw* d) {
                return d->skinned                                       ? kVertexSkinned
                       : (env_.lightmap && d->mesh->lightmapped()) ? kVertexLightmapped
                                                                       : kVertexStatic;
            };
            FrameUniforms frame{view_proj};
            ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
            ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, lighting);
            rhi::PipelineHandle bound{};
            // `indirect`: main batch index whose instance count comes from the occlusion pass.
            auto draw_batch = [&](const Batch& b, rhi::PipelineHandle want, int indirect = -1) {
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
                // blend.z: alpha to coverage (ADR-0152) - the cut-out becomes a coverage ramp.
                mat.blend = {static_cast<float>(m.blend), std::clamp(m.occlusion_strength, 0.0f, 1.0f), a2c_of(m) ? 1.0f : 0.0f,
                             d->lod_fade};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 1, mat);
                if (m.vertex_stage) push_vertex_material(ctx.cmd, m, false);
                auto pick = [](rhi::TextureHandle t, rhi::TextureHandle fallback) { return t ? t : fallback; };
                const rhi::SamplerHandle tex_sampler = m.nearest_filter ? ctx_.nearest_repeat() : material_linear.value();
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, pick(m.base_color_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 1, pick(m.metallic_roughness_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 2, pick(m.normal_texture, ctx_.flat_normal()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 3, pick(m.emissive_texture, ctx_.white()), tex_sampler);
                pass.bind_texture(rhi::ShaderStage::Fragment, 4, shadow_tex, ctx_.shadow_compare());
                pass.bind_texture(rhi::ShaderStage::Fragment, 7, local_shadow_tex, ctx_.shadow_compare());
                pass.bind_texture(rhi::ShaderStage::Fragment, 8, gi_tex, ctx_.linear_clamp());
                pass.bind_texture(rhi::ShaderStage::Fragment, 9, rain_tex, ctx_.shadow_compare());
                pass.bind_texture(rhi::ShaderStage::Fragment, 10, lightmap_tex, ctx_.linear_clamp());
                pass.bind_texture(rhi::ShaderStage::Fragment, 11, lightmap_dir_tex, ctx_.linear_clamp());
                pass.bind_texture(rhi::ShaderStage::Fragment, 5, ao_tex, ctx_.linear_clamp());
                if (m.surface_shader) bind_shader_textures(pass, m, true);
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
                pass.bind_storage_buffer(rhi::ShaderStage::Fragment, 2, fog_grid_buffer());
                pass.bind_vertex_buffer(0, d->mesh->vertices);
                if (vertex_variant(d) == kVertexLightmapped) pass.bind_vertex_buffer(3, d->mesh->lightmap_uv);
                pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                if (indirect >= 0) {
                    // Survivors are packed from the batch's own offset; first_instance stays 0
                    // (SDL_GPU does not require indirect first-instance support).
                    pass.bind_vertex_buffer(1, occl_instances_, b.instance_offset * kInstanceStride);
                    pass.draw_indexed_indirect(occl_args_, static_cast<std::uint32_t>(indirect) *
                                                               static_cast<std::uint32_t>(sizeof(rhi::IndexedIndirectCommand)));
                    ++stats_.occlusion_batches;
                } else {
                    pass.bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                    draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                }
            };
            for (std::size_t bi = 0; opaque_phase && bi < main_batches.size(); ++bi) {
                const Batch& b = main_batches[bi];
                const Draw* d = b.first;
                const std::uint32_t variant = vertex_variant(d);
                rhi::PipelineHandle want = variant == kVertexSkinned       ? (d->material.double_sided ? skin_double_ : skin_cull_)
                                           : variant == kVertexLightmapped ? (d->material.double_sided ? lightmap_double_ : lightmap_cull_)
                                                                           : (d->material.double_sided ? opaque_double_ : opaque_cull_);
                if (d->material.surface_shader || d->material.vertex_stage)
                    if (const auto custom = surface_pipeline(d->material.surface_shader, d->material.vertex_stage, variant,
                                                             d->material.double_sided, MaterialBlend::Opaque))
                        want = custom;
                want = ms_pipe(want, a2c_of(d->material));
                if (want) draw_batch(b, want, occlusion && bi < occl_batches_ ? static_cast<int>(bi) : -1);
            }
            if (!blended_phase) return;
            // Blended geometry after every opaque draw (and the decals), far to near.
            for (const Batch& b : transparent_batches) {
                const Draw* d = b.first;
                rhi::PipelineHandle want = blend_pipeline(d->material.blend, vertex_variant(d), d->material.double_sided);
                if (d->material.surface_shader || d->material.vertex_stage)
                    if (const auto custom = surface_pipeline(d->material.surface_shader, d->material.vertex_stage, vertex_variant(d),
                                                             d->material.double_sided, d->material.blend))
                        want = custom;
                want = ms_pipe(want, false);
                if (want) draw_batch(b, want);
            }
            if (!lines_.empty()) {
                pass.bind_pipeline(ms_pipe(line_pipeline_, false));
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj);
                pass.bind_vertex_buffer(0, line_buffer_);
                pass.draw(static_cast<std::uint32_t>(lines_.size()));
            }
    };
    // Clouds and volumetric fog are composited between the opaque and blended passes (ADR-0119).
    const bool split_forward = !decal_draws.empty() || !water_draws.empty() || fog || clouds;
    graph.add_pass(
        "forward",
        [&](PassBuilder& b) {
            if (shadow_map.valid()) b.read(shadow_map);
            if (local_shadow_map.valid()) b.read(local_shadow_map);
            if (rain_map.valid()) b.read(rain_map);
            if (ao.valid()) b.read(ao);
            if (msaa > 1) {
                b.write_color_resolved(hdr_ms, hdr, rhi::LoadOp::Clear, env_.clear_color);
                b.write_depth(depth_ms, rhi::LoadOp::Clear, 1.0f, false);
            } else {
                b.write_color(hdr, rhi::LoadOp::Clear, env_.clear_color);
                b.write_depth(depth, prepass ? rhi::LoadOp::Load : rhi::LoadOp::Clear, 1.0f);
            }
        },
        [&](PassContext& ctx) { forward_body(ctx, true, !split_forward); });
    if (split_forward && !decal_draws.empty()) {
        const Mat4 inv_view_proj = inverse(view_proj);
        graph.add_pass(
            "decals",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
            },
            [&, inv_view_proj, view_proj, rw, rh](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                rhi::PipelineHandle bound{};
                for (const Decal* d : decal_draws) {
                    const rhi::PipelineHandle want = decal_pipelines_[static_cast<std::size_t>(d->blend)];
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        bound = want;
                    }
                    const DecalFrameUniforms fu{view_proj * d->transform};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, fu);
                    DecalUniforms du{};
                    du.inv_view_proj = inv_view_proj;
                    du.world_to_decal = inverse(d->transform);
                    du.color = d->color.vec();
                    du.uv = d->uv;
                    const float angle = std::isfinite(d->angle_fade) ? std::clamp(d->angle_fade, 0.0f, 1.0f) : 0.0f;
                    du.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), static_cast<float>(d->blend), angle};
                    const float fade = std::isfinite(d->depth_fade) ? std::clamp(d->depth_fade, 0.0f, 1.0f) : 0.0f;
                    du.axis = Vec4(normalize(d->transform.transform_vector({0, 0, -1})), fade);
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, du);
                    pass.bind_texture(rhi::ShaderStage::Fragment, 0, d->texture ? d->texture : ctx_.white(),
                                      d->nearest_filter ? ctx_.nearest_clamp() : ctx_.linear_clamp());
                    pass.bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                    pass.draw(36);
                }
            });
    }
    // Reactive mask (ADR-0143): effects with no motion vectors (particles, water, precipitation)
    // also write how much they cover each pixel, and the TAA resolve trusts the current frame there.
    // Blended meshes opt in per material (Material::responsive, ADR-0187). Custom vertex stages
    // are skipped: the mask pass draws with the built-in vertex functions and would misplace them.
    const auto responsive_draw = [](const Draw* d) {
        return d->material.blend != MaterialBlend::Opaque && d->material.responsive > 0.0f && std::isfinite(d->material.responsive) &&
               !d->material.vertex_stage;
    };
    const auto mesh_variant = [&](const Draw* d) {
        return d->skinned ? kVertexSkinned : (env_.lightmap && d->mesh->lightmapped()) ? kVertexLightmapped : kVertexStatic;
    };
    const bool responsive_meshes = std::any_of(transparent_batches.begin(), transparent_batches.end(),
                                               [&](const Batch& b) { return responsive_draw(b.first); });
    bool reactive = taa && S.reactive_mask && !reactive_failed_ && S.reactive_max > 0.0f &&
                    (!water_draws.empty() || !particle_draws.empty() || precip_count > 0 || responsive_meshes);
    rhi::PipelineHandle reactive_pl[3][2]{}; // [particles, ribbons, water][alpha, additive]
    rhi::PipelineHandle reactive_mesh_pl[kVertexVariants][2]{}; // [variant][double sided], ADR-0187
    if (reactive) {
        const auto use = [&](Result<rhi::PipelineHandle> pl, rhi::PipelineHandle& slot) {
            if (!reactive) return;
            if (!pl) {
                VICTUS_LOG_WARN("render", "TAA reactive mask disabled: {}", pl.error().message);
                reactive_failed_ = true;
                reactive = false;
            } else slot = pl.value();
        };
        for (const ParticleBatch& b : particle_draws) {
            const int kind = b.ribbon ? 1 : 0, add = b.material.blend == ParticleBlend::Additive ? 1 : 0;
            if (!reactive_pl[kind][add]) use(reactive_pipeline(kind, add != 0), reactive_pl[kind][add]);
        }
        if (!water_draws.empty()) use(reactive_pipeline(2, false), reactive_pl[2][0]);
        for (const Batch& b : transparent_batches) {
            if (!responsive_draw(b.first)) continue;
            const std::uint32_t v = mesh_variant(b.first);
            const int ds = b.first->material.double_sided ? 1 : 0;
            if (!reactive_mesh_pl[v][ds]) use(reactive_mesh_pipeline(v, ds != 0), reactive_mesh_pl[v][ds]);
        }
        rhi::PipelineHandle rp{}, rs{};
        if (precip_count > 0) use(precipitation_pipeline(false, true), rp);
        if (precip_count > 0 && splash_count > 0) use(precipitation_pipeline(true, true), rs);
        if (reactive && precip_count > 0) {
            precip_pl = rp;
            if (splash_count > 0) splash_pl = rs;
        }
    }
    RGTexture reactive_tex, reactive_prev;
    bool reactive_written = false;
    {
        const std::uint32_t cur = history_index_ ^ 1u, prev = history_index_;
        reactive_valid_[cur] = false;
        if (reactive && !(reactive_[cur] && reactive_w_[cur] == rw && reactive_h_[cur] == rh)) {
            if (reactive_[cur]) dev.destroy(reactive_[cur]);
            rhi::TextureDesc d = color_desc(rhi::Format::R8_UNORM, rw, rh);
            d.name = cur == 0 ? "reactive_mask_a" : "reactive_mask_b";
            auto t = dev.create_texture(d);
            reactive_[cur] = t ? t.value() : rhi::TextureHandle{};
            reactive_w_[cur] = t ? rw : 0;
            reactive_h_[cur] = t ? rh : 0;
            if (!t) {
                VICTUS_LOG_WARN("render", "TAA reactive mask disabled: {}", t.error().message);
                reactive_failed_ = true;
                reactive = false;
            }
        }
        if (reactive) {
            reactive_tex = graph.import_texture("reactive_mask", reactive_[cur], color_desc(rhi::Format::R8_UNORM, rw, rh));
            reactive_valid_[cur] = true;
        }
        // Last frame's mask, sampled at the reprojected uv by the resolve (any resolution).
        if (taa && history_valid_ && reactive_valid_[prev] && reactive_[prev])
            reactive_prev = graph.import_texture("reactive_mask_prev", reactive_[prev],
                                                 color_desc(rhi::Format::R8_UNORM, reactive_w_[prev], reactive_h_[prev]));
    }
    // Called after write_color(hdr) so the mask is colour target 1; the first writer clears it.
    const auto write_reactive = [&](PassBuilder& b) {
        if (!reactive) return;
        b.write_color(reactive_tex, reactive_written ? rhi::LoadOp::Load : rhi::LoadOp::Clear, {0, 0, 0, 0});
        reactive_written = true;
    };
    stats_.reactive_mask = reactive;
    facts_.reactive_mask = reactive;
    if (!water_draws.empty()) {
        // Refraction source: the opaque scene before any water is drawn.
        const RGTexture water_scene = graph.create_texture("water_scene", color_desc(hdr_format_, rw, rh));
        graph.add_pass(
            "water_scene_copy",
            [&](PassBuilder& b) {
                b.read(hdr);
                b.write_color(water_scene, rhi::LoadOp::Clear);
                b.no_raster();
            },
            [&, water_scene](PassContext& ctx) { ctx.cmd.blit(ctx.texture(hdr), ctx.texture(water_scene), rhi::Filter::Nearest); });
        const EnvironmentMap* wenv = env_.environment_map && env_.environment_map->cube() ? env_.environment_map : nullptr;
        WaterUniforms base{};
        base.inv_view_proj = inverse(view_proj);
        base.sun_dir = lighting.sun_dir;
        base.sun_color = lighting.sun_color;
        base.params = {0, 0, wenv ? 1.0f : 0.0f, wenv ? static_cast<float>(wenv->mip_count() - 1) : 0.0f};
        base.env = {std::isfinite(env_.environment_intensity) ? std::max(env_.environment_intensity, 0.0f) : 1.0f,
                    std::isfinite(env_.environment_rotation) ? env_.environment_rotation : 0.0f, 0, 0};
        base.sky = lighting.sky_color;
        base.horizon = lighting.horizon_color;
        base.ground = lighting.ground_color;
        base.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 0, 0};
        base.fog_color = lighting.fog_color;
        base.fog_params = lighting.fog_params;
        auto frame_of = [&](const WaterSurface& w) {
            WaterFrameUniforms f{};
            f.view_proj = view_proj;
            const float cells = static_cast<float>(w.resolution);
            f.center = Vec4(w.center, cells);
            f.extent = {w.half_extent.x, w.half_extent.y, 2.0f * std::max(w.half_extent.x, w.half_extent.y) / cells, 0};
            for (std::size_t i = 0; i < 4; ++i) {
                const WaterWave& wave = w.waves[i];
                f.wave_a[i] = {wave.direction.x, wave.direction.y, wave.k, wave.phase};
                f.wave_b[i] = {wave.amplitude, wave.q, 0, 0};
            }
            return f;
        };
        graph.add_pass(
            "water",
            [&](PassBuilder& b) {
                b.read(water_scene);
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
                write_reactive(b);
            },
            [&, base, frame_of, wenv, water_scene](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                pass.bind_pipeline(reactive ? reactive_pl[2][0] : water_pipeline_);
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(water_scene), ctx_.linear_clamp());
                pass.bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                pass.bind_texture(rhi::ShaderStage::Fragment, 2, wenv ? wenv->cube() : ctx_.black_cube(), ctx_.linear_clamp());
                for (const WaterSurface* w : water_draws) {
                    const WaterFrameUniforms f = frame_of(*w);
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, f);
                    WaterUniforms u = base;
                    u.camera = Vec4(camera_.position, w->time);
                    u.shallow = {w->shallow_color.r, w->shallow_color.g, w->shallow_color.b, w->clarity};
                    u.deep = {w->deep_color.r, w->deep_color.g, w->deep_color.b, w->foam};
                    u.params.x = w->refraction;
                    u.params.y = w->roughness;
                    u.env.z = w->center.y;
                    for (std::size_t i = 0; i < 4; ++i) {
                        u.wave_a[i] = f.wave_a[i];
                        u.wave_b[i] = f.wave_b[i];
                    }
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, u);
                    pass.draw(6u * w->resolution * w->resolution);
                }
            });
        graph.add_pass(
            "water_depth",
            [&](PassBuilder& b) { b.write_depth(depth, rhi::LoadOp::Load, 1.0f); },
            [&, frame_of](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                pass.bind_pipeline(water_depth_pipeline_);
                for (const WaterSurface* w : water_draws) {
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame_of(*w));
                    pass.draw(6u * w->resolution * w->resolution);
                }
            });
    }
    if (clouds) {
        const RGTexture cloud_tex = graph.create_texture("clouds", color_desc(rhi::Format::RGBA16_FLOAT, cloud_w, cloud_h));
        graph.add_pass(
            "clouds",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(cloud_tex, rhi::LoadOp::DontCare);
            },
            [&, pl = cloud_march_pl](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, cloud_u);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
        graph.add_pass(
            "clouds_apply",
            [&](PassBuilder& b) {
                b.read(cloud_tex);
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
            },
            [&, pl = cloud_apply_pl, cloud_tex](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, cloud_apply_u);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(cloud_tex), ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
    }

    if (fog) {
        const auto* pfog = &fog_u;
        graph.add_pass(
            "fog_inject",
            [&](PassBuilder& b) {
                if (shadow_map.valid()) b.read(shadow_map);
                if (local_shadow_map.valid()) b.read(local_shadow_map);
                b.no_raster();
                b.side_effect();
            },
            [&, pfog](PassContext& ctx) {
                const rhi::BufferHandle inject_rw[] = {fog_froxels_[fog_cur]};
                rhi::ComputePass cp = ctx.cmd.begin_compute_pass(inject_rw);
                if (!cp.active()) return;
                cp.bind_pipeline(fog_inject_);
                cp.bind_texture(0, shadow_map.valid() ? ctx.texture(shadow_map) : no_shadow_, ctx_.shadow_compare());
                cp.bind_texture(1, local_shadow_map.valid() ? ctx.texture(local_shadow_map) : no_shadow_, ctx_.shadow_compare());
                cp.bind_storage_buffer(0, light_buffer_);
                cp.bind_storage_buffer(1, cluster_buffer_);
                cp.bind_storage_buffer(2, fog_froxels_[fog_prev]);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, *pfog);
                cp.dispatch((fog_dims[0] + 3) / 4, (fog_dims[1] + 3) / 4, (fog_dims[2] + 3) / 4);
                cp.end();
                const rhi::BufferHandle integrate_rw[] = {fog_integrated_};
                rhi::ComputePass ip = ctx.cmd.begin_compute_pass(integrate_rw);
                if (!ip.active()) return;
                ip.bind_pipeline(fog_integrate_);
                ip.bind_storage_buffer(0, fog_froxels_[fog_cur]);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, *pfog);
                ip.dispatch((fog_dims[0] + 7) / 8, (fog_dims[1] + 7) / 8);
                ip.end();
            });
        auto apply_pl = post_pipeline("fog_apply.frag", hdr_format_, rhi::BlendMode::PremultipliedAlpha);
        if (!apply_pl) return std::move(apply_pl).error();
        graph.add_pass(
            "fog_apply",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
            },
            [&, pl = apply_pl.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, fog_apply_u);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->bind_storage_buffer(rhi::ShaderStage::Fragment, 0, fog_integrated_);
                ctx.pass->draw(3);
            });
    }

    // Blended geometry after the cloud and fog composites (ADR-0119): those are resolved by the
    // opaque depth, and each blended fragment fetches the fog in front of it instead.
    if (split_forward) {
        graph.add_pass(
            "forward_blended",
            [&](PassBuilder& b) {
                if (shadow_map.valid()) b.read(shadow_map);
                if (local_shadow_map.valid()) b.read(local_shadow_map);
                if (rain_map.valid()) b.read(rain_map);
                if (ao.valid()) b.read(ao);
                b.write_color(hdr, rhi::LoadOp::Load);
                b.write_depth(depth, rhi::LoadOp::Load, 1.0f);
            },
            [&](PassContext& ctx) { forward_body(ctx, false, true); });
    }

    // Responsive blended meshes (ADR-0187) write the reactive mask over their coverage, after all
    // blended geometry; depth-tested in the shader against the opaque depth.
    if (reactive && responsive_meshes) {
        graph.add_pass(
            "reactive_meshes",
            [&](PassBuilder& b) {
                b.read(depth);
                write_reactive(b);
            },
            [&](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                FrameUniforms frame{view_proj};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
                const rhi::TextureHandle depth_tex = ctx.texture(depth);
                rhi::PipelineHandle bound{};
                for (const Batch& b : transparent_batches) {
                    const Draw* d = b.first;
                    if (!responsive_draw(d)) continue;
                    const std::uint32_t v = mesh_variant(d);
                    const rhi::PipelineHandle want = reactive_mesh_pl[v][d->material.double_sided ? 1 : 0];
                    if (!want) continue;
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                        bound = want;
                    }
                    const Material& m = d->material;
                    ReactiveMeshUniforms u{};
                    u.base_color = m.base_color.vec();
                    u.uv_transform = {m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y};
                    u.params = {std::clamp(m.responsive, 0.0f, 1.0f), m.alpha_cutoff, m.vertex_alpha_coverage_mask ? 1.0f : 0.0f,
                                m.blend == MaterialBlend::Additive ? 1.0f : 0.0f};
                    u.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 0.0f, 0.0f};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, u);
                    const rhi::SamplerHandle tex_sampler = m.nearest_filter ? ctx_.nearest_repeat() : material_linear.value();
                    pass.bind_texture(rhi::ShaderStage::Fragment, 0, m.base_color_texture ? m.base_color_texture : ctx_.white(), tex_sampler);
                    pass.bind_texture(rhi::ShaderStage::Fragment, 1, depth_tex, ctx_.nearest_clamp());
                    if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                    pass.bind_vertex_buffer(0, d->mesh->vertices);
                    if (v == kVertexLightmapped) pass.bind_vertex_buffer(3, d->mesh->lightmap_uv);
                    pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                    pass.bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                    draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                    ++stats_.reactive_mesh_draws;
                }
            });
    }

    if (!gpu_jobs.empty() || !sort_jobs.empty()) {
        // Passes per sort: keys, local sort, then for each stage k > 256 the global steps
        // j = k/2 .. 256 and one shared-memory merge, then the gather.
        for (const GpuSortJob& job : sort_jobs) {
            std::uint32_t passes = 3;
            for (std::uint32_t k = 512; k <= job.u.meta[2]; k <<= 1)
                passes += static_cast<std::uint32_t>(std::countr_zero(k)) - 8u + 1u;
            stats_.gpu_particle_sort_passes += passes;
        }
        graph.add_pass(
            "gpu_particles",
            [&](PassBuilder& b) {
                b.read(depth);
                b.no_raster();
                b.side_effect();
            },
            [&](PassContext& ctx) {
                for (const GpuParticleJob& job : gpu_jobs) {
                    const GpuParticleState& st = *job.state;
                    const rhi::BufferHandle rw_buffers[] = {st.state, st.instances, st.trail, st.ribbon};
                    // Spawn, then update in a second compute pass so it sees the spawned slots.
                    for (std::uint32_t mode = job.spawns > 0 ? 0u : 1u; mode < 2; ++mode) {
                        rhi::ComputePass cp = ctx.cmd.begin_compute_pass(rw_buffers);
                        if (!cp.active()) return;
                        ParticleSimUniforms u = job.u;
                        u.meta[0] = mode;
                        u.meta[1] = mode == 0 ? job.spawns : st.capacity;
                        cp.bind_pipeline(particle_sim_);
                        cp.bind_texture(0, ctx.texture(depth), ctx_.nearest_clamp());
                        cp.bind_storage_buffer(0, st.spawns);
                        ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, u);
                        cp.dispatch((u.meta[1] + 63) / 64);
                        cp.end();
                    }
                }
                // Depth sort after the simulation wrote this frame's instances.
                for (const GpuSortJob& job : sort_jobs) {
                    const GpuParticleState& st = *job.state;
                    const rhi::BufferHandle rw_buffers[] = {st.pairs, st.sorted};
                    const std::uint32_t n = job.u.meta[2];
                    const auto run = [&](std::uint32_t mode, std::uint32_t k, std::uint32_t j, std::uint32_t threads) {
                        rhi::ComputePass cp = ctx.cmd.begin_compute_pass(rw_buffers);
                        if (!cp.active()) return false;
                        ParticleSortUniforms u = job.u;
                        u.meta[0] = mode;
                        u.step = {k, j, 0u, 0u};
                        cp.bind_pipeline(particle_sort_);
                        cp.bind_storage_buffer(0, st.instances);
                        ctx.cmd.push_uniforms(rhi::ShaderStage::Compute, 0, u);
                        cp.dispatch((threads + 255) / 256);
                        cp.end();
                        return true;
                    };
                    if (!run(0, 0, 0, n) || !run(1, 0, 0, n)) return;
                    for (std::uint32_t k = 512; k <= n; k <<= 1) {
                        for (std::uint32_t j = k >> 1; j >= 256; j >>= 1)
                            if (!run(2, k, j, n)) return;
                        if (!run(3, k, 0, n)) return;
                    }
                    if (!run(4, 0, 0, job.u.meta[1])) return;
                }
            });
    }

    if (!particle_draws.empty()) {
        ParticleFrameUniforms pf{};
        pf.view_proj = view_proj;
        const Mat4 cam_world = inverse(camera_.view);
        pf.right = Vec4(normalize(cam_world.transform_vector({1, 0, 0})), 0);
        pf.up = Vec4(normalize(cam_world.transform_vector({0, 1, 0})), 0);
        pf.eye = Vec4(camera_.position, 1);
        graph.add_pass(
            "particles",
            [&](PassBuilder& b) {
                b.read(depth);
                b.write_color(hdr, rhi::LoadOp::Load);
                write_reactive(b);
            },
            [&, pf, proj, rw, rh](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, pf);
                if (particle_buffer_) pass.bind_vertex_buffer(0, particle_buffer_);
                rhi::PipelineHandle bound{};
                for (const ParticleBatch& b : particle_draws) {
                    const bool add = b.material.blend == ParticleBlend::Additive;
                    const rhi::PipelineHandle want =
                        reactive ? reactive_pl[b.ribbon ? 1 : 0][add ? 1 : 0]
                                 : b.ribbon ? (add ? ribbon_additive_ : ribbon_alpha_) : (add ? particle_additive_ : particle_alpha_);
                    if (!(bound == want)) {
                        pass.bind_pipeline(want);
                        bound = want;
                    }
                    ParticleUniforms pu{};
                    pu.proj = {proj[3].z, proj[2].z, 0, 0};
                    pu.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 1.0f / std::max(b.material.softness, 1e-3f),
                                 add ? 1.0f : 0.0f};
                    pu.fog_color = lighting.fog_color;
                    pu.fog_params = lighting.fog_params;
                    pu.view = {1.0f / proj[0].x, 1.0f / proj[1].y, 0, 0};
                    pu.volumetric = lighting.volumetric;
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, pu);
                    pass.bind_storage_buffer(rhi::ShaderStage::Fragment, 0, fog_grid_buffer());
                    pass.bind_texture(rhi::ShaderStage::Fragment, 0, b.material.texture ? b.material.texture : ctx_.soft_particle(),
                                      ctx_.linear_clamp());
                    pass.bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(depth), ctx_.nearest_clamp());
                    if (b.buffer) pass.bind_vertex_buffer(0, b.buffer);
                    else pass.bind_vertex_buffer(0, particle_buffer_, b.offset * static_cast<std::uint32_t>(sizeof(ParticleInstance)));
                    if (b.index) {
                        pass.bind_index_buffer(b.index, rhi::IndexType::U32);
                        pass.draw_indexed(b.count, 1);
                    } else if (b.ribbon) pass.draw(b.count, 1);
                    else pass.draw(6, b.count);
                }
            });
    }

    if (precip_count > 0) {
        graph.add_pass(
            "precipitation",
            [&](PassBuilder& b) {
                b.read(depth);
                if (rain_map.valid()) b.read(rain_map);
                b.write_color(hdr, rhi::LoadOp::Load);
                write_reactive(b);
            },
            [&, pl = precip_pl, precip_count, splash_count, splash_pl](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, precip_u);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, precip_fu);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1, rain_map.valid() ? ctx.texture(rain_map) : no_shadow_, ctx_.shadow_compare());
                ctx.pass->bind_storage_buffer(rhi::ShaderStage::Fragment, 0, fog_grid_buffer());
                ctx.pass->draw(6, precip_count);
                if (splash_count > 0) {
                    ctx.pass->bind_pipeline(splash_pl);
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, splash_u);
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, splash_fu);
                    ctx.pass->bind_texture(rhi::ShaderStage::Vertex, 0, ctx.texture(rain_map), ctx_.nearest_clamp());
                    ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(depth), ctx_.nearest_clamp());
                    ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1, ctx.texture(rain_map), ctx_.shadow_compare());
                    ctx.pass->bind_storage_buffer(rhi::ShaderStage::Fragment, 0, fog_grid_buffer());
                    ctx.pass->draw(6, splash_count);
                }
            });
    }

    // TAA resolve into the persistent history; everything after reads the resolved image.
    RGTexture scene_color = hdr;
    std::uint32_t scene_w = rw, scene_h = rh; // size of scene_color
    if (taa) {
        const std::uint32_t cur = history_index_ ^ 1u, prev = history_index_;
        const std::uint32_t hw = upscale ? width : rw, hh = upscale ? height : rh;
        VICTUS_TRY_STATUS(ensure_history(cur, hw, hh));
        if (!history_[prev]) VICTUS_TRY_STATUS(ensure_history(prev, hw, hh)); // steady texture count from frame one
        const bool have_prev = history_valid_ && history_[prev];
        auto taa_pl = post_pipeline(upscale ? "taa_upscale.frag" : "taa.frag", hdr_format_, rhi::BlendMode::Opaque, depth_history_format_);
        if (!taa_pl) return std::move(taa_pl).error();
        TaaUpscaleUniforms tu{};
        tu.reproject = prev_view_proj_ * inverse(view_proj_unjittered);
        const bool object_motion = have_prev && !motion_batches.empty();
        // Clip box width (in standard deviations): the upscaler's 3x3 render-texel neighbourhood spans
        // several output pixels, so a native-width box clips away detail the history reconstructed
        // from earlier jitter positions (ADR-0142: 2.0 measured best, also under camera motion).
        tu.params = {S.taa_blend, have_prev ? 1.0f : 0.0f, upscale ? 2.0f : 1.25f, object_motion ? 1.0f : 0.0f};
        tu.screen = {1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh),
                     1.0f / static_cast<float>(have_prev ? history_w_[prev] : hw),
                     1.0f / static_cast<float>(have_prev ? history_h_[prev] : hh)};
        // Render pixels are y-down; the projection jitter is y-up (NDC).
        tu.jitter = {jitter.x, -jitter.y, static_cast<float>(rw), static_cast<float>(rh)};
        tu.output = {1.0f / static_cast<float>(hw), 1.0f / static_cast<float>(hh), static_cast<float>(hw),
                     static_cast<float>(hh)};
        // y: reactive mask bound, z: current-frame weight at a fully reactive pixel (ADR-0143).
        // w: last frame's mask bound (reprojected).
        tu.quant = {hdr_quant, reactive ? 1.0f : 0.0f, S.reactive_max, reactive_prev.valid() ? 1.0f : 0.0f};
        {
            // ADR-0147: linear depth L = 1 / (a d + b) for the 0..1 perspective depth d, and FSR2's
            // separation tolerance Ksep * Kfov * half width (per unit depth), floored at 0.5 % for R16F
            // storage and depth-buffer precision.
            const float p22 = proj_unjittered[2].z, p23 = proj_unjittered[3].z;
            const float tx = 1.0f / proj_unjittered[0].x, ty = 1.0f / proj_unjittered[1].y;
            const float kfov = std::sqrt(1.0f + tx * tx + ty * ty);
            const float tolerance = std::max(1.37e-5f * kfov * 0.5f * static_cast<float>(hw), 0.005f);
            // The coefficients are set on every frame, also without a history: this frame's linear
            // depth is next frame's comparison.
            const bool linear = depth_history_format_ != rhi::Format::RGBA8_UNORM && std::abs(p23) > 1e-12f &&
                                proj_unjittered[2].w < 0.0f;
            const bool usable = linear && have_prev && depth_history_[prev];
            const float strength = usable ? S.taa_disocclusion : 0.0f;
            tu.depth_clip = {strength, linear ? 1.0f / p23 : 0.0f, linear ? p22 / p23 : 1.0f, tolerance};
            stats_.taa_disocclusion = strength > 0.0f;
            // ADR-0148: a lock lasts 8 frames unless the ridge is seen again (FSR2: one jitter period;
            // the upscaler's longer periods, 32 frames at half resolution, held vanished wires too long).
            const bool lockable = have_prev && depth_history_[prev] && depth_history_format_ != rhi::Format::RGBA8_UNORM;
            const float lock = lockable ? S.taa_thin_features : 0.0f;
            tu.lock = {lock, 1.0f / 8.0f, 1.05f, 0.0f};
            stats_.taa_thin_features = lock > 0.0f;
        }
        scene_w = hw;
        scene_h = hh;
        const RGTexture resolved = graph.import_texture("taa_resolved", history_[cur], color_desc(hdr_format_, hw, hh));
        const RGTexture depth_resolved =
            graph.import_texture("taa_depth_history", depth_history_[cur], color_desc(depth_history_format_, hw, hh));
        RGTexture motion;
        if (object_motion) {
            rhi::TextureDesc md = color_desc(rhi::Format::RG16_FLOAT, rw, rh);
            md.name = "motion_vectors";
            motion = graph.create_texture("motion_vectors", md);
            graph.add_pass(
                "motion_vectors",
                [&](PassBuilder& b) {
                    b.write_color(motion, rhi::LoadOp::Clear, {0, 0, 0, 0});
                    b.write_depth(depth, rhi::LoadOp::Load, 1.0f);
                },
                [this, &motion_batches, view_proj](PassContext& ctx) {
                    rhi::RenderPass& pass = *ctx.pass;
                    const MotionUniforms mu{view_proj, prev_view_proj_};
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, mu);
                    rhi::PipelineHandle bound{};
                    for (const auto& b : motion_batches) {
                        const Draw* d = b.first;
                        const rhi::PipelineHandle want = d->skinned ? (d->material.double_sided ? skin_motion_double_ : skin_motion_cull_)
                                                                    : (d->material.double_sided ? motion_double_ : motion_cull_);
                        if (!(bound == want)) {
                            pass.bind_pipeline(want);
                            if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                            bound = want;
                        }
                        if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                        pass.bind_vertex_buffer(0, d->mesh->vertices);
                        pass.bind_vertex_buffer(1, motion_buffer_, b.instance_offset * static_cast<std::uint32_t>(sizeof(MotionInstance)));
                        pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                        draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                    }
                });
        }
        RGTexture previous, depth_previous;
        if (have_prev)
            previous = graph.import_texture("taa_history", history_[prev],
                                            color_desc(hdr_format_, history_w_[prev], history_h_[prev]));
        if (tu.depth_clip.x > 0.0f || tu.lock.x > 0.0f)
            depth_previous = graph.import_texture("taa_depth_history_prev", depth_history_[prev],
                                                  color_desc(depth_history_format_, history_w_[prev], history_h_[prev]));
        graph.add_pass(
            "taa",
            [&](PassBuilder& b) {
                b.read(hdr);
                b.read(depth);
                if (previous.valid()) b.read(previous);
                if (motion.valid()) b.read(motion);
                if (reactive_tex.valid()) b.read(reactive_tex);
                if (reactive_prev.valid()) b.read(reactive_prev);
                if (depth_previous.valid()) b.read(depth_previous);
                b.write_color(resolved, rhi::LoadOp::DontCare);
                b.write_color(depth_resolved, rhi::LoadOp::DontCare);
            },
            [this, hdr, depth, previous, motion, reactive_tex, reactive_prev, depth_previous, tu, upscale,
             pl = taa_pl.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                if (upscale) ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, tu);
                else
                    ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0,
                                          TaaUniforms{tu.reproject, tu.params, tu.screen, tu.quant, tu.depth_clip, tu.lock});
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(hdr), ctx_.nearest_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 1,
                                       previous.valid() ? ctx.texture(previous) : ctx.texture(hdr), ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 2, ctx.texture(depth), ctx_.nearest_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 3, motion.valid() ? ctx.texture(motion) : ctx.texture(hdr),
                                       ctx_.nearest_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 4, reactive_tex.valid() ? ctx.texture(reactive_tex) : ctx_.black(),
                                       ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 5, reactive_prev.valid() ? ctx.texture(reactive_prev) : ctx_.black(),
                                       ctx_.linear_clamp());
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 6, depth_previous.valid() ? ctx.texture(depth_previous) : ctx_.black(),
                                       ctx_.nearest_clamp());
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
            chain.push_back(graph.create_texture(std::format("bloom_{}", levels), color_desc(hdr_format_, bw, bh)));
            sizes.emplace_back(bw, bh);
            ++levels;
        }
        if (levels > 0) {
            auto down_pl = post_pipeline("bloom_down.frag", hdr_format_);
            if (!down_pl) return std::move(down_pl).error();
            auto up_pl = post_pipeline("bloom_up.frag", hdr_format_, rhi::BlendMode::Additive);
            if (!up_pl) return std::move(up_pl).error();
            for (std::uint32_t i = 0; i < levels; ++i) {
                const RGTexture src = i == 0 ? scene_color : chain[i - 1];
                const auto [sw, sh] = i == 0 ? std::pair{scene_w, scene_h} : sizes[i - 1];
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

    // Tonemap (+ grading) into the target, or into an LDR buffer that FXAA resolves. ADR-0066: with spatial
    // upscaling (or sharpening) both run at render resolution into sRGB-encoded 8-bit buffers and a final pass
    // resamples and sharpens into the target.
    const bool scaled_down = rw < width || rh < height;
    const bool spatial = S.spatial_upscale && scaled_down && !upscale && !linear_out;
    facts_.spatial_upscale = spatial;
    const bool sharpen_pass = !linear_out && (spatial || S.sharpen > 0.0f);
    if (S.debug_view == DebugView::Overdraw) {
        // Overdraw debug view: the visible opaque, alpha-tested and blended batches are drawn again
        // without depth testing into a persistent RGBA8 counter (one step per fragment), which a
        // fullscreen pass maps to a heat ramp in `out`. The lit passes have no consumer and are culled
        // by the graph, so their stats stay at zero this frame.
        if (!overdraw_tex_ || overdraw_w_ != rw || overdraw_h_ != rh) {
            if (overdraw_tex_) dev.destroy(overdraw_tex_);
            overdraw_tex_ = {};
            rhi::TextureDesc od = color_desc(rhi::Format::RGBA8_UNORM, rw, rh);
            od.name = "overdraw_count";
            auto made = dev.create_texture(od);
            if (!made) return std::move(made).error();
            overdraw_tex_ = made.value();
            overdraw_w_ = rw;
            overdraw_h_ = rh;
        }
        const RGTexture counts = graph.import_texture("overdraw_count", overdraw_tex_, color_desc(rhi::Format::RGBA8_UNORM, rw, rh));
        graph.add_pass(
            "overdraw_count", [&](PassBuilder& b) { b.write_color(counts, rhi::LoadOp::Clear, math::Color{0, 0, 0, 0}); },
            [&](PassContext& ctx) {
                rhi::RenderPass& pass = *ctx.pass;
                FrameUniforms frame{view_proj};
                ctx.cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, frame);
                rhi::PipelineHandle bound{};
                for (const std::vector<Batch>* list : {&main_batches, &transparent_batches})
                    for (const Batch& b : *list) {
                        const Draw* d = b.first;
                        const rhi::PipelineHandle want = overdraw_[d->skinned ? 1 : 0][d->material.double_sided ? 1 : 0];
                        if (!(bound == want)) {
                            pass.bind_pipeline(want);
                            if (d->skinned) pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, palette_buffer_);
                            bound = want;
                        }
                        if (d->skinned) pass.bind_vertex_buffer(2, d->mesh->skin);
                        pass.bind_vertex_buffer(0, d->mesh->vertices);
                        pass.bind_vertex_buffer(1, b.buffer ? b.buffer : instance_buffer_, b.instance_offset * kInstanceStride);
                        pass.bind_index_buffer(d->mesh->indices, rhi::IndexType::U32);
                        draw_mesh_lod(pass, *d->mesh, d->lod, b.instance_count);
                    }
            });
        auto heat = post_pipeline("overdraw_heat.frag", target_format);
        if (!heat) return std::move(heat).error();
        const Vec4 heat_params{rhi::is_srgb(target_format) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        graph.add_pass(
            "overdraw_heat",
            [&](PassBuilder& b) {
                b.read(counts);
                b.write_color(out, rhi::LoadOp::DontCare);
            },
            [this, counts, heat_params, pl = heat.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, heat_params);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(counts), ctx_.nearest_clamp());
                ctx.pass->draw(3);
            });
        stage.reset();
        VICTUS_TRY_STATUS(graph.compile());
        VICTUS_TRY_STATUS(graph.execute(dev, cmd, pool_));
        graph_info_ = graph.info();
        overdraw_valid_ = true;
        // Temporal state was not advanced; start TAA and eye adaptation fresh when the view returns.
        history_valid_ = false;
        adaptation_valid_ = false;
        return {};
    }
    overdraw_valid_ = false;

    const std::uint32_t pw = spatial ? rw : width, ph = spatial ? rh : height; // tonemap / FXAA size
    const bool ldr_buffer = fxaa || sharpen_pass;
    // The upscale pass reads an sRGB texture so its filtering and sharpening run in linear light (filtering
    // sRGB-encoded values darkens fine detail). FXAA still reads sRGB-encoded colour with luma in alpha.
    const rhi::Format ldr_format = sharpen_pass ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
    const rhi::Format tonemap_format = ldr_buffer ? ldr_format : target_format;
    auto tonemap = post_pipeline("tonemap.frag", tonemap_format);
    if (!tonemap) return std::move(tonemap).error();
    const RGTexture ldr = ldr_buffer ? graph.create_texture("ldr_color", color_desc(ldr_format, pw, ph)) : out;
    PostUniforms post{};
    const Tonemapper op = env_.tonemap ? env_.tonemapper : Tonemapper::None;
    post.params = {env_.exposure, env_.vignette, rhi::is_srgb(tonemap_format) ? 0.0f : 1.0f, static_cast<float>(op)};
    post.params2 = {bloom_result.valid() ? env_.bloom_intensity : 0.0f, exposure_now.valid() ? 1.0f : 0.0f,
                    std::exp2(env_.exposure_compensation), env_.saturation};
    // The luma-in-alpha path always writes sRGB-encoded colour, which is what FXAA and the sharpen pass read.
    post.params3 = {env_.contrast, (fxaa && !sharpen_pass) ? 1.0f : 0.0f, linear_out ? 1.0f : 0.0f, 0.0f};
    post.params4 = {env_.lens_distortion, env_.chromatic_aberration, env_.film_grain,
                    static_cast<float>(post_frame_ % 4096u)};
    if (capture_mode_) { // scene-referred radiance: the viewer's exposure applies later
        post.params4 = {};
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
    // ADR-0066 order: tonemap (render res) -> upscale + sharpen (output res) -> FXAA (output res), so FXAA
    // smooths the final edges and never blurs texture detail before it is resampled.
    RGTexture aa_src = ldr;
    if (sharpen_pass) {
        const rhi::Format up_format = fxaa ? rhi::Format::RGBA8_UNORM : target_format;
        auto up = post_pipeline("upscale_sharpen.frag", up_format);
        if (!up) return std::move(up).error();
        const RGTexture up_out = fxaa ? graph.create_texture("upscaled_color", color_desc(up_format, width, height)) : out;
        // z: 0 linear out (sRGB target), 1 encode to sRGB, 2 encode with luma in alpha (FXAA input).
        struct {
            Vec4 params, size;
        } uu{inv_size(pw, ph, fxaa ? 2.0f : (rhi::is_srgb(target_format) ? 0.0f : 1.0f), S.sharpen),
             Vec4(static_cast<float>(pw), static_cast<float>(ph), spatial ? 0.0f : 1.0f, 0.0f)};
        graph.add_pass(
            "upscale_sharpen",
            [&](PassBuilder& b) {
                b.read(ldr);
                b.write_color(up_out, rhi::LoadOp::DontCare);
            },
            [this, ldr, uu, pl = up.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, uu);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(ldr), ctx_.linear_clamp());
                ctx.pass->draw(3);
            });
        aa_src = up_out;
    }
    if (fxaa) {
        auto fx = post_pipeline("fxaa.frag", target_format);
        if (!fx) return std::move(fx).error();
        PostUniforms fu{inv_size(width, height, rhi::is_srgb(target_format) ? 1.0f : 0.0f, 0.75f), {}, {}};
        graph.add_pass(
            "fxaa",
            [&](PassBuilder& b) {
                b.read(aa_src);
                b.write_color(out, rhi::LoadOp::DontCare);
            },
            [this, aa_src, fu, pl = fx.value()](PassContext& ctx) {
                ctx.pass->bind_pipeline(pl);
                ctx.cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, fu.params);
                ctx.pass->bind_texture(rhi::ShaderStage::Fragment, 0, ctx.texture(aa_src), ctx_.linear_clamp());
                ctx.pass->draw(3);
            });
    }
    stats_.spatial_upscale = spatial;
    stats_.sharpened = sharpen_pass;

    stage.reset();
    VICTUS_TRY_STATUS(graph.compile());
    VICTUS_TRY_STATUS(graph.execute(dev, cmd, pool_));
    graph_info_ = graph.info();
    if (taa) {
        history_index_ ^= 1u;
        history_valid_ = true;
        prev_view_proj_ = view_proj_unjittered;
        ++frame_index_;
    }
    ++post_frame_;
    if (clouds) ++cloud_frame_;
    if (fog) {
        fog_index_ ^= 1u;
        fog_history_valid_ = true;
        fog_prev_view_proj_ = view_proj_unjittered;
        fog_prev_position_ = camera_.position;
        fog_prev_forward_ = camera_.forward();
        fog_dims_ = fog_dims;
        fog_far_ = fog_far;
        ++fog_frame_;
    } else {
        fog_history_valid_ = false;
    }
    motion_prev_.swap(motion_next_);
    motion_joints_.swap(motion_joints_next_);
    if (auto_exposure) {
        exposure_index_ ^= 1u;
        adaptation_valid_ = true;
    } else {
        adaptation_valid_ = false;
    }
    facts_.cascades = cascade_count_;
    facts_.fog_x = fog ? fog_dims[0] : 0;
    facts_.fog_y = fog ? fog_dims[1] : 0;
    facts_.fog_slices = fog ? fog_dims[2] : 0;
    facts_.occlusion_tested = stats_.occlusion_tested;
    facts_.local_shadow_lights = stats_.local_shadow_lights;
    facts_.local_shadow_tiles = stats_.local_shadow_tiles;
    facts_.visible_draws = stats_.visible;
    facts_.lod_draws = stats_.lod_draws;
    facts_.lod_held = stats_.lod_held;
    facts_.moving_draws = stats_.motion_draws;
    facts_.main_batches = stats_.main_batches;
    facts_.instanced_draws = stats_.visible;
    facts_.decals_submitted = static_cast<std::uint32_t>(decals_.size());
    facts_.decals_drawn = stats_.decals;
    facts_.decal_budget = settings_.max_decals;
    facts_.water_submitted = static_cast<std::uint32_t>(waters_.size());
    facts_.water_drawn = stats_.water_surfaces;
    facts_.water_budget = settings_.max_water_surfaces;
    facts_.particle_budget = settings_.max_particles;
    facts_.particle_batches_submitted = static_cast<std::uint32_t>(particle_batches_.size());
    facts_.particle_batches_drawn = stats_.particle_batches;
    facts_.particles_drawn = stats_.particles;
    facts_.ribbon_vertices = stats_.ribbon_vertices;
    facts_.gpu_emitters_submitted = static_cast<std::uint32_t>(gpu_emitters_.size());
    facts_.gpu_emitters_simulated = stats_.gpu_particle_emitters;
    facts_.gpu_particles = stats_.gpu_particles;
    facts_.gpu_emitters_sorted = stats_.gpu_particle_sorted_emitters;
    facts_.gpu_emitters_trailed = stats_.gpu_particle_trail_emitters;
    facts_.gpu_trails_reduced = stats_.gpu_particle_trails_reduced;
    facts_.probes_requested = probe_requests_;
    facts_.probes_unbaked = probe_unbaked_;
    facts_.probes_over_budget = probe_over_budget_;
    facts_.probes_used = static_cast<std::uint32_t>(probes_.size());
    facts_.probe_budget = static_cast<std::uint32_t>(kMaxReflectionProbes);
    facts_.probe_draws = stats_.probe_draws;
    path_ = describe_render_path(S, facts_);
    return {};
}

} // namespace victus::render
