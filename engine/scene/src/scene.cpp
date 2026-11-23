// SPDX-License-Identifier: Apache-2.0
#include "victus/scene/scene.hpp"

#include "victus/scene/prefab.hpp"

#include "victus/core/fs.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace victus::scene {

using json::Value;

// ---- json helpers ------------------------------------------------------------------------------

Value to_json(math::Vec2 v) { return Value(json::Array{v.x, v.y}); }
Value to_json(math::Vec3 v) { return Value(json::Array{v.x, v.y, v.z}); }
Value to_json(math::Quat q) { return Value(json::Array{q.x, q.y, q.z, q.w}); }
Value to_json(math::Color c) { return Value(json::Array{c.r, c.g, c.b, c.a}); }

namespace {
bool read_floats(const Value& v, float* out, std::size_t n, std::size_t min_n) {
    if (!v.is_array() || v.size() < min_n || v.size() > n) return false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (!v[i].is_number()) return false;
        out[i] = v[i].as_float();
    }
    return true;
}
Status bad(std::string_view what) { return make_error(ErrorCode::ParseError, std::string(what)); }

template <typename E>
struct EnumName {
    E value;
    const char* name;
};
template <typename E, std::size_t N>
const char* enum_to(E v, const EnumName<E> (&names)[N]) {
    for (const auto& n : names)
        if (n.value == v) return n.name;
    return names[0].name;
}
template <typename E, std::size_t N>
bool enum_from(const Value& v, E& out, const EnumName<E> (&names)[N]) {
    if (!v.is_string()) return false;
    for (const auto& n : names) {
        if (v.as_string() == n.name) {
            out = n.value;
            return true;
        }
    }
    return false;
}
constexpr EnumName<CameraComponent::Projection> kProjections[] = {
    {CameraComponent::Projection::Perspective, "perspective"},
    {CameraComponent::Projection::Orthographic, "orthographic"}};
constexpr EnumName<LightComponent::Type> kLightTypes[] = {{LightComponent::Type::Directional, "directional"},
                                                          {LightComponent::Type::Point, "point"},
                                                          {LightComponent::Type::Spot, "spot"},
                                                          {LightComponent::Type::Area, "area"}};

Value uuid_value(const Uuid& u) { return Value(u.to_string()); }
bool read_uuid(const Value& v, Uuid& out) {
    if (v.is_null()) {
        out = {};
        return true;
    }
    return v.is_string() && Uuid::parse(v.as_string(), out);
}
} // namespace

bool from_json(const Value& v, math::Vec2& out) { return read_floats(v, &out.x, 2, 2); }
bool from_json(const Value& v, math::Vec3& out) { return read_floats(v, &out.x, 3, 3); }
bool from_json(const Value& v, math::Quat& out) {
    if (!read_floats(v, &out.x, 4, 4)) return false;
    out = math::normalize(out);
    return true;
}
bool from_json(const Value& v, math::Color& out) {
    out.a = 1.0f;
    return read_floats(v, &out.r, 4, 3);
}

// ---- serializer registry ------------------------------------------------------------------------

SerializerRegistry& SerializerRegistry::instance() {
    static SerializerRegistry registry;
    return registry;
}

void SerializerRegistry::add(ComponentSerializer serializer) {
    for (auto& existing : serializers_) {
        if (existing.name == serializer.name) {
            existing = std::move(serializer);
            return;
        }
    }
    serializers_.push_back(std::move(serializer));
}

const ComponentSerializer* SerializerRegistry::by_name(std::string_view n) const {
    for (const auto& s : serializers_)
        if (s.name == n) return &s;
    for (const auto& s : serializers_)
        for (const auto& former : s.former_names)
            if (former == n) return &s;
    return nullptr;
}

bool SerializerRegistry::set_version(std::string_view name, std::uint32_t version, std::vector<migration::Step> migrations) {
    if (version < 1 || migrations.size() != version - 1) return false;
    std::sort(migrations.begin(), migrations.end(), [](const auto& a, const auto& b) { return a.from < b.from; });
    for (std::size_t i = 0; i < migrations.size(); ++i)
        if (migrations[i].from != i + 1 || !migrations[i].apply) return false;
    for (auto& s : serializers_) {
        if (s.name != name) continue;
        s.version = version;
        s.migrations = std::move(migrations);
        return true;
    }
    return false;
}

bool SerializerRegistry::add_former_name(std::string_view name, std::string former_name) {
    for (auto& s : serializers_) {
        if (s.name != name) continue;
        if (std::find(s.former_names.begin(), s.former_names.end(), former_name) == s.former_names.end())
            s.former_names.push_back(std::move(former_name));
        return true;
    }
    return false;
}

const ComponentSerializer* SerializerRegistry::by_id(ecs::ComponentId id) const {
    for (const auto& s : serializers_)
        if (s.id == id) return &s;
    return nullptr;
}

