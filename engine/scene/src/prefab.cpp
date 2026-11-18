// SPDX-License-Identifier: Apache-2.0
#include "victus/scene/prefab.hpp"

#include "victus/core/fs.hpp"

#include <array>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace victus::scene {

using json::Value;

namespace {

constexpr std::string_view kInstanceKey = "PrefabInstance";
constexpr std::string_view kPartKey = "PrefabPart";

Error bad(std::string what) { return make_error(ErrorCode::ParseError, std::move(what)); }

bool read_uuid(const Value& v, Uuid& out) { return v.is_string() && Uuid::parse(v.as_string(), out); }

// Replaces every string in `v` that is a key of `map` (uuid strings) with the mapped string.
void remap_strings(Value& v, const std::unordered_map<std::string, std::string>& map) {
    if (map.empty()) return;
    if (v.is_string()) {
        if (const auto it = map.find(v.as_string()); it != map.end()) v = Value(it->second);
    } else if (v.is_array()) {
        for (Value& x : v.as_array()) remap_strings(x, map);
    } else if (v.is_object()) {
        for (auto& [k, x] : v.as_object()) remap_strings(x, map);
    }
}

void erase_key(Value& obj, std::string_view key) {
    if (!obj.is_object()) return;
    auto& o = obj.as_object();
    std::erase_if(o, [&](const auto& kv) { return kv.first == key; });
}

Value instance_json(const PrefabInstance& p) {
    Value v = Value::object();
    v.set("prefab", p.prefab.to_string());
    Value ov = Value::array();
    for (const auto& o : p.overrides) {
        Value e = Value::object();
        e.set("part", o.part.to_string());
        e.set("component", o.component);
        ov.push(std::move(e));
    }
    v.set("overrides", std::move(ov));
    return v;
}

Value part_json(const PrefabPart& p) {
    Value v = Value::object();
    v.set("source", p.source.to_string());
    return v;
}

// Depth-first, parents first, sibling order (the order of Scene::save_subtree).
void collect(const Scene& scene, Entity e, std::vector<Entity>& out) {
    out.push_back(e);
    for (const Entity c : scene.children(e)) collect(scene, c, out);
}

// Builds prefab snapshots for `root`'s subtree, giving `root` the local uuid `root_local` and
// parts of `root`'s own instance their recorded local uuid.
Prefab build(const Scene& scene, Entity root, const Uuid& root_local) {
    Prefab prefab;
    const auto& w = scene.world();
    if (const Name* n = w.get<Name>(root)) prefab.name = n->value;
    std::vector<Entity> order;
    collect(scene, root, order);
    std::unordered_map<std::string, std::string> map;
    for (const Entity x : order) {
        Uuid local = scene.uuid(x);
        if (x == root) {
            local = root_local;
        } else if (const PrefabPart* part = w.get<PrefabPart>(x); part && instance_root(scene, x) == root) {
            local = part->source;
        }
        map.emplace(scene.uuid(x).to_string(), local.to_string());
    }
    Value snapshots = scene.save_subtree(root);
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        Value s = snapshots[i];
        Value comps = s["components"];
        erase_key(comps, kInstanceKey);
        erase_key(comps, kPartKey);
        if (i == 0) {
            erase_key(s, "parent");
            erase_key(s, "next");
            if (const Value* t = comps.find("Transform")) {
                Value tv = *t;
                tv.set("position", Value(json::Array{0.0, 0.0, 0.0}));
                comps.set("Transform", std::move(tv));
            }
        }
        remap_strings(comps, map);
        s.set("components", std::move(comps));
        for (const char* key : {"uuid", "parent", "next"}) {
            if (const Value* v = s.find(key); v && v->is_string()) {
                if (const auto it = map.find(v->as_string()); it != map.end()) s.set(key, it->second);
            }
        }
        prefab.entities.push(std::move(s));
    }
    return prefab;
}

