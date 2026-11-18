// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/scene_rendering.hpp"
#include "victus/render/reflection_capture.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/particles/particles.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/gameplay/gameplay.hpp"

#include <algorithm>
#include <format>

namespace victus::runtime {


using namespace victus::scene;

GpuAssetCache::~GpuAssetCache() { clear(); }

void GpuAssetCache::clear() {
    for (auto& [id, tex] : textures_) ctx_.device().destroy(tex);
    textures_.clear();
    nearest_.clear();
    environments_.clear();
    environment_errors_.clear();
    atmosphere_.reset();
    probes_.clear();
    irradiance_.clear();
    probe_renderer_.reset();
    for (auto& [id, model] : models_) {
        for (auto& m : model->meshes) ctx_.destroy(m);
        for (auto& t : model->textures) ctx_.device().destroy(t);
    }
    models_.clear();
    for (auto& [name, mesh] : primitives_) ctx_.destroy(mesh);
    primitives_.clear();
    for (auto& [id, entry] : procedural_)
        if (entry.mesh.valid()) ctx_.destroy(entry.mesh);
    procedural_.clear();
    evict_shaders();
    if (terrain_shader_ && *terrain_shader_) ctx_.device().destroy(**terrain_shader_);
    terrain_shader_.reset();
}

void GpuAssetCache::evict(const Uuid& uuid) {
    // Materials hold texture handles: any eviction may invalidate them, and they are cheap to rebuild.
    materials_.clear();
    nearest_.erase(uuid);
    environments_.erase(uuid);
    environment_errors_.erase(uuid);
    if (const auto it = textures_.find(uuid); it != textures_.end()) {
        ctx_.device().destroy(it->second);
        textures_.erase(it);
    }
    if (const auto it = models_.find(uuid); it != models_.end()) {
        for (auto& m : it->second->meshes) ctx_.destroy(m);
        for (auto& t : it->second->textures) ctx_.device().destroy(t);
        models_.erase(it);
    }
}

Result<rhi::TextureHandle> upload_cooked_texture(render::GpuContext& ctx, const assets::CookedTexture& texture, bool allow_compressed,
                                                 std::optional<assets::TextureCodec>* chosen) {
    if (chosen) chosen->reset();
    render::TextureOptions opt;
    opt.srgb = texture.srgb;
    opt.name = texture.name;
    if (allow_compressed && !texture.mips.empty()) {
        for (const auto& variant : texture.compressed) {
            const rhi::Format format = assets::codec_format(variant.codec, texture.srgb);
            if (variant.levels.size() != texture.mips.size() || !ctx.device().supports_format(format, rhi::TextureUsageSampled)) continue;
            auto tex = ctx.create_compressed_texture(format, texture.mips[0].width, texture.mips[0].height, variant.levels, opt);
            if (!tex) continue; // fall through to the next variant / RGBA8
            if (chosen) *chosen = variant.codec;
            return tex;
        }
    }
    return ctx.create_texture_mips(texture.mips, opt);
}

Result<rhi::TextureHandle> GpuAssetCache::texture(const Uuid& uuid) {
    if (const auto it = textures_.find(uuid); it != textures_.end()) return it->second;
    if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
    auto cooked = db_->load_texture(uuid);
    if (!cooked) return std::move(cooked).error();
    std::optional<assets::TextureCodec> codec;
    auto tex = upload_cooked_texture(ctx_, *cooked, allow_compressed_, &codec);
    if (!tex) return tex;
    if (codec) ++compressed_uploads_;
    textures_.emplace(uuid, *tex);
    return tex;
}

Result<const render::EnvironmentMap*> GpuAssetCache::environment(const Uuid& uuid) {
    if (const auto it = environments_.find(uuid); it != environments_.end())
        return static_cast<const render::EnvironmentMap*>(it->second.get());
    if (const auto it = environment_errors_.find(uuid); it != environment_errors_.end()) return it->second;
    auto build = [&]() -> Result<std::unique_ptr<render::EnvironmentMap>> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        auto cooked = db_->load_texture(uuid);
        if (!cooked) return std::move(cooked).error();
        if (!cooked->hdr)
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{}: environment maps need an HDR (.hdr) texture", cooked->name));
        render::SphericalHarmonics9 sh;
        sh.coefficients = cooked->sh;
        return render::EnvironmentMap::create(ctx_, cooked->hdr_image(), sh, env_bake_);
    };
    auto env = build();
    if (!env) {
        VICTUS_LOG_ERROR("runtime", "environment map {}: {}", uuid.to_string(), env.error().message);
        environment_errors_.emplace(uuid, env.error());
        return std::move(env).error();
    }
    const render::EnvironmentMap* ptr = env->get();
    environments_.emplace(uuid, std::move(*env));
    return ptr;
}

Result<const render::EnvironmentMap*> GpuAssetCache::atmosphere(const render::AtmosphereSettings& s) {
    if (atmosphere_ && atmosphere_key_.same_as(s)) return static_cast<const render::EnvironmentMap*>(atmosphere_.get());
    VICTUS_PROFILE_SCOPE("GpuAssetCache::atmosphere");
    // Bake settings scale the cube; the sky itself is smooth, so a 64-128 px cube is plenty.
    render::EnvironmentBakeSettings bake = env_bake_;
    bake.cube_size = std::min(bake.cube_size, 128u);
    auto env = render::EnvironmentMap::create(ctx_, render::render_atmosphere(s), bake);
    if (!env) return std::move(env).error();
    atmosphere_ = std::move(*env); // the previous cube is released once in-flight frames finish
    atmosphere_key_ = s;
    ++atmosphere_bakes_;
    return static_cast<const render::EnvironmentMap*>(atmosphere_.get());
}

Result<render::SceneRenderer*> GpuAssetCache::probe_renderer() {
    if (!probe_renderer_) {
        auto r = render::SceneRenderer::create(ctx_);
        if (!r) return std::move(r).error();
        probe_renderer_ = std::move(*r);
        // Captures are small and viewed through rough reflections: skip SSAO, keep shadows.
        render::RenderSettings s = render::RenderSettings::preset(render::QualityTier::Medium);
        s.ssao = false;
        s.shadow_cascades = 1;
        s.shadow_resolution = 1024;
        probe_renderer_->set_settings(s);
    }
    return probe_renderer_.get();
}

bool GpuAssetCache::texture_nearest(const Uuid& uuid) {
    if (uuid.is_nil() || !db_) return false;
    if (const auto it = nearest_.find(uuid); it != nearest_.end()) return it->second;
    const assets::AssetRecord* rec = db_->find(uuid);
    const bool nearest = rec && rec->kind == assets::AssetKind::Texture &&
                         assets::TextureImportSettings::from_json(rec->settings).nearest;
    nearest_.emplace(uuid, nearest);
    return nearest;
}

