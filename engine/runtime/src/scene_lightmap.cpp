// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/scene_lightmap.hpp"

#include "victus/animation/animation.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>

namespace victus::runtime {

namespace {

math::Vec3 rgb(const math::Color& c) { return {c.r, c.g, c.b}; }

float finite_nonneg(float v, float fallback) { return std::isfinite(v) ? std::max(v, 0.0f) : fallback; }

} // namespace

render::MeshData primitive_mesh_data(std::string_view name) {
    render::MeshData data;
    if (name == "cube") data = render::make_cube();
    else if (name == "sphere") data = render::make_uv_sphere();
    else if (name == "plane") data = render::make_plane({1, 1});
    else if (name == "cylinder") data = render::make_cylinder();
    else return data;
    // Curved primitives get LODs like imported models (the cube and plane are already minimal).
    if (name == "sphere" || name == "cylinder") render::generate_lods(data);
    render::optimize_mesh(data);
    return data;
}

render::MeshData procedural_mesh_data(const scene::ProceduralMesh& mesh) {
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
    return data;
}

namespace {

// Geometry plus the material inputs the baker needs.
struct BakeSource {
    render::MeshData mesh;
    math::Vec3 albedo{0.8f, 0.8f, 0.8f};
    math::Vec3 emissive{0, 0, 0};
};

Result<BakeSource> gather(scene::Scene& scene, scene::Entity entity, assets::AssetDatabase* db,
                          std::map<Uuid, std::shared_ptr<assets::CookedModel>>* models) {
    ecs::World& w = scene.world();
    const auto* mr = w.get<scene::MeshRendererComponent>(entity);
    if (!mr) return make_error(ErrorCode::InvalidArgument, "no MeshRenderer");
    BakeSource out;
    math::Color base{1, 1, 1, 1};
    float metallic = mr->metallic;
    math::Vec3 emissive{0, 0, 0};
    const auto* pm = w.get<scene::ProceduralMeshComponent>(entity);
    if (pm && pm->mesh) {
        if (pm->mesh->positions.empty() || pm->mesh->indices.empty()) return make_error(ErrorCode::InvalidArgument, "empty procedural mesh");
        out.mesh = procedural_mesh_data(*pm->mesh);
    } else if (!mr->mesh_asset.is_nil()) {
        if (!db) return make_error(ErrorCode::NotFound, "no asset database");
        std::shared_ptr<assets::CookedModel> model;
        if (models) {
            auto& slot = (*models)[mr->mesh_asset];
            if (!slot) {
                auto loaded = db->load_model(mr->mesh_asset);
                if (!loaded) return std::move(loaded).error();
                slot = std::make_shared<assets::CookedModel>(std::move(loaded).value());
            }
            model = slot;
        } else {
            auto loaded = db->load_model(mr->mesh_asset);
            if (!loaded) return std::move(loaded).error();
            model = std::make_shared<assets::CookedModel>(std::move(loaded).value());
        }
        if (mr->mesh_index >= model->meshes.size()) return make_error(ErrorCode::NotFound, "mesh index out of range");
        const assets::ModelMesh& mm = model->meshes[mr->mesh_index];
        if (mm.data.skinned() || w.get<animation::SkinnedMeshComponent>(entity))
            return make_error(ErrorCode::Unsupported, "skinned meshes are not lightmapped");
        if (!mm.data.morphs.empty()) return make_error(ErrorCode::Unsupported, "meshes with morph targets are not lightmapped");
        out.mesh = mm.data;
        if (mm.material < model->materials.size()) {
            const assets::ModelMaterial& mat = model->materials[mm.material];
            base = mat.base_color;
            metallic = mat.metallic;
            emissive = rgb(mat.emissive) * finite_nonneg(mat.emissive_strength, 1.0f);
        }
    } else if (!mr->primitive.empty()) {
        out.mesh = primitive_mesh_data(mr->primitive);
        if (out.mesh.indices.empty()) return make_error(ErrorCode::NotFound, std::format("unknown primitive '{}'", mr->primitive));
    } else {
        return make_error(ErrorCode::InvalidArgument, "no geometry");
    }
    if (!mr->material_asset.is_nil() && db) {
        if (auto mat = db->load_material(mr->material_asset)) {
            base = mat->base_color;
            metallic = mat->metallic;
            emissive = rgb(mat->emissive) * finite_nonneg(mat->emissive_strength, 1.0f);
        }
    }
    // Metals have no diffuse albedo; their bounce light is mostly specular, which v1 ignores.
    const float diffuse = 1.0f - std::clamp(std::isfinite(metallic) ? metallic : 0.0f, 0.0f, 1.0f);
    out.albedo = math::Vec3{base.r * mr->base_color.r, base.g * mr->base_color.g, base.b * mr->base_color.b} * diffuse;
    out.albedo = {std::clamp(out.albedo.x, 0.0f, 1.0f), std::clamp(out.albedo.y, 0.0f, 1.0f), std::clamp(out.albedo.z, 0.0f, 1.0f)};
    out.emissive = emissive;
    return out;
}

} // namespace

Result<render::MeshData> static_mesh_data(scene::Scene& scene, scene::Entity entity, assets::AssetDatabase* db) {
    auto src = gather(scene, entity, db, nullptr);
    if (!src) return std::move(src).error();
    return std::move(src->mesh);
}

std::uint64_t scene_lightmap_fingerprint(const render::MeshData& mesh, const math::Mat4& world) noexcept {
    std::uint64_t h = render::lightmap_mesh_fingerprint(mesh);
    for (const math::Vec4& col : world.c)
        for (const float v : {col.x, col.y, col.z, col.w}) {
            const double q = std::isfinite(v) ? std::round(static_cast<double>(v) * 1e4) : 0.0;
            const auto bits = static_cast<std::int64_t>(std::clamp(q, -9.0e15, 9.0e15));
            h ^= static_cast<std::uint64_t>(bits) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        }
    return h;
}

Result<PreparedLightmapBake> prepare_scene_lightmap(scene::Scene& scene, assets::AssetDatabase* db, const render::LightmapSettings& settings) {
    ecs::World& w = scene.world();
    PreparedLightmapBake out;
    out.settings = settings;
    std::map<Uuid, std::shared_ptr<assets::CookedModel>> models;
    std::uint32_t receivers = 0;
    w.each<scene::MeshRendererComponent, scene::WorldTransform>(
        [&](scene::Entity e, const scene::MeshRendererComponent& mr, const scene::WorldTransform& wt) {
            if (!mr.visible) return;
            const Uuid id = scene.uuid(e);
            auto src = gather(scene, e, db, &models);
            if (!src) {
                if (mr.lightmapped) {
                    const auto* n = w.get<scene::Name>(e);
                    out.warnings.push_back(std::format("{}: {}", n ? n->value : id.to_string(), src.error().message));
                }
                return;
            }
            PreparedLightmapBake::Item item;
            item.entity = id;
            item.mesh = std::move(src->mesh);
            item.model = wt.matrix;
            item.albedo = src->albedo;
            item.emissive = src->emissive;
            item.receives = mr.lightmapped && !id.is_nil();
            if (item.receives) ++receivers;
            out.items.push_back(std::move(item));
        });
    if (receivers == 0) return make_error(ErrorCode::InvalidArgument, "no visible MeshRenderer is marked lightmapped (static lighting)");

    bool has_env = false;
    w.each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) {
        if (has_env) return;
        has_env = true;
        out.sky.sky = rgb(e.sky_zenith);
        out.sky.horizon = rgb(e.sky_horizon);
        out.sky.ground = rgb(e.ground);
        out.sky.intensity = finite_nonneg(e.ambient_intensity, 0.6f);
        if (!e.environment_map.is_nil() || e.physical_sky)
            out.warnings.push_back("the Environment's environment map / physical sky is not baked yet; the analytic sky colours are used");
    });
    if (!has_env) {
        // Same defaults the renderer uses without an Environment component.
        const render::Environment env;
        out.sky.sky = rgb(env.sky_zenith);
        out.sky.horizon = rgb(env.sky_horizon);
        out.sky.ground = rgb(env.ground);
        out.sky.intensity = env.ambient_intensity;
    }
    bool has_sun = false;
    w.each<scene::LightComponent, scene::WorldTransform>([&](const scene::LightComponent& l, const scene::WorldTransform& wt) {
        render::LightmapLight light;
        light.radiance = rgb(l.color) * finite_nonneg(l.intensity, 0.0f);
        light.cast_shadows = l.shadows;
        light.range = finite_nonneg(l.range, 10.0f);
        light.position = wt.matrix.transform_point({});
        light.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
        if (l.type == scene::LightComponent::Type::Directional) {
            if (has_sun) return; // the renderer uses the first directional light only
            has_sun = true;
            light.type = render::LightmapLight::Type::Directional;
        } else if (l.type == scene::LightComponent::Type::Spot) {
            light.type = render::LightmapLight::Type::Spot;
            const float outer = std::isfinite(l.outer_angle_degrees) ? std::clamp(l.outer_angle_degrees, 0.1f, 89.5f) : 30.0f;
            const float inner = std::isfinite(l.inner_angle_degrees) ? std::clamp(l.inner_angle_degrees, 0.0f, outer) : outer;
            light.cos_outer = std::cos(math::radians(outer));
            light.cos_inner = std::cos(math::radians(inner));
        } else {
            light.type = render::LightmapLight::Type::Point;
        }
        out.lights.push_back(light);
    });
    return out;
}