SerializerRegistry::SerializerRegistry() {
    ecs::ComponentRegistry::instance().set_name(ecs::component_id<Id>(), "Id");
    ecs::ComponentRegistry::instance().set_name(ecs::component_id<Name>(), "Name");
    ecs::ComponentRegistry::instance().set_name(ecs::component_id<Hierarchy>(), "Hierarchy");
    ecs::ComponentRegistry::instance().set_name(ecs::component_id<WorldTransform>(), "WorldTransform");
    ecs::ComponentRegistry::instance().set_name(ecs::component_id<UnknownComponents>(), "UnknownComponents");

    add<Transform>(
        "Transform",
        [](const Transform& t) {
            Value v = Value::object();
            v.set("position", to_json(t.position));
            v.set("rotation", to_json(t.rotation));
            v.set("scale", to_json(t.scale));
            return v;
        },
        [](Transform& t, const Value& v) -> Status {
            if (const Value* p = v.find("position"); p && !from_json(*p, t.position)) return bad("Transform.position");
            if (const Value* r = v.find("rotation"); r && !from_json(*r, t.rotation)) return bad("Transform.rotation");
            if (const Value* s = v.find("scale"); s && !from_json(*s, t.scale)) return bad("Transform.scale");
            return {};
        });

    add<CameraComponent>(
        "Camera",
        [](const CameraComponent& c) {
            Value v = Value::object();
            v.set("projection", enum_to(c.projection, kProjections));
            v.set("fov_y", c.fov_y_degrees);
            v.set("ortho_height", c.ortho_height);
            v.set("near", c.z_near);
            v.set("far", c.z_far);
            v.set("primary", c.primary);
            if (c.pixel_snap) v.set("pixel_snap", true);
            return v;
        },
        [](CameraComponent& c, const Value& v) -> Status {
            if (const Value* p = v.find("projection"); p && !enum_from(*p, c.projection, kProjections))
                return bad("Camera.projection");
            c.fov_y_degrees = v["fov_y"].as_float(c.fov_y_degrees);
            c.ortho_height = v["ortho_height"].as_float(c.ortho_height);
            c.z_near = v["near"].as_float(c.z_near);
            c.z_far = v["far"].as_float(c.z_far);
            c.primary = v["primary"].as_bool(c.primary);
            c.pixel_snap = v["pixel_snap"].as_bool(c.pixel_snap);
            if (!(c.z_near > 0.0f) || !(c.z_far > c.z_near)) return bad("Camera near/far");
            return {};
        });

    add<LightComponent>(
        "Light",
        [](const LightComponent& l) {
            Value v = Value::object();
            v.set("type", enum_to(l.type, kLightTypes));
            v.set("color", to_json(l.color));
            v.set("intensity", l.intensity);
            v.set("range", l.range);
            v.set("shadows", l.shadows);
            if (l.type == LightComponent::Type::Spot) {
                v.set("inner_angle", l.inner_angle_degrees);
                v.set("outer_angle", l.outer_angle_degrees);
            }
            if (l.type == LightComponent::Type::Point && l.radius != 0.0f) v.set("radius", l.radius);
            if (l.baked) v.set("baked", true);
            if (l.type == LightComponent::Type::Area) {
                v.set("width", l.width);
                v.set("height", l.height);
            }
            if (!l.cookie.is_nil()) v.set("cookie", uuid_value(l.cookie));
            if (l.type == LightComponent::Type::Directional && l.cookie_size != 10.0f) v.set("cookie_size", l.cookie_size);
            return v;
        },
        [](LightComponent& l, const Value& v) -> Status {
            if (const Value* t = v.find("type"); t && !enum_from(*t, l.type, kLightTypes)) return bad("Light.type");
            if (const Value* c = v.find("color"); c && !from_json(*c, l.color)) return bad("Light.color");
            l.intensity = v["intensity"].as_float(l.intensity);
            l.range = v["range"].as_float(l.range);
            l.shadows = v["shadows"].as_bool(l.shadows);
            l.inner_angle_degrees = v["inner_angle"].as_float(l.inner_angle_degrees);
            l.outer_angle_degrees = v["outer_angle"].as_float(l.outer_angle_degrees);
            l.radius = v["radius"].as_float(l.radius);
            l.width = v["width"].as_float(l.width);
            l.height = v["height"].as_float(l.height);
            l.baked = v["baked"].as_bool(l.baked);
            if (!read_uuid(v["cookie"], l.cookie)) return bad("Light.cookie");
            l.cookie_size = v["cookie_size"].as_float(l.cookie_size);
            if (!(l.cookie_size > 0.0f)) return bad("Light.cookie_size");
            if (!(l.radius >= 0.0f) || !(l.width > 0.0f) || !(l.height > 0.0f)) return bad("Light radius/width/height");
            return {};
        });

    add<MeshRendererComponent>(
        "MeshRenderer",
        [](const MeshRendererComponent& m) {
            Value v = Value::object();
            if (!m.mesh_asset.is_nil()) {
                v.set("mesh", uuid_value(m.mesh_asset));
                v.set("mesh_index", m.mesh_index);
            }
            if (!m.primitive.empty()) v.set("primitive", m.primitive);
            if (!m.material_asset.is_nil()) v.set("material", uuid_value(m.material_asset));
            v.set("base_color", to_json(m.base_color));
            v.set("metallic", m.metallic);
            v.set("roughness", m.roughness);
            v.set("cast_shadows", m.cast_shadows);
            v.set("visible", m.visible);
            if (m.lightmapped) v.set("lightmapped", true);
            return v;
        },
        [](MeshRendererComponent& m, const Value& v) -> Status {
            if (!read_uuid(v["mesh"], m.mesh_asset)) return bad("MeshRenderer.mesh");
            m.mesh_index = static_cast<std::uint32_t>(v["mesh_index"].as_int(0));
            m.primitive = v["primitive"].as_string_or("");
            if (!read_uuid(v["material"], m.material_asset)) return bad("MeshRenderer.material");
            if (const Value* c = v.find("base_color"); c && !from_json(*c, m.base_color))
                return bad("MeshRenderer.base_color");
            m.metallic = v["metallic"].as_float(m.metallic);
            m.roughness = v["roughness"].as_float(m.roughness);
            m.cast_shadows = v["cast_shadows"].as_bool(m.cast_shadows);
            m.visible = v["visible"].as_bool(m.visible);
            m.lightmapped = v["lightmapped"].as_bool(false);
            return {};
        });

    add<SpriteComponent>(
        "Sprite",
        [](const SpriteComponent& s) {
            Value v = Value::object();
            if (!s.texture_asset.is_nil()) v.set("texture", uuid_value(s.texture_asset));
            v.set("size", to_json(s.size));
            v.set("tint", to_json(s.tint));
            v.set("pivot", to_json(s.pivot));
            v.set("layer", static_cast<std::int64_t>(s.layer));
            v.set("flip_x", s.flip_x);
            v.set("flip_y", s.flip_y);
            if (s.unlit) v.set("unlit", true);
            if (!(s.uv.x == 0 && s.uv.y == 0 && s.uv.z == 1 && s.uv.w == 1)) {
                Value uv = Value::array();
                for (const float c : {s.uv.x, s.uv.y, s.uv.z, s.uv.w}) uv.push(c);
                v.set("uv", std::move(uv));
            }
            return v;
        },
        [](SpriteComponent& s, const Value& v) -> Status {
            if (!read_uuid(v["texture"], s.texture_asset)) return bad("Sprite.texture");
            if (const Value* p = v.find("size"); p && !from_json(*p, s.size)) return bad("Sprite.size");
            if (const Value* p = v.find("tint"); p && !from_json(*p, s.tint)) return bad("Sprite.tint");
            if (const Value* p = v.find("pivot"); p && !from_json(*p, s.pivot)) return bad("Sprite.pivot");
            s.layer = static_cast<std::int32_t>(v["layer"].as_int(0));
            s.flip_x = v["flip_x"].as_bool(false);
            s.flip_y = v["flip_y"].as_bool(false);
            s.unlit = v["unlit"].as_bool(false);
            s.uv = {0, 0, 1, 1};
            if (const Value* p = v.find("uv"); p && !p->is_null()) {
                if (!p->is_array() || p->size() != 4) return bad("Sprite.uv");
                float c[4];
                for (std::size_t i = 0; i < 4; ++i) {
                    if (!(*p)[i].is_number()) return bad("Sprite.uv");
                    c[i] = (*p)[i].as_float();
                }
                s.uv = {c[0], c[1], c[2], c[3]};
            }
            return {};
        });

    detail::register_prefab_serializers(*this);
}

