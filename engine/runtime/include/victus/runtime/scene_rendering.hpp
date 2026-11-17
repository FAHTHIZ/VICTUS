// SPDX-License-Identifier: Apache-2.0
// Runtime glue between scenes, cooked assets and the renderer:
// - GpuAssetCache uploads cooked textures/models (and built-in primitives) once per device.
// - instantiate_model() turns a cooked model's node graph into scene entities.
// - extract_scene() feeds a scene's cameras, lights, meshes and sprites to the renderers.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/assets/database.hpp"
#include "victus/render/atmosphere.hpp"
#include "victus/render/irradiance_bake.hpp"
#include "victus/render/irradiance_volume.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/scene/scene.hpp"

#include <memory>
#include <optional>
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

// Uploads a cooked texture: the first block-compressed variant the device can sample (in the
// cooker's preference order) when `allow_compressed`, otherwise the RGBA8 mips. `chosen`
// receives the uploaded codec, or nullopt for RGBA8.
[[nodiscard]] Result<rhi::TextureHandle> upload_cooked_texture(render::GpuContext& ctx, const assets::CookedTexture& texture,
                                                               bool allow_compressed = true,
                                                               std::optional<assets::TextureCodec>* chosen = nullptr);

class GpuAssetCache {
public:
    GpuAssetCache(render::GpuContext& ctx, assets::AssetDatabase* db) : ctx_(ctx), db_(db) {}
    ~GpuAssetCache();
    GpuAssetCache(const GpuAssetCache&) = delete;
    GpuAssetCache& operator=(const GpuAssetCache&) = delete;

    [[nodiscard]] Result<rhi::TextureHandle> texture(const Uuid& uuid);
    // True when the texture's import settings ask for nearest ("filter": "nearest") sampling.
    [[nodiscard]] bool texture_nearest(const Uuid& uuid);
    // HDR texture asset baked into an image-based-lighting environment map (created on first
    // use and cached; failures are cached and logged once until the asset is evicted).
    [[nodiscard]] Result<const render::EnvironmentMap*> environment(const Uuid& uuid);
    void set_environment_bake(const render::EnvironmentBakeSettings& s) noexcept { env_bake_ = s; }
    // Physical sky baked into an environment map; re-baked only when the settings change (the
    // sun by more than about half a degree), so animated time of day stays cheap.
    [[nodiscard]] Result<const render::EnvironmentMap*> atmosphere(const render::AtmosphereSettings& s);
    [[nodiscard]] std::uint32_t atmosphere_bakes() const noexcept { return atmosphere_bakes_; }
    // Block-compressed uploads (on by default; off forces RGBA8 for every texture loaded later).
    void set_compressed_textures(bool enabled) noexcept { allow_compressed_ = enabled; }
    [[nodiscard]] std::uint32_t compressed_texture_count() const noexcept { return compressed_uploads_; }