Result<render::Material> GpuAssetCache::material(const Uuid& uuid) {
    if (const auto it = materials_.find(uuid); it != materials_.end()) return it->second;
    auto build = [&]() -> Result<render::Material> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        auto src = db_->resolve_material(uuid); // instances inherit from their parents
        if (!src) return std::move(src).error();
        render::Material m;
        m.base_color = src->base_color;
        m.emissive = src->emissive;
        m.emissive_strength = src->emissive_strength;
        m.metallic = src->metallic;
        m.roughness = src->roughness;
        m.normal_scale = src->normal_scale;
        m.alpha_cutoff = src->alpha_cutoff;
        m.double_sided = src->double_sided;
        m.unlit = src->unlit;
        m.uv_scale = src->uv_scale;
        m.uv_offset = src->uv_offset;
        // Nearest when the material asks for it or its base colour texture is imported as pixel art.
        m.nearest_filter = src->filter == assets::TextureFilter::Nearest || texture_nearest(src->base_color_texture);
        m.shader_params = src->shader_params;
        if (!src->shader.is_nil()) {
            if (auto sh = surface_shader(src->shader); sh) m.surface_shader = *sh;
            // else: already logged by surface_shader(); the material renders with built-in PBR
        }
        m.vertex_alpha_coverage_mask = src->vertex_alpha_coverage_mask;
        m.coverage_response = src->coverage_response;
        m.blend = static_cast<render::MaterialBlend>(src->blend);
        rhi::TextureHandle* slots[4] = {&m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture, &m.emissive_texture};
        const auto refs = src->textures();
        for (std::size_t i = 0; i < 4; ++i) {
            if (refs[i]->is_nil()) continue;
            const assets::AssetRecord* rec = db_->find(*refs[i]);
            if (!rec || rec->kind != assets::AssetKind::Texture) {
                VICTUS_LOG_WARN("assets", "material {}: {} texture {} is missing", uuid.to_string(),
                                assets::kMaterialTextureSlotNames[i], refs[i]->to_string());
                continue;
            }
            if (auto t = texture(*refs[i]); t) *slots[i] = *t;
            else VICTUS_LOG_WARN("assets", "material {}: {}", uuid.to_string(), t.error().message);
        }
        return m;
    };
    auto result = build();
    if (!result) VICTUS_LOG_WARN("assets", "{}", result.error().message);
    return materials_.emplace(uuid, std::move(result)).first->second;
}

namespace {
Result<rhi::ShaderHandle> create_surface_fragment(rhi::Device& device, std::string_view code, const std::string& name) {
    auto compiled = compile_surface_shader(code, name, device.capabilities().backend == rhi::Backend::Metal);
    if (!compiled) return std::move(compiled).error();
    rhi::ShaderDesc d;
    d.name = name;
    d.stage = rhi::ShaderStage::Fragment;
    d.spirv = compiled->spirv;
    d.msl = compiled->msl;
    d.samplers = compiled->counts.samplers;
    d.storage_textures = compiled->counts.storage_textures;
    d.storage_buffers = compiled->counts.storage_buffers;
    d.uniform_buffers = compiled->counts.uniform_buffers;
    return device.create_shader(d);
}
} // namespace

Result<rhi::ShaderHandle> GpuAssetCache::surface_shader(const Uuid& uuid) {
    if (const auto it = shaders_.find(uuid); it != shaders_.end()) return it->second;
    auto build = [&]() -> Result<rhi::ShaderHandle> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        const assets::AssetRecord* rec = db_->find(uuid);
        if (!rec || rec->kind != assets::AssetKind::Shader)
            return make_error(ErrorCode::NotFound, "surface shader " + uuid.to_string() + " is missing");
        const std::string path = rec->path; // records may move on refresh
        auto code = load_surface_shader_code(*db_, uuid);
        if (!code) return std::move(code).error();
        return create_surface_fragment(ctx_.device(), *code, path);
    };
    auto result = build();
    if (!result) VICTUS_LOG_ERROR("assets", "{}", result.error().message);
    return shaders_.emplace(uuid, std::move(result)).first->second;
}

Result<rhi::ShaderHandle> GpuAssetCache::terrain_shader() {
    if (!terrain_shader_) {
        terrain_shader_ = create_surface_fragment(ctx_.device(), terrain::terrain_surface_code(), "terrain");
        if (!*terrain_shader_) VICTUS_LOG_ERROR("assets", "built-in terrain shader: {}", terrain_shader_->error().message);
    }
    return *terrain_shader_;
}

void GpuAssetCache::evict_shaders() {
    materials_.clear(); // they hold the shader handles
    for (auto& [id, sh] : shaders_)
        if (sh) ctx_.device().destroy(*sh);
    shaders_.clear();
}

Result<const GpuModel*> GpuAssetCache::model(const Uuid& uuid) {
    if (const auto it = models_.find(uuid); it != models_.end()) return static_cast<const GpuModel*>(it->second.get());
    if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
    auto cooked = db_->load_model(uuid);
    if (!cooked) return std::move(cooked).error();
    return add_model(uuid, std::move(cooked).value());
}

Result<const GpuModel*> GpuAssetCache::add_model(const Uuid& uuid, assets::CookedModel model) {
    evict(uuid);
    auto gpu = std::make_unique<GpuModel>();
    auto fail = [&](Error e) -> Result<const GpuModel*> {
        for (auto& m : gpu->meshes) ctx_.destroy(m);
        for (auto& t : gpu->textures) ctx_.device().destroy(t);
        return e;
    };
    for (const auto& t : model.textures) {
        std::optional<assets::TextureCodec> codec;
        auto tex = upload_cooked_texture(ctx_, t, allow_compressed_, &codec);
        if (!tex) return fail(std::move(tex).error());
        if (codec) ++compressed_uploads_;
        gpu->textures.push_back(*tex);
    }
    auto tex_or = [&](std::int32_t i) { return i >= 0 ? gpu->textures[static_cast<std::size_t>(i)] : rhi::TextureHandle{}; };
    for (auto& mesh : model.meshes) {
        std::vector<math::Aabb> bounds;
        std::int32_t max_joint = -1;
        if (mesh.data.skinned()) {
            for (std::size_t v = 0; v < mesh.data.skin.size(); ++v) {
                const render::SkinVertex& sv = mesh.data.skin[v];
                for (int k = 0; k < 4; ++k) {
                    if (sv.weights[k] <= 0.0f) continue;
                    const std::uint16_t j = sv.joints[k];
                    if (j >= bounds.size()) bounds.resize(j + 1u);
                    bounds[j].expand(mesh.data.vertices[v].position);
                    max_joint = std::max<std::int32_t>(max_joint, j);
                }
            }
        }
        gpu->joint_bounds.push_back(std::move(bounds));
        gpu->max_joint.push_back(max_joint);
        auto g = ctx_.create_mesh(mesh.data, mesh.name);
        if (!g) return fail(std::move(g).error());
        gpu->meshes.push_back(*g);
        const assets::ModelMaterial& src = model.materials[mesh.material];
        render::Material m;
        m.base_color = src.base_color;
        m.emissive = src.emissive;
        m.emissive_strength = src.emissive_strength;
        m.metallic = src.metallic;
        m.roughness = src.roughness;
        m.normal_scale = src.normal_scale;
        m.alpha_cutoff = src.alpha_cutoff;
        m.double_sided = src.double_sided;
        m.unlit = src.unlit;
        m.base_color_texture = tex_or(src.base_color_texture);
        m.metallic_roughness_texture = tex_or(src.metallic_roughness_texture);
        m.normal_texture = tex_or(src.normal_texture);
        m.emissive_texture = tex_or(src.emissive_texture);
        gpu->mesh_materials.push_back(m);
        // GPU owns the geometry now; keep only counts/bounds-relevant metadata on the CPU side.
        mesh.data.vertices.clear();
        mesh.data.vertices.shrink_to_fit();
        mesh.data.indices.clear();
        mesh.data.indices.shrink_to_fit();
    }
    model.textures.clear();
    for (const auto& n : model.nodes) gpu->skeleton.push_back({.name = n.name, .parent = n.parent, .rest = {n.translation, n.rotation, n.scale}});
    gpu->cpu = std::move(model);
    const GpuModel* raw = gpu.get();
    models_[uuid] = std::move(gpu);
    return raw;
}