// ---- scene -----------------------------------------------------------------------------------

Scene::Scene() { (void)SerializerRegistry::instance(); }

Entity Scene::create(std::string_view entity_name, Entity parent_entity, Uuid id) {
    if (id.is_nil()) id = Uuid::generate();
    VICTUS_VERIFY(!by_uuid_.contains(id), "duplicate entity uuid");
    const Entity e = world_.create();
    world_.add<Id>(e, id);
    world_.add<Name>(e, std::string(entity_name));
    world_.add<Transform>(e);
    world_.add<Hierarchy>(e);
    by_uuid_.emplace(id, e);
    link_child(world_.alive(parent_entity) ? parent_entity : kNullEntity, e);
    return e;
}

void Scene::link_child(Entity p, Entity child) {
    ++hierarchy_version_;
    Hierarchy* h = world_.get<Hierarchy>(child);
    VICTUS_ASSERT(h, "scene entity without Hierarchy");
    h->parent = p;
    h->next_sibling = kNullEntity;
    Entity* first = &first_root_;
    Entity* last = &last_root_;
    std::uint32_t depth = 0;
    if (!p.is_null()) {
        Hierarchy* ph = world_.get<Hierarchy>(p);
        first = &ph->first_child;
        last = &ph->last_child;
        depth = ph->depth + 1;
    }
    h->prev_sibling = *last;
    if (!last->is_null()) world_.get<Hierarchy>(*last)->next_sibling = child;
    else *first = child;
    *last = child;
    fix_depth(child, depth);
}

