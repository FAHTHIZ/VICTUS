// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/particles/particles.hpp"

#include "victus/core/log.hpp"
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
    for (auto& [id, model] : models_) {
        for (auto& m : model->meshes) ctx_.destroy(m);
        for (auto& t : model->textures) ctx_.device().destroy(t);
    }
    models_.clear();
    for (auto& [name, mesh] : primitives_) ctx_.destroy(mesh);
    primitives_.clear();
    evict_shaders();
}

void GpuAssetCache::evict(const Uuid& uuid) {
    // Materials hold texture handles: any eviction may invalidate them, and they are cheap to rebuild.
    materials_.clear();
    nearest_.erase(uuid);
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

Result<rhi::TextureHandle> GpuAssetCache::texture(const Uuid& uuid) {
    if (const auto it = textures_.find(uuid); it != textures_.end()) return it->second;
    if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
    auto cooked = db_->load_texture(uuid);
    if (!cooked) return std::move(cooked).error();
    render::TextureOptions opt;
    opt.srgb = cooked->srgb;
    opt.name = cooked->name;
    auto tex = ctx_.create_texture_mips(cooked->mips, opt);
    if (!tex) return tex;
    textures_.emplace(uuid, *tex);
    return tex;
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

Result<rhi::ShaderHandle> GpuAssetCache::surface_shader(const Uuid& uuid) {
    if (const auto it = shaders_.find(uuid); it != shaders_.end()) return it->second;
    auto build = [&]() -> Result<rhi::ShaderHandle> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        const assets::AssetRecord* rec = db_->find(uuid);
        if (!rec || rec->kind != assets::AssetKind::Shader)
            return make_error(ErrorCode::NotFound, "surface shader " + uuid.to_string() + " is missing");
        const std::string path = rec->path; // records may move on refresh
        auto code = db_->load_shader_source(uuid);
        if (!code) return std::move(code).error();
        auto compiled = compile_surface_shader(*code, path, ctx_.device().capabilities().backend == rhi::Backend::Metal);
        if (!compiled) return std::move(compiled).error();
        rhi::ShaderDesc d;
        d.name = path;
        d.stage = rhi::ShaderStage::Fragment;
        d.spirv = compiled->spirv;
        d.msl = compiled->msl;
        d.samplers = compiled->counts.samplers;
        d.storage_textures = compiled->counts.storage_textures;
        d.storage_buffers = compiled->counts.storage_buffers;
        d.uniform_buffers = compiled->counts.uniform_buffers;
        return ctx_.device().create_shader(d);
    };
    auto result = build();
    if (!result) VICTUS_LOG_ERROR("assets", "{}", result.error().message);
    return shaders_.emplace(uuid, std::move(result)).first->second;
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
        render::TextureOptions opt;
        opt.srgb = t.srgb;
        opt.name = t.name;
        auto tex = ctx_.create_texture_mips(t.mips, opt);
        if (!tex) return fail(std::move(tex).error());
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

const render::GpuMesh* GpuAssetCache::primitive(std::string_view name) {
    const std::string key(name);
    if (const auto it = primitives_.find(key); it != primitives_.end()) return &it->second;
    render::MeshData data;
    if (key == "cube") data = render::make_cube();
    else if (key == "sphere") data = render::make_uv_sphere();
    else if (key == "plane") data = render::make_plane({1, 1});
    else if (key == "cylinder") data = render::make_cylinder();
    else return nullptr;
    auto g = ctx_.create_mesh(data, key);
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "primitive '{}': {}", key, g.error().message);
        return nullptr;
    }
    return &primitives_.emplace(key, *g).first->second;
}

Entity instantiate_model(Scene& scene, const Uuid& model_uuid, const assets::CookedModel& model, std::string_view name,
                         Entity parent) {
    const Entity root = scene.create(name, parent);
    std::vector<Entity> created(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const assets::ModelNode& node = model.nodes[i];
        const Entity p = node.parent < 0 ? root : created[static_cast<std::size_t>(node.parent)];
        const Entity e = scene.create(node.name, p);
        Transform* t = scene.world().get<Transform>(e);
        t->position = node.translation;
        t->rotation = node.rotation;
        t->scale = node.scale;
        created[i] = e;
        auto add_mesh = [&](Entity target, std::uint32_t mi) {
            auto& mr = scene.world().add<MeshRendererComponent>(target);
            mr.mesh_asset = model_uuid;
            mr.mesh_index = mi;
            if (node.skin >= 0 && model.meshes[mi].data.skinned())
                scene.world().add<animation::SkinnedMeshComponent>(target, scene.uuid(root), static_cast<std::uint32_t>(node.skin));
        };
        if (node.meshes.size() == 1) {
            add_mesh(e, node.meshes[0]);
        } else {
            // Multi-primitive nodes get one child per primitive so each has its own component.
            for (const std::uint32_t mi : node.meshes) add_mesh(scene.create(std::format("{}[{}]", node.name, mi), e), mi);
        }
    }
    if (!model.animations.empty()) {
        auto& a = scene.world().add<animation::AnimatorComponent>(root);
        a.model = model_uuid;
        a.clip = model.animations.front().name;
    }
    return root;
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

ExtractStats extract_scene(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                           const SceneViewSettings& settings, const render::Camera3D* camera_override) {
    ExtractStats stats;
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
    w.each<LightComponent, WorldTransform>([&](const LightComponent& l, const WorldTransform& wt) {
        ++stats.lights;
        if (l.type == LightComponent::Type::Directional) {
            if (has_sun) return;
            has_sun = true;
            sun.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            sun.color = l.color;
            sun.intensity = l.intensity;
            sun.shadows = l.shadows;
        } else {
            render::PointLight p;
            p.position = wt.matrix.transform_point({});
            p.color = l.color;
            p.intensity = l.intensity;
            p.range = l.range;
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
    });
    renderer.begin(camera, sun, env);
    for (const auto& p : points) renderer.add_point_light(p);

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
    w.each<MeshRendererComponent, WorldTransform>([&](Entity entity, const MeshRendererComponent& mr, const WorldTransform& wt) {
        if (!mr.visible) return;
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
            if (skinned && gm.max_joint[mr.mesh_index] >= 0 && skinned->skin < gm.cpu.skins.size()) {
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
                    renderer.submit_skinned(gm.meshes[mr.mesh_index], m, wt.matrix, palette, bounds);
                    ++stats.meshes;
                    ++stats.skinned;
                    return;
                }
            }
            renderer.submit(gm.meshes[mr.mesh_index], m, wt.matrix);
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
        renderer.submit(*mesh, m, wt.matrix);
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

std::uint32_t extract_sprites(Scene& scene, GpuAssetCache& assets, render::SpriteBatch& batch) {
    std::uint32_t count = 0;
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
        batch.draw(sp);
        ++count;
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
            batch.draw(sp);
            ++count;
        }
    }
    return count;
}

} // namespace victus::runtime