const render::GpuMesh* GpuAssetCache::procedural(const scene::ProceduralMesh& mesh) {
    auto [it, inserted] = procedural_.try_emplace(mesh.id);
    ProceduralEntry& entry = it->second;
    entry.last_used = procedural_clock_;
    if (!inserted) return entry.failed ? nullptr : &entry.mesh;
    if (mesh.positions.empty() || mesh.indices.empty()) {
        entry.failed = true;
        return nullptr;
    }
    render::MeshData data;
    data.vertices.resize(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        render::Vertex3D& v = data.vertices[i];
        v.position = mesh.positions[i];
        if (i < mesh.normals.size()) v.normal = mesh.normals[i];
        if (i < mesh.uvs.size()) v.uv = mesh.uvs[i];
        if (i < mesh.colors.size()) v.color = mesh.colors[i];
    }
    data.indices = mesh.indices;
    for (const scene::ProceduralMesh::Lod& lod : mesh.lods) data.lods.push_back({lod.indices, lod.error});
    if (!mesh.uvs.empty()) render::compute_tangents(data);
    auto g = ctx_.create_mesh(data, "procedural");
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "procedural mesh {}: {}", mesh.id, g.error().message);
        entry.failed = true;
        return nullptr;
    }
    entry.mesh = *g;
    return &entry.mesh;
}

void GpuAssetCache::collect_procedural(std::uint32_t keep_frames) {
    ++procedural_clock_;
    for (auto it = procedural_.begin(); it != procedural_.end();) {
        if (it->second.last_used + keep_frames < procedural_clock_) {
            if (it->second.mesh.valid()) ctx_.destroy(it->second.mesh);
            it = procedural_.erase(it);
        } else {
            ++it;
        }
    }
}

const render::GpuMesh* GpuAssetCache::primitive(std::string_view name) {
    const std::string key(name);
    if (const auto it = primitives_.find(key); it != primitives_.end()) return &it->second;
    render::MeshData data;
    if (key == "cube") data = render::make_cube();
    else if (key == "sphere") data = render::make_uv_sphere();
    else if (key == "plane") data = render::make_plane({1, 1});
    else if (key == "cylinder") data = render::make_cylinder();
    else return nullptr;
    // Curved primitives get LODs like imported models (the cube and plane are already minimal).
    if (key == "sphere" || key == "cylinder") render::generate_lods(data);
    render::optimize_mesh(data);
    auto g = ctx_.create_mesh(data, key);
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "primitive '{}': {}", key, g.error().message);
        return nullptr;
    }
    return &primitives_.emplace(key, *g).first->second;
}

static_assert(static_cast<int>(gameplay::EnvironmentComponent::Tonemapper::Reinhard) ==
              static_cast<int>(render::Tonemapper::Reinhard));
static_assert(static_cast<int>(gameplay::EnvironmentComponent::Tonemapper::Neutral) ==
              static_cast<int>(render::Tonemapper::Neutral));

void apply_environment(const gameplay::EnvironmentComponent& e, render::Environment& env, render::DirectionalLight& sun) {
    env.sky_zenith = e.sky_zenith;
    env.sky_horizon = e.sky_horizon;
    env.ground = e.ground;
    env.ambient_intensity = std::max(e.ambient_intensity, 0.0f);
    env.draw_sky = e.draw_sky;
    env.clear_color = e.clear_color;
    env.tonemap = true;
    env.tonemapper = static_cast<render::Tonemapper>(e.tonemapper);
    env.exposure = std::max(e.exposure, 0.0f);
    env.exposure_compensation = e.exposure_compensation;
    env.auto_exposure = e.auto_exposure;
    env.auto_exposure_min_ev = e.auto_exposure_min_ev;
    env.auto_exposure_max_ev = std::max(e.auto_exposure_min_ev, e.auto_exposure_max_ev);
    env.saturation = std::max(e.saturation, 0.0f);
    env.contrast = std::max(e.contrast, 0.0f);
    env.vignette = std::clamp(e.vignette, 0.0f, 1.0f);
    env.bloom_intensity = std::max(e.bloom_intensity, 0.0f);
    env.bloom_threshold = std::max(e.bloom_threshold, 0.0f);
    env.ao_intensity = std::max(e.ao_intensity, 0.0f);
    env.ao_radius = std::max(e.ao_radius, 0.0f);
    sun.shadow_distance = std::max(e.shadow_distance, 1.0f);
    env.fog_mode = static_cast<render::FogMode>(e.fog);
    env.fog_color = e.fog_color;
    env.fog_start = std::max(e.fog_start, 0.0f);
    env.fog_end = std::max(e.fog_end, env.fog_start);
    env.fog_density = std::max(e.fog_density, 0.0f);
    env.fog_max_opacity = std::clamp(e.fog_max_opacity, 0.0f, 1.0f);
    env.coverage_amount = std::clamp(e.coverage_amount, 0.0f, 1.0f);
    env.coverage_color = e.coverage_color;
    env.coverage_roughness = e.coverage_roughness;
    env.coverage_slope = e.coverage_slope;
    // environment_map (an asset) is resolved by extract_scene through the GpuAssetCache.
    env.environment_intensity = std::isfinite(e.environment_intensity) ? std::max(e.environment_intensity, 0.0f) : 1.0f;
    env.environment_rotation = std::isfinite(e.environment_rotation) ? math::radians(e.environment_rotation) : 0.0f;
    env.sky_blur = std::isfinite(e.sky_blur) ? std::clamp(e.sky_blur, 0.0f, 1.0f) : 0.0f;
}
static_assert(static_cast<int>(gameplay::EnvironmentComponent::Fog::ExponentialSquared) ==
              static_cast<int>(render::FogMode::ExponentialSquared));