// Parts of the instance rooted at `root` by local uuid (nested instance roots are not entered).
void collect_parts(const Scene& scene, Entity e, std::unordered_map<Uuid, Entity, UuidHash>& parts,
                   std::vector<Entity>& all_parts) {
    for (const Entity c : scene.children(e)) {
        if (scene.world().has<PrefabInstance>(c)) continue;
        if (const PrefabPart* p = scene.world().get<PrefabPart>(c)) {
            parts.emplace(p->source, c);
            all_parts.push_back(c);
        }
        collect_parts(scene, c, parts, all_parts);
    }
}

} // namespace

// ---- serialization -----------------------------------------------------------------------------

namespace detail {
void register_prefab_serializers(SerializerRegistry& registry) {
    registry.add<PrefabInstance>(
        std::string(kInstanceKey), [](const PrefabInstance& p) { return instance_json(p); },
        [](PrefabInstance& p, const Value& v) -> Status {
            if (!v.is_object() || !read_uuid(v["prefab"], p.prefab)) return bad("PrefabInstance.prefab must be a uuid");
            p.overrides.clear();
            if (const Value* ov = v.find("overrides"); ov && !ov->is_null()) {
                if (!ov->is_array()) return bad("PrefabInstance.overrides must be an array");
                for (std::size_t i = 0; i < ov->size(); ++i) {
                    PrefabOverride o;
                    if (!read_uuid((*ov)[i]["part"], o.part) || !(*ov)[i]["component"].is_string())
                        return bad("PrefabInstance.overrides entries need a part uuid and a component name");
                    o.component = (*ov)[i]["component"].as_string();
                    p.overrides.push_back(std::move(o));
                }
            }
            return {};
        });
    registry.add<PrefabPart>(
        std::string(kPartKey), [](const PrefabPart& p) { return part_json(p); },
        [](PrefabPart& p, const Value& v) -> Status {
            if (!v.is_object() || !read_uuid(v["source"], p.source)) return bad("PrefabPart.source must be a uuid");
            return {};
        });
}
} // namespace detail

Uuid Prefab::root() const {
    Uuid u;
    if (entities.size() > 0) read_uuid(entities[0]["uuid"], u);
    return u;
}

Result<Prefab> parse_prefab(const Value& doc) {
    if (!doc.is_object() || doc["format"].as_string_or("") != "victus.prefab") return bad("not a VICTUS prefab (format must be \"victus.prefab\")");
    const auto version = doc["version"].as_int(0);
    if (version < 1 || version > kPrefabFormatVersion)
        return bad(std::format("unsupported prefab version {} (this build reads up to {})", version, kPrefabFormatVersion));
    const Value& entities = doc["entities"];
    if (!entities.is_array() || entities.size() == 0) return bad("prefab has no entities");
    std::unordered_set<Uuid, UuidHash> seen;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const Value& e = entities[i];
        Uuid id;
        if (!e.is_object() || !read_uuid(e["uuid"], id) || id.is_nil()) return bad(std::format("prefab entity {} has no valid uuid", i));
        if (!seen.insert(id).second) return bad(std::format("prefab entity {} repeats uuid {}", i, id.to_string()));
        const Value* pv = e.find("parent");
        const bool has_parent = pv && !pv->is_null();
        if (i == 0 && has_parent) return bad("the first prefab entity (the root) must not have a parent");
        if (i > 0) {
            Uuid pid;
            if (!has_parent || !read_uuid(*pv, pid) || !seen.contains(pid))
                return bad(std::format("prefab entity '{}' must have a parent defined before it (a prefab has one root)",
                                       e["name"].as_string_or(id.to_string())));
        }
        if (const Value* c = e.find("components"); c && !c->is_null() && !c->is_object())
            return bad(std::format("prefab entity '{}': components must be an object", e["name"].as_string_or(id.to_string())));
    }
    Prefab p;
    p.name = doc["name"].as_string_or(entities[0]["name"].as_string_or("Prefab"));
    p.entities = entities;
    return p;
}

Result<Prefab> load_prefab_file(const std::filesystem::path& path) {
    auto text = fs::read_with_recovery(path, json::document_validator("victus.prefab"), fs::kDefaultHistory, 64u << 20);
    if (!text) return std::move(text).error();
    auto doc = json::parse(text->bytes);
    if (!doc) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.string(), doc.error().message));
    auto p = parse_prefab(*doc);
    if (!p) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.string(), p.error().message));
    return p;
}

