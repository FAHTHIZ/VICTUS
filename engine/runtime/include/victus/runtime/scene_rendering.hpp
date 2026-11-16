// SPDX-License-Identifier: Apache-2.0
// Runtime glue between scenes, cooked assets and the renderer:
// - GpuAssetCache uploads cooked textures/models (and built-in primitives) once per device.
// - instantiate_model() turns a cooked model's node graph into scene entities.
// - extract_scene() feeds a scene's cameras, lights, meshes and sprites to the renderers.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/assets/database.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/scene/scene.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::runtime {

struct GpuModel {
    std::vector<render::GpuMesh> meshes;
    std::vector<render::Material> mesh_materials; // one per mesh, textures resolved
    std::vector<rhi::TextureHandle> textures;
    assets::CookedModel cpu; // node graph + materials (vertex data cleared after upload)
    // GPU skinning metadata, per mesh: bind-space bounds of the vertices influenced by each joint
    // index (empty for unskinned meshes) and the highest joint index used (-1 = unskinned).
    std::vector<std::vector<math::Aabb>> joint_bounds;
    std::vector<std::int32_t> max_joint;
    std::vector<animation::AnimationSet::Node> skeleton; // node names/parents for joint binding
};

class GpuAssetCache {
public:
    GpuAssetCache(render::GpuContext& ctx, assets::AssetDatabase* db) : ctx_(ctx), db_(db) {}
    ~GpuAssetCache();
    GpuAssetCache(const GpuAssetCache&) = delete;
    GpuAssetCache& operator=(const GpuAssetCache&) = delete;

    [[nodiscard]] Result<rhi::TextureHandle> texture(const Uuid& uuid);
    [[nodiscard]] Result<const GpuModel*> model(const Uuid& uuid);
    // Registers an already-cooked model under a UUID (tools, tests, procedural content).
    [[nodiscard]] Result<const GpuModel*> add_model(const Uuid& uuid, assets::CookedModel model);
    // Built-in primitive mesh: "cube", "sphere", "plane", "cylinder". nullptr when unknown.
    [[nodiscard]] const render::GpuMesh* primitive(std::string_view name);
    // Drops one asset (e.g. after re-import) so the next request reloads it.
    void evict(const Uuid& uuid);
    void clear();
    [[nodiscard]] render::GpuContext& context() noexcept { return ctx_; }
    [[nodiscard]] assets::AssetDatabase* database() noexcept { return db_; }

private:
    render::GpuContext& ctx_;
    assets::AssetDatabase* db_;
    std::unordered_map<Uuid, rhi::TextureHandle, UuidHash> textures_;
    std::unordered_map<Uuid, std::unique_ptr<GpuModel>, UuidHash> models_;
    std::unordered_map<std::string, render::GpuMesh> primitives_;
};

// Creates one entity per model node under `parent` (a new root named `name` when parent is
// null) with MeshRenderer components referencing (model_uuid, mesh index). Meshes of skinned
// nodes get a SkinnedMesh bound to the root; models with animations get an Animator on the root
// playing the first clip. Returns the root.
scene::Entity instantiate_model(scene::Scene& scene, const Uuid& model_uuid, const assets::CookedModel& model,
                                std::string_view name, scene::Entity parent = scene::kNullEntity);

struct ExtractStats {
    std::uint32_t meshes{0}, sprites{0}, lights{0}, missing_assets{0};
    std::uint32_t skinned{0}; // meshes drawn with GPU skinning
    bool camera_found{false};
};

struct SceneViewSettings {
    render::Environment environment;
    // Used when the scene has no primary camera.
    render::Camera3D fallback_camera = render::Camera3D::look_at({6, 5, 8}, {0, 0.5f, 0});
};

// Copies an Environment component into renderer environment/sun settings (clamped).
void apply_environment(const gameplay::EnvironmentComponent& component, render::Environment& env,
                       render::DirectionalLight& sun);

// Requires up-to-date WorldTransforms (call scene.update_transforms() first). Calls
// renderer.begin() and submits everything; the caller then calls renderer.render().
ExtractStats extract_scene(scene::Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                           const SceneViewSettings& settings = {}, const render::Camera3D* camera_override = nullptr);

// Draws SpriteComponents (world units, XY plane) into a SpriteBatch in layer order.
std::uint32_t extract_sprites(scene::Scene& scene, GpuAssetCache& assets, render::SpriteBatch& batch);

} // namespace victus::runtime