void Scene::unlink(Entity child) {
    ++hierarchy_version_;
    Hierarchy* h = world_.get<Hierarchy>(child);
    if (!h) return;
    Entity* first = &first_root_;
    Entity* last = &last_root_;
    if (!h->parent.is_null()) {
        Hierarchy* ph = world_.get<Hierarchy>(h->parent);
        first = &ph->first_child;
        last = &ph->last_child;
    }
    if (!h->prev_sibling.is_null()) world_.get<Hierarchy>(h->prev_sibling)->next_sibling = h->next_sibling;
    else *first = h->next_sibling;
    if (!h->next_sibling.is_null()) world_.get<Hierarchy>(h->next_sibling)->prev_sibling = h->prev_sibling;
    else *last = h->prev_sibling;
    h->parent = h->prev_sibling = h->next_sibling = kNullEntity;
}

void Scene::fix_depth(Entity e, std::uint32_t depth) {
    Hierarchy* h = world_.get<Hierarchy>(e);
    h->depth = depth;
    for (Entity c = h->first_child; !c.is_null(); c = world_.get<Hierarchy>(c)->next_sibling) fix_depth(c, depth + 1);
}

void Scene::collect_subtree(Entity e, std::vector<Entity>& out) const {
    out.push_back(e);
    const Hierarchy* h = world_.get<Hierarchy>(e);
    if (!h) return;
    for (Entity c = h->first_child; !c.is_null(); c = world_.get<Hierarchy>(c)->next_sibling) collect_subtree(c, out);
}

void Scene::collect_persistent(Entity e, std::vector<Entity>& out) const {
    if (world_.has<TransientComponent>(e)) return;
    out.push_back(e);
    const Hierarchy* h = world_.get<Hierarchy>(e);
    if (!h) return;
    for (Entity c = h->first_child; !c.is_null(); c = world_.get<Hierarchy>(c)->next_sibling) collect_persistent(c, out);
}

void Scene::destroy(Entity e) {
    if (!world_.alive(e)) return;
    std::vector<Entity> subtree;
    collect_subtree(e, subtree);
    unlink(e);
    for (const Entity x : subtree) {
        if (const Id* id = world_.get<Id>(x)) by_uuid_.erase(id->uuid);
        world_.destroy(x);
    }
}

bool Scene::is_descendant(Entity e, Entity ancestor) const noexcept {
    for (Entity p = parent(e); !p.is_null(); p = parent(p))
        if (p == ancestor) return true;
    return false;
}

Entity Scene::find_descendant(Entity root, std::string_view wanted) const {
    if (!world_.alive(root)) return kNullEntity;
    std::vector<Entity> queue = children(root);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Entity e = queue[i];
        const auto* n = world_.get<Name>(e);
        if (n && n->value == wanted) return e;
        for (const Entity c : children(e)) queue.push_back(c);
    }
    return kNullEntity;
}

Entity Scene::attach_to_socket(Entity item, Entity root, std::string_view socket, bool keep_world) {
    const Entity s = find_descendant(root, socket);
    if (s.is_null() || s == item || !set_parent(item, s, keep_world)) return kNullEntity;
    if (!keep_world) {
        Transform* t = world_.get<Transform>(item);
        t->position = {};
        t->rotation = {};
    }
    return s;
}

bool Scene::set_parent(Entity child, Entity new_parent, bool keep_world) {
    if (!world_.alive(child) || child == new_parent) return false;
    if (!new_parent.is_null() && (!world_.alive(new_parent) || is_descendant(new_parent, child))) return false;
    if (keep_world) {
        const math::Mat4 w = world_matrix(child);
        const math::Mat4 p = new_parent.is_null() ? math::Mat4{} : world_matrix(new_parent);
        Transform* t = world_.get<Transform>(child);
        if (t) math::decompose(math::inverse(p) * w, t->position, t->rotation, t->scale);
    }
    unlink(child);
    link_child(new_parent, child);
    return true;
}

Entity Scene::next_sibling(Entity e) const noexcept {
    const Hierarchy* h = world_.get<Hierarchy>(e);
    return h ? h->next_sibling : kNullEntity;
}

bool Scene::move_before(Entity e, Entity next, Entity new_parent) {
    if (!world_.alive(e) || e == next) return false;
    if (!next.is_null()) {
        if (!world_.alive(next)) return false;
        new_parent = parent(next);
    }
    if (!new_parent.is_null() && (new_parent == e || !world_.alive(new_parent) || is_descendant(new_parent, e))) return false;
    unlink(e);
    if (next.is_null()) {
        link_child(new_parent, e);
        return true;
    }
    Hierarchy* h = world_.get<Hierarchy>(e);
    Hierarchy* nh = world_.get<Hierarchy>(next);
    Entity* first = &first_root_;
    std::uint32_t depth = 0;
    if (!new_parent.is_null()) {
        Hierarchy* ph = world_.get<Hierarchy>(new_parent);
        first = &ph->first_child;
        depth = ph->depth + 1;
    }
    h->parent = new_parent;
    h->next_sibling = next;
    h->prev_sibling = nh->prev_sibling;
    if (!nh->prev_sibling.is_null()) world_.get<Hierarchy>(nh->prev_sibling)->next_sibling = e;
    else *first = e;
    nh->prev_sibling = e;
    fix_depth(e, depth);
    return true;
}

Entity Scene::parent(Entity e) const noexcept {
    const Hierarchy* h = world_.get<Hierarchy>(e);
    return h ? h->parent : kNullEntity;
}