Value prefab_to_json(const Prefab& prefab) {
    Value doc = Value::object();
    doc.set("format", "victus.prefab");
    doc.set("version", kPrefabFormatVersion);
    doc.set("name", prefab.name);
    doc.set("entities", prefab.entities);
    return doc;
}

Status save_prefab_file(const Prefab& prefab, const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    return fs::write_safely(path, json::stringify(prefab_to_json(prefab)) + "\n",
                            {.validate = json::document_validator("victus.prefab")});
}

// ---- instances ---------------------------------------------------------------------------------

Uuid derive_part_uuid(const Uuid& root, const Uuid& part) noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::memcpy(bytes.data(), &root.hi, 8);
    std::memcpy(bytes.data() + 8, &root.lo, 8);
    std::memcpy(bytes.data() + 16, &part.hi, 8);
    std::memcpy(bytes.data() + 24, &part.lo, 8);
    Uuid u;
    u.hi = fnv1a64(bytes, 0x6a09e667f3bcc909ull);
    u.lo = fnv1a64(bytes, 0xbb67ae8584caa73bull);
    // RFC 9562 version 8 (custom) and variant bits, so derived ids look like ordinary UUIDs.
    u.hi = (u.hi & ~0xF000ull) | 0x8000ull;
    u.lo = (u.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    return u;
}

Prefab make_prefab(const Scene& scene, Entity root) { return build(scene, root, scene.uuid(root)); }

void link_as_instance(Scene& scene, Entity root, const Uuid& prefab_asset) {
    auto& w = scene.world();
    std::vector<Entity> order;
    collect(scene, root, order);
    std::vector<bool> keep(order.size(), false);
    for (std::size_t i = 1; i < order.size(); ++i)
        keep[i] = w.has<PrefabPart>(order[i]) && instance_root(scene, order[i]) == root;
    for (std::size_t i = 1; i < order.size(); ++i) {
        w.remove<PrefabInstance>(order[i]);
        if (!keep[i]) w.add<PrefabPart>(order[i], PrefabPart{scene.uuid(order[i])});
    }
    w.remove<PrefabPart>(root);
    w.add<PrefabInstance>(root, PrefabInstance{prefab_asset, {}});
}

Result<Entity> instantiate(Scene& scene, const Prefab& prefab, const Uuid& prefab_asset, const InstantiateOptions& options) {
    const Uuid local_root = prefab.root();
    if (local_root.is_nil()) return bad("prefab has no root");
    const Uuid root_uuid = options.uuid.is_nil() ? Uuid::generate() : options.uuid;
    std::unordered_map<std::string, std::string> map;
    for (std::size_t i = 0; i < prefab.entities.size(); ++i) {
        Uuid local;
        if (!read_uuid(prefab.entities[i]["uuid"], local)) return bad("prefab entity without uuid");
        const Uuid id = i == 0 ? root_uuid : derive_part_uuid(root_uuid, local);
        if (!scene.find(id).is_null()) return make_error(ErrorCode::InvalidArgument, std::format("an entity with uuid {} already exists", id.to_string()));
        map.emplace(local.to_string(), id.to_string());
    }
    if (!options.parent.is_null() && !scene.world().alive(options.parent)) return make_error(ErrorCode::InvalidArgument, "instantiate: parent is not alive");
    Entity result = kNullEntity;
    for (std::size_t i = 0; i < prefab.entities.size(); ++i) {
        Value s = prefab.entities[i];
        const std::string local = s["uuid"].as_string();
        Value comps = s["components"].is_object() ? s["components"] : Value::object();
        remap_strings(comps, map);
        for (const char* key : {"uuid", "parent", "next"}) {
            if (const Value* v = s.find(key); v && v->is_string()) {
                if (const auto it = map.find(v->as_string()); it != map.end()) s.set(key, it->second);
            }
        }
        if (i == 0) {
            erase_key(s, "next");
            if (options.parent.is_null()) erase_key(s, "parent");
            else s.set("parent", scene.uuid(options.parent).to_string());
            if (!options.name.empty()) s.set("name", options.name);
            if (options.position) {
                Value t = comps["Transform"].is_object() ? comps["Transform"] : Value::object();
                t.set("position", to_json(*options.position));
                comps.set("Transform", std::move(t));
            }
            comps.set(kInstanceKey, instance_json(PrefabInstance{prefab_asset, {}}));
        } else {
            Uuid src;
            (void)Uuid::parse(local, src);
            comps.set(kPartKey, part_json(PrefabPart{src}));
        }
        s.set("components", std::move(comps));
        auto e = scene.restore_entity(s);
        if (!e) {
            if (!result.is_null()) scene.destroy(result);
            return std::move(e).error();
        }
        if (i == 0) result = *e;
    }
    return result;
}