namespace {

// Joint entities of skeletons, cached per (skeleton root, model) in a scene resource and
// re-resolved when an entity dies or is renamed.
struct SkinBindings {
    struct Entry {
        Entity root;
        Uuid model;
        std::vector<Entity> nodes;
    };
    std::unordered_map<std::uint64_t, Entry> entries;
};

const std::vector<Entity>& skeleton_nodes(Scene& scene, Entity root, const Uuid& model_uuid, const GpuModel& model) {
    auto* res = scene.world().resource<SkinBindings>();
    if (!res) res = &scene.world().set_resource<SkinBindings>();
    if (res->entries.size() > 4096) res->entries.clear(); // bound memory for scenes that churn characters
    const std::uint64_t key = root.bits() * 0x9e3779b97f4a7c15ull ^ UuidHash{}(model_uuid);
    auto& entry = res->entries[key];
    bool valid = entry.root == root && entry.model == model_uuid && entry.nodes.size() == model.skeleton.size();
    for (std::size_t i = 0; valid && i < entry.nodes.size(); ++i) {
        const Entity e = entry.nodes[i];
        if (e == Entity{}) continue;
        const auto* n = scene.world().alive(e) ? scene.world().get<Name>(e) : nullptr;
        valid = n && n->value == model.skeleton[i].name && scene.is_descendant(e, root);
    }
    if (!valid) {
        entry.root = root;
        entry.model = model_uuid;
        entry.nodes = animation::bind_nodes(scene, root, model.skeleton);
    }
    return entry.nodes;
}

} // namespace