std::vector<Entity> Scene::children(Entity e) const {
    std::vector<Entity> out;
    const Hierarchy* h = world_.get<Hierarchy>(e);
    if (!h) return out;
    for (Entity c = h->first_child; !c.is_null(); c = world_.get<Hierarchy>(c)->next_sibling) out.push_back(c);
    return out;
}

std::vector<Entity> Scene::roots() const {
    std::vector<Entity> out;
    for (Entity c = first_root_; !c.is_null(); c = world_.get<Hierarchy>(c)->next_sibling) out.push_back(c);
    return out;
}

Entity Scene::duplicate(Entity e) {
    if (!world_.alive(e)) return kNullEntity;
    std::vector<Entity> subtree;
    collect_subtree(e, subtree);
    std::unordered_map<Entity, Entity, ecs::EntityHash> remap;
    Entity result = kNullEntity;
    for (const Entity src : subtree) {
        const Entity src_parent = parent(src);
        const Entity n = world_.clone(src);
        const Uuid id = Uuid::generate();
        world_.get<Id>(n)->uuid = id;
        *world_.get<Hierarchy>(n) = Hierarchy{};
        by_uuid_.emplace(id, n);
        remap.emplace(src, n);
        if (src == e) {
            link_child(src_parent, n);
            result = n;
        } else {
            link_child(remap.at(src_parent), n);
        }
    }
    return result;
}

Entity Scene::find(Uuid id) const noexcept {
    const auto it = by_uuid_.find(id);
    return it == by_uuid_.end() ? kNullEntity : it->second;
}

Entity Scene::find_by_name(std::string_view n) const {
    Entity found = kNullEntity;
    std::vector<Entity> order;
    for (const Entity r : roots()) collect_subtree(r, order);
    for (const Entity e : order) {
        if (const Name* nm = world_.get<Name>(e); nm && nm->value == n) {
            found = e;
            break;
        }
    }
    return found;
}

Uuid Scene::uuid(Entity e) const noexcept {
    const Id* id = world_.get<Id>(e);
    return id ? id->uuid : Uuid{};
}

math::Mat4 Scene::world_matrix(Entity e) const {
    math::Mat4 m;
    for (Entity x = e; !x.is_null(); x = parent(x)) {
        if (const Transform* t = world_.get<Transform>(x)) m = t->matrix() * m;
    }
    return m;
}

// update_transforms compares Transforms bytewise: they must have no padding.
static_assert(std::is_trivially_copyable_v<Transform> && sizeof(Transform) == 10 * sizeof(float));

void Scene::rebuild_transform_order() {
    // Carry snapshots over by entity so a hierarchy edit elsewhere (a spawn, a reparent) does
    // not force a full recompute. A snapshot is reused only under the same parent.
    const auto parent_bits = [&](const FlatTransform& f) {
        return f.parent >= 0 ? flat_[static_cast<std::size_t>(f.parent)].e.bits() : kNullEntity.bits();
    };
    std::unordered_map<std::uint64_t, std::pair<TransformSnapshot, std::uint64_t>> previous;
    previous.reserve(flat_.size());
    for (std::size_t i = 0; i < flat_.size(); ++i)
        if (snapshots_[i].valid) previous.emplace(flat_[i].e.bits(), std::pair{snapshots_[i], parent_bits(flat_[i])});
    flat_.clear();
    stack_.clear();
    for (Entity r = last_root_; !r.is_null(); r = world_.get<Hierarchy>(r)->prev_sibling) stack_.emplace_back(r, -1);
    while (!stack_.empty()) {
        const auto [e, parent_slot] = stack_.back();
        stack_.pop_back();
        const auto slot = static_cast<std::int32_t>(flat_.size());
        flat_.push_back({e, parent_slot, nullptr, nullptr});
        const Hierarchy* h = world_.get<Hierarchy>(e);
        for (Entity c = h->last_child; !c.is_null(); c = world_.get<Hierarchy>(c)->prev_sibling) stack_.emplace_back(c, slot);
    }
    snapshots_.assign(flat_.size(), TransformSnapshot{});
    for (std::size_t i = 0; i < flat_.size(); ++i)
        if (const auto it = previous.find(flat_[i].e.bits()); it != previous.end() && it->second.second == parent_bits(flat_[i]))
            snapshots_[i] = it->second.first;
    changed_.assign(flat_.size(), 0);
    flat_hierarchy_version_ = hierarchy_version_;
    refresh_transform_pointers();
}

void Scene::refresh_transform_pointers() {
    // Adding WorldTransform moves entities between archetypes: add every missing one first,
    // then take pointers once storage is stable.
    for (std::size_t i = 0; i < flat_.size(); ++i) {
        const Entity e = flat_[i].e;
        if (!world_.has<WorldTransform>(e)) {
            world_.add<WorldTransform>(e);
            snapshots_[i].valid = false;
        }
    }
    for (auto& f : flat_) {
        f.local = world_.get<Transform>(f.e);
        f.world = world_.get<WorldTransform>(f.e);
    }
    flat_world_version_ = world_.structural_version();
}

// Snapshots compare Transforms bytewise: they must have no padding.
static_assert(std::is_trivially_copyable_v<Transform> && sizeof(Transform) == 10 * sizeof(float));

