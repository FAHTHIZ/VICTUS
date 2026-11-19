// SPDX-License-Identifier: Apache-2.0
// Runtime glue between scenes, cooked assets and the renderer:
// - GpuAssetCache uploads cooked textures/models (and built-in primitives) once per device.
// - instantiate_model() (from victus/assets/instantiate.hpp) turns a cooked model into entities.
// - extract_scene() feeds a scene's cameras, lights, meshes and sprites to the renderers.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/assets/async_loader.hpp"
#include "victus/assets/database.hpp"
#include "victus/assets/instantiate.hpp"
#include "victus/render/atmosphere.hpp"
#include "victus/render/irradiance_bake.hpp"
#include "victus/render/irradiance_volume.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/scene/scene.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::runtime {

// V.A.U.L.T. asynchronous loading (GpuAssetCache::set_async_loading).
struct AsyncLoadingSettings {
    // Decoded bytes uploaded per pump() (per rendered frame). At least one asset is uploaded per
    // pump even when it alone exceeds the budget.
    std::size_t upload_budget_bytes{8u << 20};
    std::uint32_t max_in_flight{4}; // decodes running on workers at once
};

struct AsyncLoadingStats {
    std::uint32_t pending{0};         // requested, not uploaded yet (queued + decoding + decoded)
    std::uint32_t uploaded_last_pump{0};
    std::size_t uploaded_bytes_last_pump{0};
    std::uint64_t uploaded{0}, failed{0}; // totals
};

// V.A.U.L.T. residency (GpuAssetCache::set_residency, ADR-0088).
struct ResidencySettings {
    std::size_t budget_bytes{0};       // resident texture + model bytes allowed; 0 = unlimited
    std::uint32_t min_idle_frames{60}; // pumps an asset must go unused before it may be evicted
    // Mip streaming (texture assets used by .vmat materials and decals): each texture keeps only
    // the mip levels its largest on-screen footprint needs, measured against screen_height.
    bool mip_streaming{false};
    std::uint32_t screen_height{1080};
    std::uint32_t min_streamed_size{64}; // never drop below this edge length (smaller textures never stream)
    std::uint32_t mip_drop_frames{30};   // pumps a texture must want less detail before it drops levels
    std::uint32_t restreams_per_pump{4}; // level changes started per pump
};

struct ResidencyStats {
    std::size_t resident_bytes{0};
    std::uint32_t resident_assets{0};
    std::uint64_t evicted{0};       // total assets evicted for the budget
    std::size_t evicted_bytes{0};   // total bytes evicted for the budget
    bool over_budget{false};        // every asset over the budget was in recent use
    std::uint64_t frame{0};         // pump() count, the residency clock
    std::uint32_t streamed_textures{0}; // resident textures below their full resolution
    std::uint64_t restreams{0};         // total mip-level changes applied
    std::uint64_t budget_drops{0};      // total level drops forced by the budget (in-use textures)
};

// Approximate on-screen size in pixels of a world-space bounding box seen by `camera` on a
// screen `screen_height` pixels tall (the diameter of its bounding sphere, nearest point).
[[nodiscard]] float projected_pixels(const render::Camera3D& camera, const math::Aabb& world_bounds,
                                     std::uint32_t screen_height) noexcept;

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
    // glTF node.weights by node name (nodes overriding their mesh's default morph weights).
    std::vector<std::pair<std::string, std::vector<float>>> node_morph_weights;
};

// Uploads a cooked texture: the first block-compressed variant the device can sample (in the
// cooker's preference order) when `allow_compressed`, otherwise the RGBA8 mips. `chosen`
// receives the uploaded codec, or nullopt for RGBA8.
[[nodiscard]] Result<rhi::TextureHandle> upload_cooked_texture(render::GpuContext& ctx, const assets::CookedTexture& texture,
                                                               bool allow_compressed = true,
                                                               std::optional<assets::TextureCodec>* chosen = nullptr,
                                                               std::uint32_t base_level = 0);

class GpuAssetCache {
public:
    GpuAssetCache(render::GpuContext& ctx, assets::AssetDatabase* db) : ctx_(ctx), db_(db) {}
    ~GpuAssetCache();
    GpuAssetCache(const GpuAssetCache&) = delete;
    GpuAssetCache& operator=(const GpuAssetCache&) = delete;