    // Reflection probe captures (managed by extract_scene, keyed by entity).
    struct ProbeCapture {
        std::unique_ptr<render::EnvironmentMap> map;
        math::Vec3 position{};
        std::uint32_t revision{0}, resolution{0};
        bool seen{false};
    };
    [[nodiscard]] std::unordered_map<std::uint64_t, ProbeCapture>& probe_captures() noexcept { return probes_; }
    [[nodiscard]] std::uint32_t probe_capture_count() const noexcept { return probe_capture_count_; }
    void note_probe_capture() noexcept { ++probe_capture_count_; }
    [[nodiscard]] bool capturing_probes() const noexcept { return capturing_probes_; }
    void set_capturing_probes(bool on) noexcept { capturing_probes_ = on; }
    [[nodiscard]] Result<render::SceneRenderer*> probe_renderer();
    // Captures allowed per extract_scene call (spreads probe captures over frames; 0 = none).
    void set_probe_captures_per_frame(std::uint32_t n) noexcept { probe_budget_ = n; }
    [[nodiscard]] std::uint32_t probe_captures_per_frame() const noexcept { return probe_budget_; }
    // Irradiance volumes (managed by extract_scene, keyed by entity). A rebake fills `pending`
    // progressively while `active` (the last finished bake) keeps lighting the scene.
    struct IrradianceBake {
        std::unique_ptr<render::IrradianceVolume> active, pending;
        render::IrradianceBaker baker;
        render::IrradianceRefresher refresher; // dynamic volumes, after the bake finished
        render::IrradianceGrid grid;
        std::uint32_t revision{0}, resolution{0}, bounces{0};
        float normal_offset{0.0f};
        bool seen{false};
    };
    [[nodiscard]] std::unordered_map<std::uint64_t, IrradianceBake>& irradiance_bakes() noexcept { return irradiance_; }
    // Probes baked per extract_scene call, across all volumes (0 = no baking).
    void set_irradiance_probes_per_frame(std::uint32_t n) noexcept { irradiance_budget_ = n; }
    [[nodiscard]] std::uint32_t irradiance_probes_per_frame() const noexcept { return irradiance_budget_; }
    [[nodiscard]] std::uint32_t irradiance_bakes_completed() const noexcept { return irradiance_completed_; }
    void note_irradiance_bake() noexcept { ++irradiance_completed_; }
    [[nodiscard]] const render::EnvironmentBakeSettings& environment_bake() const noexcept { return env_bake_; }
    [[nodiscard]] Result<const GpuModel*> model(const Uuid& uuid);
    // Registers an already-cooked model under a UUID (tools, tests, procedural content).
    [[nodiscard]] Result<const GpuModel*> add_model(const Uuid& uuid, assets::CookedModel model);
    // Material asset (.vmat) resolved to a render material with its textures uploaded. Results
    // (including failures) are cached until evict()/clear(), so a broken file is read once, not
    // every frame. Missing or non-texture references leave the slot empty (white/flat default).
    [[nodiscard]] Result<render::Material> material(const Uuid& uuid);
    // Drops cached materials only (after a .vmat changed on disk).
    void evict_materials() noexcept { materials_.clear(); }
    // Surface shader asset (.vshader) compiled into a fragment shader. Failures are cached (and
    // logged once) like materials; materials using a broken shader fall back to built-in PBR.
    [[nodiscard]] Result<rhi::ShaderHandle> surface_shader(const Uuid& uuid);
    // Drops compiled surface shaders and the materials using them (after a .vshader changed).
    void evict_shaders();
    // The built-in terrain surface shader (terrain::terrain_surface_code()), compiled on first use.
    [[nodiscard]] Result<rhi::ShaderHandle> terrain_shader();
    // Built-in primitive mesh: "cube", "sphere", "plane", "cylinder". nullptr when unknown.
    [[nodiscard]] const render::GpuMesh* primitive(std::string_view name);
    // GPU copy of a procedural mesh (scene::ProceduralMeshComponent), uploaded on first use and
    // cached by the mesh's id. nullptr for an empty mesh or a failed upload (logged once).
    [[nodiscard]] const render::GpuMesh* procedural(const scene::ProceduralMesh& mesh);
    // Advances the procedural-mesh clock and destroys GPU copies not drawn during the last
    // `keep_frames` frames (meshes replaced or removed by their entities). extract_scene() calls it.
    void collect_procedural(std::uint32_t keep_frames = 8);
    [[nodiscard]] std::size_t procedural_count() const noexcept { return procedural_.size(); }
    // Drops one asset (e.g. after re-import) so the next request reloads it.
    void evict(const Uuid& uuid);
    void clear();
    [[nodiscard]] render::GpuContext& context() noexcept { return ctx_; }
    [[nodiscard]] assets::AssetDatabase* database() noexcept { return db_; }

private:
    render::GpuContext& ctx_;
    assets::AssetDatabase* db_;
    std::unordered_map<Uuid, rhi::TextureHandle, UuidHash> textures_;
    std::unordered_map<Uuid, bool, UuidHash> nearest_;
    std::unordered_map<Uuid, std::unique_ptr<render::EnvironmentMap>, UuidHash> environments_;
    std::unordered_map<Uuid, Error, UuidHash> environment_errors_;
    render::EnvironmentBakeSettings env_bake_;
    std::unique_ptr<render::EnvironmentMap> atmosphere_;
    render::AtmosphereSettings atmosphere_key_;
    std::uint32_t atmosphere_bakes_{0};
    bool allow_compressed_{true};
    std::uint32_t compressed_uploads_{0};
    std::unordered_map<std::uint64_t, ProbeCapture> probes_;
    std::unique_ptr<render::SceneRenderer> probe_renderer_;
    std::uint32_t probe_capture_count_{0}, probe_budget_{1};
    bool capturing_probes_{false};
    std::unordered_map<std::uint64_t, IrradianceBake> irradiance_;
    std::uint32_t irradiance_budget_{8}, irradiance_completed_{0};
    std::unordered_map<Uuid, std::unique_ptr<GpuModel>, UuidHash> models_;
    std::unordered_map<std::string, render::GpuMesh> primitives_;
    std::unordered_map<Uuid, Result<render::Material>, UuidHash> materials_;
    std::unordered_map<Uuid, Result<rhi::ShaderHandle>, UuidHash> shaders_;
    std::optional<Result<rhi::ShaderHandle>> terrain_shader_;
    struct ProceduralEntry {
        render::GpuMesh mesh;
        std::uint64_t last_used{0};
        bool failed{false};
    };
    std::unordered_map<std::uint64_t, ProceduralEntry> procedural_;
    std::uint64_t procedural_clock_{0};
};

// Creates one entity per model node under `parent` (a new root named `name` when parent is
// null) with MeshRenderer components referencing (model_uuid, mesh index). Meshes of skinned
// nodes get a SkinnedMesh bound to the root; models with animations get an Animator on the root
// playing the first clip. Returns the root.
scene::Entity instantiate_model(scene::Scene& scene, const Uuid& model_uuid, const assets::CookedModel& model,
                                std::string_view name, scene::Entity parent = scene::kNullEntity);

struct ExtractStats {
    std::uint32_t meshes{0}, sprites{0}, lights{0}, missing_assets{0};
    std::uint32_t reflection_probes{0}; // captured probes handed to the renderer
    std::uint32_t irradiance_volumes{0}; // volumes with baked probes in the scene
    std::uint32_t irradiance_probes_baked{0}; // probes baked during this call
    std::uint32_t irradiance_probes_refreshed{0}; // dynamic-volume probes re-captured during this call
    std::uint32_t skinned{0}; // meshes drawn with GPU skinning
    std::uint32_t terrain_chunks{0}; // terrain chunk meshes submitted (before culling)
    std::uint32_t particle_emitters{0}, particles{0}; // submitted (before camera culling)
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
