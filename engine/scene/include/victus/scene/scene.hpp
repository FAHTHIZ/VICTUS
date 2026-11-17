// SPDX-License-Identifier: Apache-2.0
// Scene: an ECS world plus the standard components every VICTUS project shares (identity,
// names, hierarchy, transforms, cameras, lights, mesh/sprite renderers) and a versioned,
// diff-friendly JSON file format with stable UUIDs.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/ecs/world.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::scene {

using ecs::Entity;
using ecs::kNullEntity;

// ---- standard components ----------------------------------------------------------------

struct Id {
    Uuid uuid;
};
struct Name {
    std::string value;
};
struct Transform {
    math::Vec3 position;
    math::Quat rotation;
    math::Vec3 scale{1, 1, 1};
    [[nodiscard]] math::Mat4 matrix() const { return math::Mat4::trs(position, rotation, scale); }
};
// Intrusive child list; maintained exclusively by Scene::set_parent/destroy.
struct Hierarchy {
    Entity parent;
    Entity first_child;
    Entity last_child;
    Entity next_sibling;
    Entity prev_sibling;
    std::uint32_t depth{0};
};
// Computed each frame by Scene::update_transforms().
struct WorldTransform {
    math::Mat4 matrix;
};
struct CameraComponent {
    enum class Projection : std::uint8_t { Perspective, Orthographic };
    Projection projection{Projection::Perspective};
    float fov_y_degrees{60.0f};
    float ortho_height{10.0f};
    float z_near{0.1f};
    float z_far{500.0f};
    bool primary{true};
    // 2D: rounds the camera position to whole screen pixels so pixel art does not shimmer.
    bool pixel_snap{false};
};
struct LightComponent {
    enum class Type : std::uint8_t { Directional, Point, Spot };
    Type type{Type::Directional};
    math::Color color{1, 1, 1, 1}; // linear
    float intensity{3.0f};
    float range{10.0f};
    bool shadows{true};
    // Spot cone half-angles in degrees (full intensity inside `inner`, zero beyond `outer`);
    // spots shine along the entity's local -Z axis.
    float inner_angle_degrees{20.0f};
    float outer_angle_degrees{30.0f};
};
// References a mesh inside an asset (glTF/model) by asset UUID and sub-mesh index, plus a
// material override index (-1 = mesh's own material).
struct MeshRendererComponent {
    Uuid mesh_asset;
    std::uint32_t mesh_index{0};
    std::string primitive; // built-in primitive when mesh_asset is nil: cube|sphere|plane|cylinder
    // Optional material asset (.vmat). When set it replaces the mesh's own material (and the
    // metallic/roughness below); base_color still multiplies it as a per-entity tint.
    Uuid material_asset;
    math::Color base_color{1, 1, 1, 1}; // linear multiplier
    float metallic{0.0f};
    float roughness{0.5f};
    bool cast_shadows{true};
    bool visible{true};
};
// Geometry built at run time (scripts, simulation bridges, tools) instead of loaded from an
// asset. Shared and immutable once published: replace the pointer to change it. `id` is unique
// per process (never reused), so renderers cache GPU copies by it. Colours are packed RGBA8 with
// red in the lowest byte, sRGB RGB and linear alpha (render::Vertex3D::color); empty normals are
// computed (smooth) and empty colours are white.
struct ProceduralMesh {
    std::uint64_t id{0};
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;  // empty or one per position
    std::vector<math::Vec2> uvs;      // empty or one per position
    std::vector<std::uint32_t> colors; // empty or one per position
    std::vector<std::uint32_t> indices; // triangle list
    // Coarser levels of detail over the same vertices (coarsest last), with the geometric error
    // of each level in local units; the renderer projects it to pixels to pick a level.
    struct Lod {
        std::vector<std::uint32_t> indices;
        float error{0.0f};
    };
    std::vector<Lod> lods;
    math::Aabb bounds;
};
// Assigns a fresh id and computes bounds (and smooth normals when none were given), then
// shares the mesh. Fails on out-of-range indices, mismatched attribute counts or an index count
// that is not a multiple of three.
[[nodiscard]] Result<std::shared_ptr<const ProceduralMesh>> publish_mesh(ProceduralMesh mesh);
// Drawn through the entity's MeshRenderer (material, tint, shadows, visibility) in place of
// its mesh asset or primitive. Runtime-only: not saved with the scene.
struct ProceduralMeshComponent {
    std::shared_ptr<const ProceduralMesh> mesh;
};
struct SpriteComponent {
    Uuid texture_asset;
    math::Vec2 size{1, 1};
    math::Color tint{1, 1, 1, 1};
    math::Vec2 pivot{0.5f, 0.5f};
    std::int32_t layer{0};
    bool flip_x{false};
    bool flip_y{false};
    // Region of the texture drawn: u0, v0 (top), u1, v1 (bottom). SpriteAnimator sets it to the
    // current sheet frame; set it by hand to draw one cell of an atlas.
    math::Vec4 uv{0, 0, 1, 1};
};
// Components present in a scene file whose type is not registered in this build are kept
// verbatim so saving never destroys data authored with plugins or newer engine versions.
struct UnknownComponents {
    json::Object components;
};

// ---- serialization registry ----------------------------------------------------------------