void Scene::update_transforms() {
    // Unity's TransformHierarchy keeps transforms of one hierarchy packed in parent-first order
    // with dirty bits set by its setters. VICTUS exposes Transform as a plain mutable component,
    // so change detection compares each local value with the snapshot that produced the cached
    // world matrix; the packed order and pointers are cached until the structure changes.
    if (flat_hierarchy_version_ != hierarchy_version_) rebuild_transform_order();
    else if (flat_world_version_ != world_.structural_version()) refresh_transform_pointers();
    const std::size_t n = flat_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const FlatTransform& f = flat_[i];
        TransformSnapshot& snap = snapshots_[i];
        const WorldTransform* pw = f.parent >= 0 ? flat_[static_cast<std::size_t>(f.parent)].world : nullptr;
        const bool changed = (f.parent >= 0 && changed_[static_cast<std::size_t>(f.parent)]) || !snap.valid ||
                             snap.has_local != (f.local != nullptr) ||
                             (f.local && std::memcmp(&snap.local, f.local, sizeof(Transform)) != 0); // NOLINT(bugprone-suspicious-memory-comparison): bitwise snapshot of a memcpy'd copy, a false change only recomputes
        changed_[i] = changed ? 1 : 0;
        if (!changed) continue;
        if (f.local) f.world->matrix = pw ? pw->matrix * f.local->matrix() : f.local->matrix();
        else f.world->matrix = pw ? pw->matrix : math::Mat4{};
        snap.valid = true;
        snap.has_local = f.local != nullptr;
        if (f.local) std::memcpy(&snap.local, f.local, sizeof(Transform));
    }
}

void Scene::clear() {
    world_.clear();
    by_uuid_.clear();
    ++hierarchy_version_;
    first_root_ = last_root_ = kNullEntity;
}

// ---- serialization ---------------------------------------------------------------------------

Value Scene::save_entity(Entity e) const {
    Value ev = Value::object();
    if (!world_.alive(e)) return ev;
    ev.set("uuid", uuid_value(uuid(e)));
    if (const Name* nm = world_.get<Name>(e)) ev.set("name", nm->value);
    if (const Entity p = parent(e); !p.is_null()) ev.set("parent", uuid_value(uuid(p)));
    const auto& registry = SerializerRegistry::instance();
    std::vector<std::pair<std::string, Value>> comps;
    for (const ecs::ComponentId id : world_.components_of(e)) {
        if (const ComponentSerializer* s = registry.by_id(id)) {
            Value data = s->save(world_.get_raw(e, id));
            if (s->version > 1 && data.is_object()) data.set(migration::kComponentVersionKey, s->version);
            comps.emplace_back(s->name, std::move(data));
        }
    }
    if (const UnknownComponents* u = world_.get<UnknownComponents>(e)) {
        for (const auto& [k, v] : u->components) comps.emplace_back(k, v);
    }
    std::sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Value cv = Value::object();
    for (auto& [k, v] : comps) cv.set(k, std::move(v));
    ev.set("components", std::move(cv));
    return ev;
}

Value Scene::save_subtree(Entity e) const {
    Value out = Value::array();
    if (!world_.alive(e)) return out;
    std::vector<Entity> order;
    collect_persistent(e, order);
    for (const Entity x : order) {
        Value ev = save_entity(x);
        Entity n = next_sibling(x);
        while (!n.is_null() && world_.has<TransientComponent>(n)) n = next_sibling(n);
        if (!n.is_null()) ev.set("next", uuid_value(uuid(n)));
        out.push(std::move(ev));
    }
    return out;
}

Value Scene::save() const {
    Value doc = Value::object();
    doc.set("format", "victus.scene");
    doc.set("version", kSceneFormatVersion);
    doc.set("name", name);
    Value entities = Value::array();
    std::vector<Entity> order;
    for (const Entity r : roots()) collect_persistent(r, order);
    for (const Entity e : order) entities.push(save_entity(e));
    doc.set("entities", std::move(entities));
    return doc;
}

Status migrate_components(Value& comps, std::vector<std::string>& notes) {
    if (!comps.is_object()) return {};
    const auto& registry = SerializerRegistry::instance();
    auto note = [&notes](std::string text) {
        if (notes.size() < 256) notes.push_back(std::move(text));
    };
    bool needed = false; // common case: everything current, nothing to rebuild
    for (const auto& [key, value] : comps.as_object()) {
        const ComponentSerializer* s = registry.by_name(key);
        if (s && (s->name != key || migration::component_version(value) != s->version)) needed = true;
    }
    if (!needed) return {};
    bool changed = false;
    Value out = Value::object();
    for (const auto& [key, value] : comps.as_object()) {
        const ComponentSerializer* s = registry.by_name(key);
        if (!s) {
            out.set(key, value);
            continue;
        }
        const std::uint32_t version = migration::component_version(value);
        if (version > s->version) {
            note(std::format("{}: version {} is newer than this build reads ({}); kept unchanged", key, version, s->version));
            out.set(key, value);
            continue;
        }
        Value data = value;
        if (s->name != key) {
            note(std::format("{}: renamed to {}", key, s->name));
            changed = true;
        }
        for (std::uint32_t v = version; v < s->version; ++v) {
            const migration::Step& step = s->migrations[v - 1];
            if (auto st = step.apply(data); !st.ok())
                return make_error(ErrorCode::ParseError,
                                  std::format("{} migration from version {} ({}) failed: {}", s->name, v, step.summary, st.error().message));
            if (!data.is_object()) return make_error(ErrorCode::ParseError, std::format("{} migration from version {} broke the component", s->name, v));
            note(std::format("{} {} -> {}: {}", s->name, v, v + 1, step.summary));
            changed = true;
        }
        if (version < s->version) data.set(migration::kComponentVersionKey, s->version);
        // A renamed component that also exists under its new name: the new name wins.
        if (!out.find(s->name) || s->name == key) out.set(s->name, std::move(data));
    }
    if (changed) comps = std::move(out);
    return {};
}