namespace {
// Captures stale probes (within the per-frame budget) and hands every captured probe to the
// renderer. Captures see the scene without probes (one reflection bounce).
void apply_reflection_probes(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                             const SceneViewSettings& settings, ExtractStats& stats) {
    auto& w = scene.world();
    struct Found {
        std::uint64_t key;
        gameplay::ReflectionProbeComponent probe;
        math::Vec3 center, capture;
    };
    std::vector<Found> found;
    w.each<gameplay::ReflectionProbeComponent, WorldTransform>(
        [&](Entity e, const gameplay::ReflectionProbeComponent& p, const WorldTransform& wt) {
            const math::Vec3 c = wt.matrix.transform_point({});
            found.push_back({e.bits(), p, c, c + p.capture_offset});
        });
    auto& captures = assets.probe_captures();
    for (auto& [key, entry] : captures) entry.seen = false;
    std::uint32_t budget = assets.probe_captures_per_frame();
    for (const Found& f : found) {
        auto& entry = captures[f.key];
        entry.seen = true;
        const std::uint32_t res = std::clamp(f.probe.resolution, 16u, 512u);
        const bool stale = !entry.map || entry.revision != f.probe.revision || entry.resolution != res ||
                           math::length(entry.position - f.capture) > 0.01f;
        if (stale && budget > 0) {
            --budget;
            auto r = assets.probe_renderer();
            if (!r) {
                VICTUS_LOG_ERROR("runtime", "reflection probe renderer: {}", r.error().message);
                continue;
            }
            assets.set_capturing_probes(true);
            auto img = render::capture_equirect(assets.context(), **r, f.capture, res,
                                                [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                    (void)extract_scene(scene, assets, pr, settings, &cam);
                                                });
            assets.set_capturing_probes(false);
            if (!img) {
                VICTUS_LOG_ERROR("runtime", "reflection probe capture: {}", img.error().message);
                continue;
            }
            render::EnvironmentBakeSettings bake = assets.environment_bake();
            bake.cube_size = res;
            auto map = render::EnvironmentMap::create(assets.context(), *img, bake);
            if (!map) {
                VICTUS_LOG_ERROR("runtime", "reflection probe bake: {}", map.error().message);
                continue;
            }
            entry.map = std::move(*map);
            entry.position = f.capture;
            entry.revision = f.probe.revision;
            entry.resolution = res;
            assets.note_probe_capture();
        }
        if (!entry.map) continue;
        render::ReflectionProbe rp;
        rp.map = entry.map.get();
        const math::Vec3 half = math::Vec3{std::abs(f.probe.size.x), std::abs(f.probe.size.y), std::abs(f.probe.size.z)} * 0.5f;
        rp.box.min = f.center - half;
        rp.box.max = f.center + half;
        rp.position = entry.position;
        rp.intensity = std::isfinite(f.probe.intensity) ? std::max(f.probe.intensity, 0.0f) : 1.0f;
        renderer.add_reflection_probe(rp);
        ++stats.reflection_probes;
    }
    std::erase_if(captures, [](const auto& kv) { return !kv.second.seen; });
}

// Advances stale irradiance bakes (within the per-frame probe budget) and binds the volume that
// contains the camera (the smallest one when several do), else the nearest. Bake captures see the
// scene without probes or volumes; bounce passes add the volume being baked (ADR-0056).
void apply_irradiance_volumes(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                              const SceneViewSettings& settings, math::Vec3 eye, ExtractStats& stats) {
    auto& w = scene.world();
    auto& bakes = assets.irradiance_bakes();
    for (auto& [key, entry] : bakes) entry.seen = false;
    std::uint32_t budget = assets.irradiance_probes_per_frame();
    const render::IrradianceVolume* best = nullptr;
    float best_score = 0.0f, best_intensity = 1.0f, best_offset = 0.0f;
    bool best_inside = false;
    w.each<gameplay::IrradianceVolumeComponent, WorldTransform>(
        [&](Entity e, const gameplay::IrradianceVolumeComponent& v, const WorldTransform& wt) {
            auto& entry = bakes[e.bits()];
            entry.seen = true;
            const math::Vec3 c = wt.matrix.transform_point({});
            const auto finite = [](float f, float d) { return std::isfinite(f) ? std::abs(f) : d; };
            const math::Vec3 size{std::max(finite(v.size.x, 1.0f), 0.1f), std::max(finite(v.size.y, 1.0f), 0.1f),
                                  std::max(finite(v.size.z, 1.0f), 0.1f)};
            render::IrradianceGrid grid;
            grid.bounds = {c - size * 0.5f, c + size * 0.5f};
            grid.counts = render::irradiance_counts_for(size, finite(v.spacing, 2.5f));
            grid = grid.sanitized();
            const std::uint32_t res = std::clamp(v.resolution, 16u, 128u), bounces = std::clamp(v.bounces, 1u, 4u);
            const float offset = std::clamp(finite(v.normal_offset, 0.0f), 0.0f, 4.0f);
            const float intensity = std::isfinite(v.intensity) ? std::max(v.intensity, 0.0f) : 1.0f;
            const bool moved = math::length(entry.grid.bounds.min - grid.bounds.min) > 0.01f ||
                               math::length(entry.grid.bounds.max - grid.bounds.max) > 0.01f || entry.grid.counts != grid.counts;
            const bool stale = moved || entry.revision != v.revision || entry.resolution != res || entry.bounces != bounces ||
                               std::abs(entry.normal_offset - offset) > 1e-4f;
            if (stale || (!entry.active && !entry.pending)) {
                if (moved) entry.active.reset(); // old probes would light the wrong place
                entry.grid = grid;
                entry.revision = v.revision;
                entry.resolution = res;
                entry.bounces = bounces;
                entry.normal_offset = offset;
                entry.baker.reset();
                entry.refresher.reset();
                auto vol = render::IrradianceVolume::create(assets.context(), grid);
                if (!vol) {
                    VICTUS_LOG_ERROR("runtime", "irradiance volume: {}", vol.error().message);
                    entry.pending.reset();
                    return;
                }
                entry.pending = std::move(*vol);
            }
            if (entry.pending && budget > 0) {
                auto r = assets.probe_renderer();
                if (!r) {
                    VICTUS_LOG_ERROR("runtime", "irradiance probe renderer: {}", r.error().message);
                } else {
                    render::IrradianceBakeSettings bs;
                    bs.face_size = res;
                    bs.bounces = bounces;
                    bs.intensity = intensity;
                    bs.normal_offset = offset;
                    const std::uint32_t before = entry.baker.probes_baked();
                    assets.set_capturing_probes(true);
                    const Status st = entry.baker.step(assets.context(), **r, *entry.pending,
                                                       [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                           (void)extract_scene(scene, assets, pr, settings, &cam);
                                                       },
                                                       bs, budget);
                    assets.set_capturing_probes(false);
                    const std::uint32_t done = entry.baker.probes_baked() - before;
                    budget -= std::min(budget, done);
                    stats.irradiance_probes_baked += done;
                    if (!st.ok()) {
                        VICTUS_LOG_ERROR("runtime", "irradiance bake: {}", st.error().message);
                        entry.pending.reset();
                    } else if (entry.baker.done()) {
                        entry.active = std::move(entry.pending);
                        assets.note_irradiance_bake();
                    }
                }
            }
            // Dynamic volumes keep refreshing the finished bake (ADR-0057).
            if (v.dynamic && entry.active && !entry.pending && budget > 0 && renderer.settings().dynamic_gi) {
                if (auto r = assets.probe_renderer()) {
                    render::IrradianceBakeSettings bs;
                    bs.face_size = res;
                    bs.intensity = intensity;
                    bs.normal_offset = offset;
                    const std::uint64_t before = entry.refresher.probes_refreshed();
                    assets.set_capturing_probes(true);
                    const Status st = entry.refresher.step(assets.context(), **r, *entry.active,
                                                           [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                               (void)extract_scene(scene, assets, pr, settings, &cam);
                                                           },
                                                           bs, budget, v.dynamic_blend);
                    assets.set_capturing_probes(false);
                    const auto done = static_cast<std::uint32_t>(entry.refresher.probes_refreshed() - before);
                    budget -= std::min(budget, done);
                    stats.irradiance_probes_refreshed += done;
                    if (!st.ok()) VICTUS_LOG_ERROR("runtime", "irradiance refresh: {}", st.error().message);
                }
            }
            // While nothing has finished yet, the partially baked first pass already helps.
            const render::IrradianceVolume* shown = entry.active ? entry.active.get() : entry.pending.get();
            if (!shown || shown->valid_count() == 0) return;
            ++stats.irradiance_volumes;
            const math::Vec3 lo = grid.bounds.min, hi = grid.bounds.max;
            const bool inside = eye.x >= lo.x && eye.y >= lo.y && eye.z >= lo.z && eye.x <= hi.x && eye.y <= hi.y && eye.z <= hi.z;
            const math::Vec3 d{std::max({lo.x - eye.x, 0.0f, eye.x - hi.x}), std::max({lo.y - eye.y, 0.0f, eye.y - hi.y}),
                               std::max({lo.z - eye.z, 0.0f, eye.z - hi.z})};
            // Inside volumes rank by size (smaller = more detailed), outside ones after them by distance.
            const float score = inside ? size.x * size.y * size.z : math::length(d);
            if (!best || (inside && !best_inside) || (inside == best_inside && score < best_score)) {
                best = shown;
                best_inside = inside;
                best_score = score;
                best_intensity = intensity;
                best_offset = offset;
            }
        });
    std::erase_if(bakes, [](const auto& kv) { return !kv.second.seen; });
    if (best) renderer.set_irradiance_volume(best, best_intensity, best_offset);
}
} // namespace