    // With asynchronous loading on, an asset not resident yet returns ErrorCode::Pending (and is
    // requested); callers skip or draw without it and ask again next frame. Failures are cached
    // with their message until evict()/clear(). Without it, the first call loads synchronously.
    [[nodiscard]] Result<rhi::TextureHandle> texture(const Uuid& uuid);
    // V.A.U.L.T.: texture and model loads read and decode on Background jobs of `jobs`, and
    // pump() uploads finished ones under a byte budget. nullptr returns to synchronous loading
    // (waiting for decodes in flight). The job system must outlive the cache or be detached
    // first. Environment maps (HDR bakes) stay synchronous. The database must not be
    // refresh()ed while loads are in flight: call wait_loads() first.
    void set_async_loading(jobs::JobSystem* jobs, const AsyncLoadingSettings& settings = {});
    [[nodiscard]] bool async_loading() const noexcept { return loader_ != nullptr; }
    // Uploads decoded assets within the budget; returns the uploads made. extract_scene() and
    // extract_sprites() call it, so the budget applies per rendered view.
    std::uint32_t pump();
    // Blocks until every requested asset is resident or failed (loading screens, tests, captures).
    void finish_loads();
    // Blocks until no decode is running (before AssetDatabase::refresh()).
    void wait_loads();
    [[nodiscard]] AsyncLoadingStats async_stats() const;
    // Residency: every resident texture/model is accounted (uploaded bytes) and stamped when used
    // (texture()/model() hits, and cached materials touching their textures). Over the budget,
    // pump() evicts assets unused for `min_idle_frames` pumps, least recently used first; assets
    // in recent use are never evicted (over_budget reports it). Evicted assets reload on their
    // next use. GPU memory is released by the RHI once in-flight frames are done with it.
    void set_residency(const ResidencySettings& settings) noexcept { residency_settings_ = settings; }
    [[nodiscard]] const ResidencySettings& residency_settings() const noexcept { return residency_settings_; }
    [[nodiscard]] ResidencyStats residency() const;
    // Starts loading without using the result (scene preloading); no-op when synchronous.
    void prefetch(const Uuid& uuid, assets::LoadType type, assets::LoadPriority priority = assets::LoadPriority::Low);
    // True when the asset is resident, failed, or loads synchronously on first use (nothing to
    // wait for); false while an async load is queued or decoding.
    [[nodiscard]] bool settled(const Uuid& uuid, assets::LoadType type) const;
    // Mip streaming input: a texture (or every texture of a material, scaled by its UV tiling)
    // covers about `pixels` on screen this frame. The largest footprint per frame wins.
    void note_footprint(const Uuid& texture, float pixels);
    void note_material_footprint(const Uuid& material, float pixels);
    // The textures embedded in a resident model (glTF) used by one of its meshes.
    void note_model_footprint(const Uuid& model, std::uint32_t mesh, float pixels);
    // A mesh of a model that is reloading after an eviction: the bounds and texture use kept
    // from its last residency give the footprint, so the reload uploads only the levels needed.
    void note_reloading_model(const Uuid& model, std::uint32_t mesh, const math::Mat4& world, const render::Camera3D& camera);
    // Resident top mip level of a texture asset (0 = full resolution); nullopt when not resident.
    [[nodiscard]] std::optional<std::uint32_t> texture_level(const Uuid& uuid) const;
    // Same for texture `index` embedded in a resident model.
    [[nodiscard]] std::optional<std::uint32_t> model_texture_level(const Uuid& model, std::uint32_t index) const;
    // Streaming key of an embedded model texture (stable for a model uuid and texture index).
    [[nodiscard]] static Uuid embedded_texture_key(const Uuid& model, std::uint32_t index) noexcept;
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
    // Per-drawer morphed copy of model mesh `mesh` (morph targets applied on the CPU with
    // `weights`), keyed by `key` (the drawing entity). Re-uploaded only when the weights change;
    // shares the base mesh's index, LOD and skin buffers. nullptr when the mesh has no targets,
    // every weight is 0, or the upload failed (draw the base mesh). Copies unused for a few
    // frames are destroyed by collect_procedural().
    [[nodiscard]] const render::GpuMesh* morphed(std::uint64_t key, const Uuid& model_id, const GpuModel& model,
                                                 std::uint32_t mesh, std::span<const float> weights);
    [[nodiscard]] std::size_t morphed_count() const noexcept { return morphed_.size(); }
    [[nodiscard]] std::uint64_t morph_uploads() const noexcept { return morph_uploads_; }
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
    struct MorphEntry {
        Uuid model;
        std::uint32_t mesh{0};
        std::vector<float> weights;
        render::GpuMesh gpu;
        std::uint64_t last_used{0};
    };
    std::unordered_map<std::uint64_t, MorphEntry> morphed_;
    std::vector<render::Vertex3D> morph_scratch_;
    std::uint64_t morph_uploads_{0};
    void drop_morphed(const Uuid* model); // all copies, or those of one model
    std::uint64_t procedural_clock_{0};
    struct Resident {
        std::size_t bytes{0};
        std::uint64_t last_used{0};
    };
    std::unordered_map<Uuid, Resident, UuidHash> resident_;
    std::unordered_map<Uuid, std::vector<Uuid>, UuidHash> material_textures_; // texture refs per material
    ResidencySettings residency_settings_;
    ResidencyStats residency_stats_;
    void touch(const Uuid& uuid) noexcept;
    void make_resident(const Uuid& uuid, std::size_t bytes);
    void enforce_residency();
    struct Streamed {
        std::uint32_t width{0}, height{0}, levels{0}; // full resolution; 0 until resident
        std::uint32_t level{0};                         // resident top level
        std::vector<std::size_t> tail_bytes;            // bytes uploaded when starting at level i
        float seen_pixels{0.0f};
        std::uint64_t seen_frame{~0ull}; // frame the footprint was noted in
        std::uint64_t lower_since{0};    // frame the texture started wanting fewer levels (0 = no)
        std::int32_t pending{-1};        // level requested from the loader (async)
    };
    std::unordered_map<Uuid, Streamed, UuidHash> stream_;
    [[nodiscard]] std::uint32_t wanted_level(const Streamed& s) const noexcept;
    [[nodiscard]] std::uint32_t max_level(const Streamed& s) const noexcept;
    void stream_mips();
    void restream(const Uuid& uuid, std::uint32_t level);
    // Embedded model textures stream under embedded_texture_key(); the registry maps keys back
    // to (model, index). Entries are never removed (keys are deterministic), so late loader
    // results for an evicted model are recognised and dropped. Shared with loader workers.
    struct EmbeddedRef {
        Uuid model;
        std::uint32_t index{0};
    };
    struct EmbeddedRegistry {
        mutable std::mutex mutex;
        std::unordered_map<Uuid, EmbeddedRef, UuidHash> refs;
        [[nodiscard]] std::optional<EmbeddedRef> find(const Uuid& key) const;
    };
    std::shared_ptr<EmbeddedRegistry> embedded_{std::make_shared<EmbeddedRegistry>()};
    // The live handle a streamed texture key refers to (texture asset or model texture slot);
    // `owner` receives the asset whose residency accounts it.
    [[nodiscard]] rhi::TextureHandle* streamed_handle(const Uuid& key, Uuid* owner = nullptr);
    void forget_model_streams(const GpuModel& model, const Uuid& uuid);
    struct ModelHint { // per mesh, kept after eviction
        std::vector<math::Aabb> bounds;
        std::vector<std::vector<std::uint32_t>> textures;
    };
    std::unordered_map<Uuid, ModelHint, UuidHash> model_hints_;
    [[nodiscard]] static std::vector<std::uint32_t> mesh_textures(const assets::CookedModel& model, std::uint32_t mesh);
    void replace_texture(const Uuid& uuid, const assets::CookedTexture& cooked, std::uint32_t level);
    std::unique_ptr<assets::AsyncLoader> loader_;
    AsyncLoadingSettings async_settings_;
    AsyncLoadingStats async_stats_;
    [[nodiscard]] Result<rhi::TextureHandle> upload_texture(const Uuid& uuid, const assets::CookedTexture& cooked);
};