Entity instance_root(const Scene& scene, Entity e) {
    const auto& w = scene.world();
    if (!w.alive(e)) return kNullEntity;
    if (w.has<PrefabInstance>(e)) return e;
    if (!w.has<PrefabPart>(e)) return kNullEntity;
    for (Entity p = scene.parent(e); !p.is_null(); p = scene.parent(p)) {
        if (w.has<PrefabInstance>(p)) return p;
    }
    return kNullEntity;
}

Uuid part_source(const Scene& scene, Entity e, const Prefab& prefab) {
    const auto& w = scene.world();
    if (instance_root(scene, e).is_null()) return {};
    if (w.has<PrefabInstance>(e)) return prefab.root();
    return w.get<PrefabPart>(e)->source;
}

bool mark_override(Scene& scene, Entity e, std::string_view component, const Prefab& prefab) {
    if (component == kInstanceKey || component == kPartKey) return false;
    const Entity root = instance_root(scene, e);
    if (root.is_null()) return false;
    if (e == root && (component == "Transform" || component == "name")) return false; // always the instance's
    const Uuid src = part_source(scene, e, prefab);
    auto* inst = scene.world().get<PrefabInstance>(root);
    const PrefabOverride o{src, std::string(component)};
    for (const auto& x : inst->overrides)
        if (x == o) return false;
    inst->overrides.push_back(o);
    return true;
}

bool is_overridden(const Scene& scene, Entity e, std::string_view component, const Prefab& prefab) {
    const Entity root = instance_root(scene, e);
    if (root.is_null()) return false;
    const Uuid src = part_source(scene, e, prefab);
    for (const auto& x : scene.world().get<PrefabInstance>(root)->overrides)
        if (x.part == src && x.component == component) return true;
    return false;
}