ExtractStats extract_scene(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                           const SceneViewSettings& settings, const render::Camera3D* camera_override) {
    ExtractStats stats;
    assets.collect_procedural();
    ecs::World& w = scene.world();
    render::Camera3D camera = settings.fallback_camera;
    if (camera_override) {
        camera = *camera_override;
        stats.camera_found = true;
    } else {
        w.each<CameraComponent, WorldTransform>([&](const CameraComponent& c, const WorldTransform& wt) {
            if (stats.camera_found || !c.primary) return;
            math::Vec3 pos, scale;
            math::Quat rot;
            math::decompose(wt.matrix, pos, rot, scale);
            camera = render::Camera3D::from_transform(pos, rot);
            camera.fov_y = math::radians(c.fov_y_degrees);
            camera.z_near = c.z_near;
            camera.z_far = c.z_far;
            stats.camera_found = true;
        });
    }

    render::DirectionalLight sun;
    bool has_sun = false;
    std::vector<render::PointLight> points;
    std::vector<render::SpotLight> spots;
    w.each<LightComponent, WorldTransform>([&](const LightComponent& l, const WorldTransform& wt) {
        ++stats.lights;
        if (l.type == LightComponent::Type::Directional) {
            if (has_sun) return;
            has_sun = true;
            sun.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            sun.color = l.color;
            sun.intensity = l.intensity;
            sun.shadows = l.shadows;
        } else if (l.type == LightComponent::Type::Spot) {
            render::SpotLight sl;
            sl.position = wt.matrix.transform_point({});
            sl.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            sl.color = l.color;
            sl.intensity = l.intensity;
            sl.range = l.range;
            sl.cast_shadows = l.shadows;
            const float outer = std::isfinite(l.outer_angle_degrees) ? std::clamp(l.outer_angle_degrees, 0.1f, 89.5f) : 30.0f;
            sl.outer_angle = math::radians(outer);
            sl.inner_angle = math::radians(std::isfinite(l.inner_angle_degrees) ? std::clamp(l.inner_angle_degrees, 0.0f, outer) : outer);
            spots.push_back(sl);
        } else {
            render::PointLight p;
            p.position = wt.matrix.transform_point({});
            p.color = l.color;
            p.intensity = l.intensity;
            p.range = l.range;
            p.cast_shadows = l.shadows;
            points.push_back(p);
        }
    });
    if (!has_sun) sun.intensity = 0.0f;
    render::Environment env = settings.environment;
    bool has_env = false;
    w.each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) {
        if (has_env) return;
        has_env = true;
        apply_environment(e, env, sun);
        if (!e.environment_map.is_nil()) {
            if (auto map = assets.environment(e.environment_map)) env.environment_map = *map;
            else ++stats.missing_assets;
        } else if (e.physical_sky) {
            render::AtmosphereSettings a;
            a.sun_direction = -sun.direction;
            const float scale = std::isfinite(e.atmosphere_intensity) ? std::max(e.atmosphere_intensity, 0.0f) : 1.0f;
            a.sun_illuminance = (has_sun ? sun.intensity : 3.0f) * scale;
            a.rayleigh_scale = e.atmosphere_density;
            a.mie_scale = e.atmosphere_haze;
            a.ground_albedo = {e.ground.r, e.ground.g, e.ground.b};
            a.width = assets.environment_bake().cube_size <= 128 ? 64 : 128; // Low tier: ~4x cheaper CPU bake
            if (auto map = assets.atmosphere(a)) {
                env.environment_map = *map;
                env.environment_sun_disc = has_sun && a.sun_direction.y > -0.02f; // no sun disc drawn on the ground at night
                env.environment_rotation = 0.0f; // the sky is baked in world space
                if (e.atmosphere_tints_sun && has_sun) {
                    const math::Vec3 t = render::atmosphere_sun_transmittance(a);
                    sun.color = {sun.color.r * t.x, sun.color.g * t.y, sun.color.b * t.z, sun.color.a};
                }
            } else {
                VICTUS_LOG_ERROR("runtime", "physical sky: {}", map.error().message);
            }
        }
    });
    renderer.begin(camera, sun, env);
    for (const auto& p : points) renderer.add_point_light(p);
    for (const auto& sl : spots) renderer.add_spot_light(sl);
    if (!assets.capturing_probes()) {
        apply_reflection_probes(scene, assets, renderer, settings, stats);
        apply_irradiance_volumes(scene, assets, renderer, settings, camera.position, stats);
    }
    static_assert(static_cast<int>(gameplay::DecalComponent::Blend::Additive) == static_cast<int>(render::DecalBlend::Additive));
    static_assert(static_cast<int>(gameplay::DecalComponent::Blend::Alpha) == static_cast<int>(render::DecalBlend::Alpha));
    w.each<gameplay::DecalComponent, WorldTransform>([&](const gameplay::DecalComponent& d, const WorldTransform& wt) {
        if (!d.visible) return;
        render::Decal decal;
        const math::Vec3 size{std::max(d.size.x, 0.001f), std::max(d.size.y, 0.001f), std::max(d.size.z, 0.001f)};
        decal.transform = wt.matrix * math::Mat4::scale(size);
        if (!d.texture.is_nil()) {
            auto tex = assets.texture(d.texture);
            if (tex) decal.texture = *tex;
            else ++stats.missing_assets;
            decal.nearest_filter = assets.texture_nearest(d.texture);
        }
        decal.color = d.color;
        decal.blend = static_cast<render::DecalBlend>(std::min<int>(static_cast<int>(d.blend), 2));
        decal.uv = {d.uv_min.x, d.uv_min.y, d.uv_max.x, d.uv_max.y};
        decal.angle_fade = d.angle_fade;
        decal.depth_fade = d.depth_fade;
        decal.sort_order = d.sort_order;
        renderer.add_decal(decal);
        ++stats.decals;
    });

    std::vector<math::Mat4> palette;
    const auto apply_overrides = [&](Entity entity, render::Material& m) {
        const auto* o = w.get<gameplay::MaterialOverridesComponent>(entity);
        if (!o) return;
        m.uv_offset = {m.uv_offset.x + o->uv_offset.x + o->scrolled.x, m.uv_offset.y + o->uv_offset.y + o->scrolled.y};
        if (o->override_emissive) {
            m.emissive = o->emissive;
            m.emissive_strength = o->emissive_strength;
        }
    };
    w.each<terrain::TerrainComponent, terrain::TerrainRuntime, WorldTransform>(
        [&](Entity entity, const terrain::TerrainComponent& t, const terrain::TerrainRuntime& rt, const WorldTransform& wt) {
            if (!rt.data || rt.chunks.empty()) return;
            render::Material m;
            m.base_color = {1, 1, 1, 1};
            m.metallic = 0.0f;
            m.roughness = std::clamp(t.roughness, 0.0f, 1.0f);
            m.cast_shadows = t.cast_shadows;
            if (auto sh = assets.terrain_shader(); sh) m.surface_shader = *sh;
            rhi::TextureHandle* slots[terrain::kLayerCount] = {&m.base_color_texture, &m.metallic_roughness_texture,
                                                              &m.normal_texture, &m.emissive_texture};
            for (std::uint32_t i = 0; i < terrain::kLayerCount; ++i) {
                const math::Color tint = t.layer_tint(i);
                float tiling = 0.0f;
                if (const Uuid& tex = t.layer_texture(i); !tex.is_nil()) {
                    if (auto h = assets.texture(tex); h) {
                        *slots[i] = *h;
                        tiling = std::max(t.layer_tiling(i), 1e-3f);
                    } else {
                        ++stats.missing_assets;
                    }
                }
                m.shader_params[i] = {tint.r, tint.g, tint.b, tiling};
            }
            // Scale is ignored (the component's size sets the extent): normalized axes.
            const math::Mat4 model = terrain::rigid_matrix(wt.matrix);
            const std::uint64_t motion_base = (entity.bits() + 1) * 0x9E3779B97F4A7C15ull; // distinct per chunk
            for (std::size_t i = 0; i < rt.chunks.size(); ++i) {
                if (!rt.chunks[i]) continue;
                const render::GpuMesh* mesh = assets.procedural(*rt.chunks[i]);
                if (!mesh) continue;
                renderer.submit(*mesh, m, model, (motion_base + i) | 1u);
                ++stats.terrain_chunks;
            }
        });
    // Foliage entities draw their MeshRenderer once per scattered instance near the camera; the
    // renderer merges the copies into instanced batches (one draw per LOD/probe/pass).
    std::vector<math::Mat4> foliage;
    const auto foliage_instances = [&](Entity entity) -> bool {
        const auto* fr = w.get<terrain::FoliageRuntime>(entity);
        const auto* fc = fr ? w.get<terrain::FoliageComponent>(entity) : nullptr;
        if (!fc) return false;
        foliage.clear();
        const auto* twt = fr->terrain.is_null() ? nullptr : w.get<WorldTransform>(fr->terrain);
        if (!twt || fr->instances == 0) return true;
        const math::Mat4 model = terrain::rigid_matrix(twt->matrix);
        const float range = std::isfinite(fc->draw_distance) ? std::max(fc->draw_distance, 0.0f) : 0.0f;
        const float range2 = range * range;
        for (std::size_t c = 0; c < fr->chunks.size(); ++c) {
            const auto& list = fr->chunks[c];
            if (list.empty() || !fr->chunk_bounds[c].valid()) continue;
            // Skip whole chunks beyond the draw distance (bounding sphere of the instance origins).
            const math::Aabb& b = fr->chunk_bounds[c];
            const math::Vec3 centre = model.transform_point(b.center());
            const float radius = math::length(b.extents());
            if (math::length(centre - camera.position) - radius > range) continue;
            for (const math::Mat4& local : list) {
                const math::Mat4 world = model * local;
                const math::Vec3 p = world.transform_point({});
                const math::Vec3 d = p - camera.position;
                if (math::dot(d, d) <= range2) foliage.push_back(world);
            }
        }
        stats.foliage_instances += static_cast<std::uint32_t>(foliage.size());
        return true;
    };
    w.each<MeshRendererComponent, WorldTransform>([&](Entity entity, const MeshRendererComponent& mr, const WorldTransform& wt) {
        if (!mr.visible) return;
        const std::uint64_t motion_id = entity.bits() + 1; // stable per entity; never 0 for live entities
        const bool scattered = foliage_instances(entity);
        const auto emit = [&](const render::GpuMesh& mesh, const render::Material& m) {
            if (!scattered) {
                renderer.submit(mesh, m, wt.matrix, motion_id);
                return;
            }
            renderer.submit_instances(mesh, m, foliage); // static: no motion id
        };
        const auto* pm = w.get<scene::ProceduralMeshComponent>(entity);
        if (pm && pm->mesh) {
            const render::GpuMesh* mesh = assets.procedural(*pm->mesh);
            if (!mesh) return; // empty geometry draws nothing
            render::Material m;
            m.base_color = mr.base_color;
            m.metallic = mr.metallic;
            m.roughness = mr.roughness;
            if (!mr.material_asset.is_nil()) {
                if (auto mat = assets.material(mr.material_asset); mat) {
                    m = *mat;
                    m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                                    m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
                } else {
                    ++stats.missing_assets;
                }
            }
            m.cast_shadows = mr.cast_shadows;
            apply_overrides(entity, m);
            emit(*mesh, m);
            ++stats.meshes;
            return;
        }
        if (!mr.mesh_asset.is_nil()) {
            auto model = assets.model(mr.mesh_asset);
            if (!model || mr.mesh_index >= (*model)->meshes.size()) {
                ++stats.missing_assets;
                return;
            }
            render::Material m = (*model)->mesh_materials[mr.mesh_index];
            if (!mr.material_asset.is_nil()) {
                if (auto mat = assets.material(mr.material_asset); mat) m = *mat;
                else ++stats.missing_assets;
            }
            m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                            m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
            m.cast_shadows = mr.cast_shadows;
            apply_overrides(entity, m);
            const GpuModel& gm = **model;
            const auto* skinned = w.get<animation::SkinnedMeshComponent>(entity);
            if (!scattered && skinned && gm.max_joint[mr.mesh_index] >= 0 && skinned->skin < gm.cpu.skins.size()) {
                const assets::ModelSkin& skin = gm.cpu.skins[skinned->skin];
                const Entity root = scene.find(skinned->skeleton);
                if (root != Entity{} && static_cast<std::size_t>(gm.max_joint[mr.mesh_index]) < skin.joints.size()) {
                    const auto& nodes = skeleton_nodes(scene, root, mr.mesh_asset, gm);
                    const math::Mat4 inv_model = math::inverse(wt.matrix);
                    const auto& jb = gm.joint_bounds[mr.mesh_index];
                    palette.resize(skin.joints.size());
                    math::Aabb bounds;
                    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                        const Entity je = skin.joints[j] < nodes.size() ? nodes[skin.joints[j]] : Entity{};
                        const auto* jw = je != Entity{} ? w.get<WorldTransform>(je) : nullptr;
                        // Unbound joints keep the bind pose relative to the mesh (identity skin matrix).
                        const math::Mat4 world_skin = jw ? jw->matrix * skin.inverse_bind[j] : wt.matrix;
                        palette[j] = jw ? inv_model * world_skin : math::Mat4{};
                        if (j < jb.size() && jb[j].valid()) {
                            const math::Aabb b = jb[j].transformed(world_skin);
                            bounds.expand(b.min);
                            bounds.expand(b.max);
                        }
                    }
                    renderer.submit_skinned(gm.meshes[mr.mesh_index], m, wt.matrix, palette, bounds, motion_id);
                    ++stats.meshes;
                    ++stats.skinned;
                    return;
                }
            }
            emit(gm.meshes[mr.mesh_index], m);
            ++stats.meshes;
            return;
        }
        const render::GpuMesh* mesh = mr.primitive.empty() ? nullptr : assets.primitive(mr.primitive);
        if (!mesh) {
            ++stats.missing_assets;
            return;
        }
        render::Material m;
        m.base_color = mr.base_color;
        m.metallic = mr.metallic;
        m.roughness = mr.roughness;
        if (!mr.material_asset.is_nil()) {
            if (auto mat = assets.material(mr.material_asset); mat) {
                m = *mat;
                m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                                m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
            } else {
                ++stats.missing_assets;
            }
        }
        m.cast_shadows = mr.cast_shadows;
        apply_overrides(entity, m);
        emit(*mesh, m);
        ++stats.meshes;
    });

    std::vector<particles::EmitterView> emitters;
    particles::gather(scene, emitters);
    for (const particles::EmitterView& v : emitters) {
        render::ParticleMaterial pm;
        pm.blend = v.emitter->blend == particles::Blend::Additive ? render::ParticleBlend::Additive : render::ParticleBlend::Alpha;
        pm.softness = v.emitter->softness;
        if (!v.emitter->texture.is_nil()) {
            auto tex = assets.texture(v.emitter->texture);
            if (tex) pm.texture = *tex;
            else ++stats.missing_assets;
        }
        // Billboard and ParticleInstance share one 48-byte layout (position+size, colour, rotation).
        static_assert(sizeof(particles::Billboard) == sizeof(render::ParticleInstance));
        const std::span<const render::ParticleInstance> instances(reinterpret_cast<const render::ParticleInstance*>(v.billboards.data()),
                                                                  v.billboards.size());
        renderer.submit_particles(instances, pm, v.bounds);
        ++stats.particle_emitters;
        stats.particles += static_cast<std::uint32_t>(v.billboards.size());
    }
    return stats;
}