Status Scene::load_components(Entity e, const Value& comps) {
    if (comps.is_null()) return {};
    if (!comps.is_object()) return bad("entity components must be an object");
    const auto& registry = SerializerRegistry::instance();
    Value migrated = comps;
    VICTUS_TRY_STATUS(migrate_components(migrated, migrations_));
    for (const auto& [key, value] : migrated.as_object()) {
        const ComponentSerializer* s = registry.by_name(key);
        if (s && migration::component_version(value) > s->version) s = nullptr; // newer build's data: keep unread
        if (s) {
            Value data = value;
            if (data.is_object() && data.find(migration::kComponentVersionKey)) {
                Value stripped = Value::object();
                for (const auto& [k, v] : data.as_object())
                    if (k != migration::kComponentVersionKey) stripped.set(k, v);
                data = std::move(stripped);
            }
            void* p = world_.add_raw(e, s->id);
            if (auto st = s->load(p, data); !st.ok())
                return make_error(ErrorCode::ParseError, "entity " + uuid(e).to_string() + ": " + st.error().message);
        } else {
            auto* u = world_.get<UnknownComponents>(e);
            if (!u) u = &world_.add<UnknownComponents>(e);
            u->components.emplace_back(key, value);
        }
    }
    return {};
}

static Error bad_entity(std::string_view m) { return Error{ErrorCode::ParseError, std::string(m)}; }

Result<Entity> Scene::restore_entity(const Value& ev) {
    Uuid id;
    if (!ev.is_object() || !read_uuid(ev["uuid"], id) || id.is_nil()) return bad_entity("snapshot: missing or invalid uuid");
    Entity parent_entity = kNullEntity;
    if (const Value* pv = ev.find("parent"); pv && !pv->is_null()) {
        Uuid pid;
        if (!read_uuid(*pv, pid) || (parent_entity = find(pid)).is_null())
            return bad_entity("snapshot " + id.to_string() + ": parent does not exist");
    }
    Entity e = find(id);
    const std::string nm = ev["name"].as_string_or("Entity");
    if (e.is_null()) {
        e = create(nm, parent_entity, id);
    } else {
        if (Name* n = world_.get<Name>(e)) n->value = nm;
        if (parent(e) != parent_entity && !set_parent(e, parent_entity)) return bad_entity("snapshot " + id.to_string() + ": invalid parent");
        // Replace all serializable components.
        const auto& registry = SerializerRegistry::instance();
        for (const ecs::ComponentId cid : world_.components_of(e)) {
            if (registry.by_id(cid)) world_.remove_raw(e, cid);
        }
        world_.remove<UnknownComponents>(e);
    }
    if (const Value* nv = ev.find("next"); nv && !nv->is_null()) {
        Uuid nid;
        if (read_uuid(*nv, nid)) {
            if (const Entity n = find(nid); !n.is_null() && parent(n) == parent_entity) move_before(e, n);
        }
    }
    VICTUS_TRY_STATUS(load_components(e, ev["components"]));
    return e;
}

Status Scene::restore_subtree(const Value& snapshots) {
    if (!snapshots.is_array()) return bad("subtree snapshot must be an array");
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        auto r = restore_entity(snapshots[i]);
        if (!r) return std::move(r).error();
    }
    return {};
}

const migration::Format& scene_format() {
    // Add a Step here when the document layout changes and raise kSceneFormatVersion.
    static const migration::Format format("victus.scene", kSceneFormatVersion);
    return format;
}

Status Scene::load(const Value& source) {
    if (source["format"].as_string_or("") != "victus.scene") return bad("not a victus.scene document");
    Value doc = source;
    auto report = scene_format().upgrade(doc);
    if (!report) return std::move(report).error();
    const Value& entities = doc["entities"];
    if (!entities.is_array()) return bad("scene.entities must be an array");

    clear();
    migrations_ = report->applied;
    name = doc["name"].as_string_or("Untitled");
    std::vector<Entity> created;
    if (auto st = load_entities(entities, created); !st.ok()) {
        clear();
        return st;
    }
    update_transforms();
    return {};
}