Status update_instance(Scene& scene, Entity root, const Prefab& prefab, bool keep_overrides) {
    auto& w = scene.world();
    const PrefabInstance* current = w.get<PrefabInstance>(root);
    if (!current) return make_error(ErrorCode::InvalidArgument, "update_instance: entity is not a prefab instance root");
    PrefabInstance inst = *current;
    if (!keep_overrides) inst.overrides.clear();
    const Uuid local_root = prefab.root();
    if (local_root.is_nil()) return bad("prefab has no root");

    std::unordered_map<Uuid, Entity, UuidHash> parts;
    std::vector<Entity> all_parts;
    collect_parts(scene, root, parts, all_parts);

    // Instance uuids for every prefab entity.
    const Uuid root_uuid = scene.uuid(root);
    std::unordered_map<std::string, std::string> map;
    std::unordered_set<Uuid, UuidHash> locals;
    for (std::size_t i = 0; i < prefab.entities.size(); ++i) {
        Uuid local;
        if (!read_uuid(prefab.entities[i]["uuid"], local)) return bad("prefab entity without uuid");
        locals.insert(local);
        Uuid id;
        if (i == 0) id = root_uuid;
        else if (const auto it = parts.find(local); it != parts.end()) id = scene.uuid(it->second);
        else {
            id = derive_part_uuid(root_uuid, local);
            if (!scene.find(id).is_null()) id = Uuid::generate();
        }
        map.emplace(local.to_string(), id.to_string());
    }

    // Values the instance keeps.
    struct Kept {
        PrefabOverride key;
        bool present{false};
        Value value;
    };
    std::vector<Kept> kept;
    for (const auto& o : inst.overrides) {
        const Entity e = o.part == local_root ? root : (parts.contains(o.part) ? parts.at(o.part) : kNullEntity);
        if (e.is_null()) continue;
        const Value snap = scene.save_entity(e);
        Kept k{o, false, {}};
        if (o.component == "name") {
            k.present = true;
            k.value = snap["name"];
        } else if (const Value* c = snap["components"].find(o.component)) {
            k.present = true;
            k.value = *c;
        }
        kept.push_back(std::move(k));
    }
    const Value root_snap = scene.save_entity(root);
    const Value root_transform = root_snap["components"]["Transform"];
    const Entity root_next = scene.next_sibling(root);
    const Entity root_parent = scene.parent(root);

    for (std::size_t i = 0; i < prefab.entities.size(); ++i) {
        Value s = prefab.entities[i];
        Uuid local;
        (void)read_uuid(s["uuid"], local);
        Value comps = s["components"].is_object() ? s["components"] : Value::object();
        erase_key(comps, kInstanceKey);
        erase_key(comps, kPartKey);
        remap_strings(comps, map);
        for (const char* key : {"uuid", "parent", "next"}) {
            if (const Value* v = s.find(key); v && v->is_string()) {
                if (const auto it = map.find(v->as_string()); it != map.end()) s.set(key, it->second);
            }
        }
        if (i == 0) {
            if (root_parent.is_null()) erase_key(s, "parent");
            else s.set("parent", scene.uuid(root_parent).to_string());
            if (root_next.is_null()) erase_key(s, "next");
            else s.set("next", scene.uuid(root_next).to_string());
            s.set("name", root_snap["name"]);
            if (!root_transform.is_null()) comps.set("Transform", root_transform);
            comps.set(kInstanceKey, instance_json(inst));
        } else {
            comps.set(kPartKey, part_json(PrefabPart{local}));
        }
        for (const Kept& k : kept) {
            if (k.key.part != local) continue;
            if (k.key.component == "name") s.set("name", k.value);
            else if (k.present) comps.set(k.key.component, k.value);
            else erase_key(comps, k.key.component);
        }
        s.set("components", std::move(comps));
        auto r = scene.restore_entity(s);
        if (!r) return std::move(r).error();
    }
    // Parts the prefab no longer has.
    for (const Entity e : all_parts) {
        if (!w.alive(e)) continue;
        if (const PrefabPart* p = w.get<PrefabPart>(e); p && !locals.contains(p->source)) scene.destroy(e);
    }
    return {};
}

std::vector<Entity> find_instances(const Scene& scene, const Uuid& prefab_asset) {
    std::vector<Entity> order;
    for (const Entity r : scene.roots()) collect(scene, r, order);
    std::vector<Entity> out;
    for (const Entity e : order) {
        if (const PrefabInstance* p = scene.world().get<PrefabInstance>(e); p && (prefab_asset.is_nil() || p->prefab == prefab_asset))
            out.push_back(e);
    }
    return out;
}

Result<std::size_t> update_instances(Scene& scene, const Uuid& prefab_asset, const Prefab& prefab) {
    std::size_t n = 0;
    for (const Entity e : find_instances(scene, prefab_asset)) {
        if (!scene.world().alive(e)) continue; // removed as a part of an outer instance
        VICTUS_TRY_STATUS(update_instance(scene, e, prefab));
        ++n;
    }
    return n;
}

Prefab apply_instance(Scene& scene, Entity root, const Prefab& previous) {
    Prefab p = build(scene, root, previous.root());
    p.name = previous.name;
    if (previous.entities.size() > 0) {
        if (const Value* t = previous.entities[0]["components"].find("Transform")) {
            Value s = p.entities[0];
            Value comps = s["components"];
            comps.set("Transform", *t);
            s.set("components", std::move(comps));
            p.entities.as_array()[0] = std::move(s);
        }
    }
    const Uuid asset = scene.world().get<PrefabInstance>(root) ? scene.world().get<PrefabInstance>(root)->prefab : Uuid{};
    link_as_instance(scene, root, asset);
    return p;
}

} // namespace victus::scene