std::uint32_t extract_lights_2d(Scene& scene, render::SpriteBatch& batch) {
    bool any = false;
    math::Color ambient{0, 0, 0, 1};
    std::vector<render::Light2D> lights;
    scene.world().each<gameplay::Light2DComponent, WorldTransform>([&](const gameplay::Light2DComponent& l, const WorldTransform& wt) {
        if (!l.enabled) return;
        any = true;
        const float k = std::isfinite(l.intensity) ? std::max(l.intensity, 0.0f) : 0.0f;
        if (l.type == gameplay::Light2DComponent::Type::Global) {
            ambient = {ambient.r + l.color.r * k, ambient.g + l.color.g * k, ambient.b + l.color.b * k, 1};
            return;
        }
        render::Light2D out;
        out.type = l.type == gameplay::Light2DComponent::Type::Spot ? render::Light2D::Type::Spot : render::Light2D::Type::Point;
        const math::Vec3 p = wt.matrix.transform_point({});
        out.position = {p.x, p.y};
        const math::Vec3 ax = wt.matrix.transform_vector({1, 0, 0}), ay = wt.matrix.transform_vector({0, 1, 0});
        const float scale = std::max(math::length(math::Vec2{ax.x, ax.y}), math::length(math::Vec2{ay.x, ay.y}));
        out.radius = l.radius * (scale > 0.0f ? scale : 1.0f);
        out.color = l.color;
        out.intensity = k;
        out.direction = {-ay.x, -ay.y};
        const float outer = std::isfinite(l.outer_angle_degrees) ? std::clamp(l.outer_angle_degrees, 0.1f, 180.0f) : 40.0f;
        out.outer_angle = math::radians(outer);
        out.inner_angle = math::radians(std::isfinite(l.inner_angle_degrees) ? std::clamp(l.inner_angle_degrees, 0.0f, outer) : outer);
        out.falloff = l.falloff;
        lights.push_back(out);
    });
    if (!any) return 0;
    batch.set_lighting(ambient);
    for (const auto& l : lights) batch.add_light(l);
    return static_cast<std::uint32_t>(lights.size());
}