Status Scene::load_additive(const Value& source, std::vector<Entity>* created_out) {
    if (source["format"].as_string_or("") != "victus.scene") return bad("not a victus.scene document");
    Value doc = source;
    auto report = scene_format().upgrade(doc);
    if (!report) return std::move(report).error();
    migrations_ = report->applied;
    const Value& entities = doc["entities"];
    if (!entities.is_array()) return bad("scene.entities must be an array");
    std::vector<Entity> created;
    if (auto st = load_entities(entities, created); !st.ok()) {
        // Roll back: destroy what this call created (subtrees may already be gone with a parent).
        std::vector<Uuid> ids;
        ids.reserve(created.size());
        for (const Entity e : created) ids.push_back(uuid(e));
        for (const Uuid& id : ids)
            if (const Entity e = find(id); !e.is_null()) destroy(e);
        return st;
    }
    update_transforms();
    if (created_out) *created_out = std::move(created);
    return {};
}

Status Scene::load_entities(const Value& entities, std::vector<Entity>& created) {
    created.reserve(entities.size());
    // Pass 1: identities (so parents may appear after children in hand-edited files).
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const Value& ev = entities[i];
        Uuid id;
        if (!ev.is_object() || !read_uuid(ev["uuid"], id) || id.is_nil())
            return bad("entity " + std::to_string(i) + ": missing or invalid uuid");
        if (by_uuid_.contains(id)) return bad("duplicate entity uuid " + id.to_string());
        created.push_back(create(ev["name"].as_string_or("Entity"), kNullEntity, id));
    }
    // Pass 2: hierarchy, preserving file order among siblings.
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const Value* pv = entities[i].find("parent");
        if (!pv || pv->is_null()) continue;
        Uuid pid;
        const Entity p = read_uuid(*pv, pid) ? find(pid) : kNullEntity;
        if (p.is_null() || !set_parent(created[i], p)) return bad("entity " + uuid(created[i]).to_string() + ": invalid parent");
    }
    // Pass 3: components.
    for (std::size_t i = 0; i < entities.size(); ++i)
        VICTUS_TRY_STATUS(load_components(created[i], entities[i]["components"]));
    return {};
}

Status Scene::save_file(const std::filesystem::path& path) const {
    if (!migrations_.empty()) VICTUS_TRY_STATUS(fs::keep_premigration_copy(path));
    return fs::write_safely(path, json::stringify(save(), true) + "\n", {.validate = json::document_validator("victus.scene")});
}

Status Scene::load_file(const std::filesystem::path& path) {
    // A torn or damaged scene falls back to its newest valid generation (logged by fs).
    auto text = fs::read_with_recovery(path, json::document_validator("victus.scene"));
    if (!text) return std::move(text).error();
    auto doc = json::parse(text->bytes);
    if (!doc) return make_error(ErrorCode::ParseError, path.string() + ": " + doc.error().message);
    return load(*doc);
}

Result<std::shared_ptr<const ProceduralMesh>> publish_mesh(ProceduralMesh mesh) {
    static std::atomic<std::uint64_t> next_id{1};
    const std::size_t n = mesh.positions.size();
    const auto bad = [](std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; };
    if (n > 0xFFFFFFFFu) return bad("procedural mesh: too many vertices");
    if (!mesh.normals.empty() && mesh.normals.size() != n) return bad("procedural mesh: normal count differs from position count");
    if (!mesh.uvs.empty() && mesh.uvs.size() != n) return bad("procedural mesh: uv count differs from position count");
    if (!mesh.colors.empty() && mesh.colors.size() != n) return bad("procedural mesh: colour count differs from position count");
    if (mesh.indices.size() % 3 != 0) return bad("procedural mesh: index count is not a multiple of 3");
    for (const std::uint32_t i : mesh.indices)
        if (i >= n) return bad(std::format("procedural mesh: index {} out of range ({} vertices)", i, n));
    if (mesh.lods.size() > 7) return bad("procedural mesh: more than 7 levels of detail");
    for (const ProceduralMesh::Lod& lod : mesh.lods) {
        if (lod.indices.empty() || lod.indices.size() % 3 != 0) return bad("procedural mesh: LOD index count is not a positive multiple of 3");
        if (!std::isfinite(lod.error) || lod.error < 0) return bad("procedural mesh: LOD error must be finite and >= 0");
        for (const std::uint32_t i : lod.indices)
            if (i >= n) return bad(std::format("procedural mesh: LOD index {} out of range ({} vertices)", i, n));
    }
    mesh.bounds = {};
    for (const math::Vec3& p : mesh.positions) mesh.bounds.expand(p);
    if (mesh.normals.empty() && n > 0) {
        mesh.normals.assign(n, math::Vec3{0, 0, 0});
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const std::uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            const math::Vec3 face = math::cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
            mesh.normals[a] = mesh.normals[a] + face; // area-weighted
            mesh.normals[b] = mesh.normals[b] + face;
            mesh.normals[c] = mesh.normals[c] + face;
        }
        for (math::Vec3& v : mesh.normals) {
            const float l = math::length(v);
            v = l > 1e-12f ? v * (1.0f / l) : math::Vec3{0, 1, 0};
        }
    }
    mesh.id = next_id.fetch_add(1, std::memory_order_relaxed);
    return std::shared_ptr<const ProceduralMesh>(std::make_shared<ProceduralMesh>(std::move(mesh)));
}

} // namespace victus::scene