// instantiate_model() moved to victus/assets/instantiate.hpp; runtime:: keeps the name.
using assets::instantiate_model;

struct ExtractStats {
    std::uint32_t meshes{0}, sprites{0}, lights{0}, missing_assets{0};
    std::uint32_t reflection_probes{0}; // captured probes handed to the renderer
    std::uint32_t irradiance_volumes{0}; // volumes with baked probes in the scene
    std::uint32_t irradiance_probes_baked{0}; // probes baked during this call
    std::uint32_t irradiance_probes_refreshed{0}; // dynamic-volume probes re-captured during this call
    std::uint32_t skinned{0}; // meshes drawn with GPU skinning
    std::uint32_t morphed{0}; // meshes drawn with morph targets applied
    std::uint32_t terrain_chunks{0}; // terrain chunk meshes submitted (before culling)
    std::uint32_t foliage_instances{0}; // foliage instances submitted (within draw distance, before culling)
    std::uint32_t particle_emitters{0}, particles{0}; // submitted (before camera culling)
    std::uint32_t particle_trails{0};                 // ribbon strips submitted
    std::uint32_t decals{0}; // visible decal components submitted (before camera culling)
    std::uint32_t loading_assets{0}; // references skipped because their asset is still loading (async)
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

// Enables 2D lighting on `batch` when the scene has an enabled Light2D (ambient = sum of Global
// lights) and adds its Point/Spot lights; returns the number of local lights. Called by extract_sprites.
std::uint32_t extract_lights_2d(scene::Scene& scene, render::SpriteBatch& batch);
// Draws SpriteComponents, tilemaps and 2D particles (world units, XY plane) into a SpriteBatch in
// layer order, lit by the scene's 2D lights (see extract_lights_2d).
std::uint32_t extract_sprites(scene::Scene& scene, GpuAssetCache& assets, render::SpriteBatch& batch);

} // namespace victus::runtime