struct ComponentSerializer {
    std::string name;
    ecs::ComponentId id{0};
    std::function<json::Value(const void*)> save;
    std::function<Status(void*, const json::Value&)> load;
};

class SerializerRegistry {
public:
    static SerializerRegistry& instance();
    void add(ComponentSerializer serializer);
    [[nodiscard]] const ComponentSerializer* by_name(std::string_view name) const;
    [[nodiscard]] const ComponentSerializer* by_id(ecs::ComponentId id) const;
    [[nodiscard]] const std::vector<ComponentSerializer>& all() const noexcept { return serializers_; }

    template <typename T>
    void add(std::string name, std::function<json::Value(const T&)> save,
             std::function<Status(T&, const json::Value&)> load) {
        ComponentSerializer s;
        s.name = name;
        s.id = ecs::component_id<T>();
        ecs::ComponentRegistry::instance().set_name(s.id, name);
        s.save = [save = std::move(save)](const void* p) { return save(*static_cast<const T*>(p)); };
        s.load = [load = std::move(load)](void* p, const json::Value& v) { return load(*static_cast<T*>(p), v); };
        add(std::move(s));
    }

private:
    SerializerRegistry();
    std::vector<ComponentSerializer> serializers_;
};

// JSON helpers shared by serializers.
[[nodiscard]] json::Value to_json(math::Vec2 v);
[[nodiscard]] json::Value to_json(math::Vec3 v);
[[nodiscard]] json::Value to_json(math::Quat q);
[[nodiscard]] json::Value to_json(math::Color c);
bool from_json(const json::Value& v, math::Vec2& out);
bool from_json(const json::Value& v, math::Vec3& out);
bool from_json(const json::Value& v, math::Quat& out);
bool from_json(const json::Value& v, math::Color& out);

// ---- scene -------------------------------------------------------------------------------------

inline constexpr int kSceneFormatVersion = 1;

class Scene {
public:
    Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] ecs::World& world() noexcept { return world_; }
    [[nodiscard]] const ecs::World& world() const noexcept { return world_; }

    // Creates an entity with Id, Name, Transform and Hierarchy.
    Entity create(std::string_view name = "Entity", Entity parent = kNullEntity, Uuid uuid = {});
    // Destroys the entity and its whole subtree.
    void destroy(Entity e);
    // Reparents; when keep_world is true the local transform is recomputed so the entity does
    // not move. Rejects cycles (parenting under a descendant). Returns false on rejection.
    bool set_parent(Entity child, Entity parent, bool keep_world = false);
    [[nodiscard]] Entity parent(Entity e) const noexcept;
    [[nodiscard]] std::vector<Entity> children(Entity e) const;
    [[nodiscard]] std::vector<Entity> roots() const;
    [[nodiscard]] bool is_descendant(Entity e, Entity ancestor) const noexcept;
    // Deep copy of an entity subtree (new UUIDs), parented next to the original.
    Entity duplicate(Entity e);
    // Moves `e` immediately before sibling `next` (adopting next's parent), or to the end of
    // `parent`'s children when `next` is null. Rejects cycles. Keeps the local transform.
    bool move_before(Entity e, Entity next, Entity parent = kNullEntity);
    [[nodiscard]] Entity next_sibling(Entity e) const noexcept;

    [[nodiscard]] Entity find(Uuid uuid) const noexcept;
    [[nodiscard]] Entity find_by_name(std::string_view name) const;
    [[nodiscard]] Uuid uuid(Entity e) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return by_uuid_.size(); }

    // Recomputes WorldTransform for every entity, parents before children.
    void update_transforms();
    [[nodiscard]] math::Mat4 world_matrix(Entity e) const;

    void clear();

    // Serialization. Entities are written depth-first so parents precede children and
    // sibling order is preserved.
    [[nodiscard]] json::Value save() const;
    [[nodiscard]] Status load(const json::Value& doc);
    [[nodiscard]] Status save_file(const std::filesystem::path& path) const;
    [[nodiscard]] Status load_file(const std::filesystem::path& path);

    // Per-entity snapshots (editor undo/redo, prefabs, network baselines). A snapshot holds
    // uuid, name, parent uuid, the uuid of the next sibling (ordering) and all serializable
    // components; it does not include children.
    [[nodiscard]] json::Value save_entity(Entity e) const;
    // Snapshots of `e` and its descendants, parents first.
    [[nodiscard]] json::Value save_subtree(Entity e) const;
    // Creates the entity if its uuid is unknown, otherwise replaces its name, parent, order and
    // components with the snapshot's. The parent (if any) must already exist.
    [[nodiscard]] Result<Entity> restore_entity(const json::Value& snapshot);
    // Restores a save_subtree() array in order.
    [[nodiscard]] Status restore_subtree(const json::Value& snapshots);

    std::string name{"Untitled"};

private:
    void link_child(Entity parent, Entity child);
    void unlink(Entity child);
    void collect_subtree(Entity e, std::vector<Entity>& out) const;
    void fix_depth(Entity e, std::uint32_t depth);
    [[nodiscard]] Status load_components(Entity e, const json::Value& comps);

    ecs::World world_;
    std::unordered_map<Uuid, Entity, UuidHash> by_uuid_;
    Entity first_root_, last_root_;
    std::vector<std::pair<Entity, math::Mat4>> stack_;
};

} // namespace victus::scene