Result<SceneLightmapBake> bake_prepared_lightmap(const PreparedLightmapBake& prepared, jobs::JobSystem* jobs) {
    SceneLightmapBake out;
    out.warnings = prepared.warnings;
    render::LightmapScene ls;
    ls.instances.reserve(prepared.items.size());
    for (const auto& it : prepared.items) {
        render::LightmapInstance inst;
        inst.mesh = &it.mesh;
        inst.model = it.model;
        inst.albedo = it.albedo;
        inst.emissive = it.emissive;
        inst.receives = it.receives;
        ls.instances.push_back(inst);
        if (it.receives) ++out.receivers;
        else ++out.blockers;
    }
    if (out.receivers == 0) return make_error(ErrorCode::InvalidArgument, "no visible MeshRenderer is marked lightmapped (static lighting)");
    ls.lights = prepared.lights;
    ls.sky = prepared.sky;
    out.lights = static_cast<std::uint32_t>(ls.lights.size());
    auto bake = render::bake_lightmaps(ls, prepared.settings, jobs);
    if (!bake) return std::move(bake).error();
    out.stats = bake->stats;
    out.file.atlas = std::move(bake->atlas);
    out.file.direction = std::move(bake->direction);
    out.file.shadowmask_direction = bake->shadowmask_direction;
    for (std::size_t i = 0; i < prepared.items.size(); ++i) {
        if (!prepared.items[i].receives || bake->instances[i].empty()) continue;
        render::LightmapFileEntry entry;
        entry.entity = prepared.items[i].entity;
        entry.mesh_fingerprint = scene_lightmap_fingerprint(prepared.items[i].mesh, prepared.items[i].model);
        entry.unwrap = std::move(bake->instances[i]);
        out.file.entries.push_back(std::move(entry));
    }
    return out;
}

Result<SceneLightmapBake> bake_scene_lightmap(scene::Scene& scene, assets::AssetDatabase* db, const render::LightmapSettings& settings,
                                              jobs::JobSystem* jobs) {
    auto prepared = prepare_scene_lightmap(scene, db, settings);
    if (!prepared) return std::move(prepared).error();
    return bake_prepared_lightmap(*prepared, jobs);
}

std::string default_lightmap_path(std::string_view scene_path) {
    std::string s(scene_path);
    const auto slash = s.find_last_of("/\\");
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) s.resize(dot);
    return s + std::string(kLightmapExtension);
}

} // namespace victus::runtime
