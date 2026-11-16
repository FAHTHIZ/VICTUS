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
};
struct LightComponent {
    enum class Type : std::uint8_t { Directional, Point };
    Type type{Type::Directional};
    math::Color color{1, 1, 1, 1}; // linear
    float intensity{3.0f};
    float range{10.0f};
    bool shadows{true};
};
// References a mesh inside an asset (glTF/model) by asset UUID and sub-mesh index, plus a
// material override index (-1 = mesh's own material).
struct MeshRendererComponent {
    Uuid mesh_asset;
    std::uint32_t mesh_index{0};
    std::string primitive; // built-in primitive when mesh_asset is nil: cube|sphere|plane|cylinder
    math::Color base_color{1, 1, 1, 1}; // linear multiplier
    float metallic{0.0f};
    float roughness{0.5f};
    bool cast_shadows{true};
    bool visible{true};
};
struct SpriteComponent {
    Uuid texture_asset;
    math::Vec2 size{1, 1};
    math::Color tint{1, 1, 1, 1};
    math::Vec2 pivot{0.5f, 0.5f};
    std::int32_t layer{0};
    bool flip_x{false};
    bool flip_y{false};
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

    std::string name{"Untitled"};

private:
    void link_child(Entity parent, Entity child);
    void unlink(Entity child);
    void collect_subtree(Entity e, std::vector<Entity>& out) const;
    void fix_depth(Entity e, std::uint32_t depth);

    ecs::World world_;
    std::unordered_map<Uuid, Entity, UuidHash> by_uuid_;
    Entity first_root_, last_root_;
    std::vector<std::pair<Entity, math::Mat4>> stack_;
};

} // namespace victus::scene