std::uint32_t extract_sprites(Scene& scene, GpuAssetCache& assets, render::SpriteBatch& batch) {
    std::uint32_t count = 0;
    (void)extract_lights_2d(scene, batch);
    scene.world().each<SpriteComponent, WorldTransform>([&](const SpriteComponent& s, const WorldTransform& wt) {
        render::Sprite sp;
        if (!s.texture_asset.is_nil()) {
            auto tex = assets.texture(s.texture_asset);
            if (tex) sp.texture = *tex;
            sp.nearest = assets.texture_nearest(s.texture_asset);
        }
        math::Vec3 pos, scale;
        math::Quat rot;
        math::decompose(wt.matrix, pos, rot, scale);
        sp.position = {pos.x, pos.y};
        sp.size = {s.size.x * scale.x, s.size.y * scale.y};
        sp.rotation = math::to_euler(rot).z;
        sp.origin = s.pivot;
        sp.color = s.tint;
        sp.layer = s.layer;
        sp.flip_x = s.flip_x;
        sp.flip_y = s.flip_y;
        sp.uv = s.uv;
        sp.lit = !s.unlit;
        batch.draw(sp);
        ++count;
    });
    // Tilemaps: one sprite per painted cell, through the map's transform.
    scene.world().each<tilemap::TilemapComponent, tilemap::TilemapRuntime, WorldTransform>(
        [&](const tilemap::TilemapComponent& map, const tilemap::TilemapRuntime& rt, const WorldTransform& wt) {
            if (!rt.tileset || map.cells.empty()) return;
            const tilemap::Tileset& ts = *rt.tileset;
            rhi::TextureHandle texture{};
            bool nearest = false;
            if (!ts.texture.is_nil()) {
                auto tex = assets.texture(ts.texture);
                if (tex) texture = *tex;
                nearest = assets.texture_nearest(ts.texture);
            }
            math::Vec3 pos, scale;
            math::Quat rot;
            math::decompose(wt.matrix, pos, rot, scale);
            const float rotation = math::to_euler(rot).z;
            const math::Vec2 size{map.cell_size.x * scale.x, map.cell_size.y * scale.y};
            const std::uint32_t tiles = ts.tile_count();
            for (const auto& [key, cell] : map.cells) {
                if (cell.tile < 0 || static_cast<std::uint32_t>(cell.tile) >= tiles) continue;
                const math::Vec3 local{(static_cast<float>(tilemap::key_x(key)) + 0.5f) * map.cell_size.x,
                                       (static_cast<float>(tilemap::key_y(key)) + 0.5f) * map.cell_size.y, 0.0f};
                const math::Vec3 p = wt.matrix.transform_point(local);
                render::Sprite sp;
                sp.texture = texture;
                sp.nearest = nearest;
                sp.position = {p.x, p.y};
                sp.size = size;
                sp.rotation = rotation;
                sp.origin = {0.5f, 0.5f};
                sp.color = map.tint;
                sp.layer = map.layer;
                sp.flip_x = cell.flip_x;
                sp.flip_y = cell.flip_y;
                sp.uv = ts.tile_uv(static_cast<std::uint32_t>(cell.tile));
                sp.lit = true;
                batch.draw(sp);
                ++count;
            }
        });
    // 2D particles: one sprite per billboard on the emitter's layer (XY plane, Z ignored).
    std::vector<particles::EmitterView> emitters;
    particles::gather(scene, emitters);
    for (const particles::EmitterView& v : emitters) {
        rhi::TextureHandle texture = assets.context().soft_particle();
        bool nearest = false;
        if (!v.emitter->texture.is_nil()) {
            auto tex = assets.texture(v.emitter->texture);
            if (tex) texture = *tex;
            nearest = assets.texture_nearest(v.emitter->texture);
        }
        for (const particles::Billboard& b : v.billboards) {
            render::Sprite sp;
            sp.texture = texture;
            sp.nearest = nearest;
            sp.position = {b.position.x, b.position.y};
            sp.size = {b.size, b.size};
            sp.rotation = b.rotation;
            sp.color = {std::min(b.color.r, 1.0f), std::min(b.color.g, 1.0f), std::min(b.color.b, 1.0f), b.color.a};
            sp.layer = v.emitter->layer;
            sp.additive = v.emitter->blend == particles::Blend::Additive;
            sp.lit = true; // additive particles stay emissive (never lit)
            batch.draw(sp);
            ++count;
        }
    }
    return count;
}

} // namespace victus::runtime
