// SPDX-License-Identifier: Apache-2.0
// Script bindings: Lua VM helpers: allocation and time budget, JSON <-> Lua conversion, Entity userdata and its core methods, the json library.
#include "script_internal.hpp"

namespace victus::script::detail {



ScriptHost::Impl& host_of(lua_State* L) { return *static_cast<ScriptHost::Impl*>(lua_callbacks(L)->userdata); }


scene::Scene& scene_of(lua_State* L) {
    auto& h = host_of(L);
    if (!h.scene) luaL_error(L, "no scene is active");
    return *h.scene;
}


void* lua_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* h = static_cast<ScriptHost::Impl*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        h->heap -= osize;
        return nullptr;
    }
    if (nsize > osize && h->heap - osize + nsize > h->options.memory_limit_bytes) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) h->heap = h->heap - osize + nsize;
    return p;
}


void interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;
    auto& h = host_of(L);
    if (h.deadline <= 0 || (++h.interrupt_counter & 255u) != 0) return;
    if (now_seconds() > h.deadline) {
        h.deadline = 0;
        lua_rawcheckstack(L, 1);
        luaL_error(L, "script exceeded its time budget of %.0f ms", h.options.call_budget_seconds * 1000.0);
    }
}


// `refs`: objects of the form {"$entity": id} become entities of the active scene (save data).
void push_json(lua_State* L, const Value& v, int depth, bool refs) {
    if (depth > kMaxConvertDepth) luaL_error(L, "value nested too deeply");
    lua_rawcheckstack(L, 3);
    if (refs && v.is_object() && v.size() == 1 && v.as_object().front().first == "$entity") {
        auto* h = static_cast<ScriptHost::Impl*>(lua_callbacks(L)->userdata);
        Uuid id;
        const Value& s = v.as_object().front().second;
        if (h->scene && s.is_string() && Uuid::parse(s.as_string(), id)) push_entity(L, h->scene->find(id));
        else lua_pushnil(L);
        return;
    }
    switch (v.type()) {
    case json::Type::Null: lua_pushnil(L); return;
    case json::Type::Bool: lua_pushboolean(L, v.as_bool()); return;
    case json::Type::Number: lua_pushnumber(L, v.as_number()); return;
    case json::Type::String: lua_pushlstring(L, v.as_string().data(), v.as_string().size()); return;
    case json::Type::Array: {
        const auto& a = v.as_array();
        if (a.size() == 3 && std::all_of(a.begin(), a.end(), [](const Value& x) { return x.is_number(); })) {
            lua_pushvector(L, a[0].as_float(), a[1].as_float(), a[2].as_float());
            return;
        }
        lua_createtable(L, static_cast<int>(a.size()), 0);
        for (std::size_t i = 0; i < a.size(); ++i) {
            push_json(L, a[i], depth + 1, refs);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return;
    }
    case json::Type::Object: {
        const auto& o = v.as_object();
        lua_createtable(L, 0, static_cast<int>(o.size()));
        for (const auto& [k, x] : o) {
            push_json(L, x, depth + 1, refs);
            lua_setfield(L, -2, k.c_str());
        }
        return;
    }
    }
}


// `refs`: entities become {"$entity": id} (save data) instead of null.
Value to_json(lua_State* L, int idx, int depth, bool refs) {
    if (depth > kMaxConvertDepth) luaL_error(L, "value nested too deeply");
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return Value(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER: return Value(lua_tonumber(L, idx));
    case LUA_TSTRING: {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        return Value(std::string(s, n));
    }
    case LUA_TVECTOR: {
        const float* f = lua_tovector(L, idx);
        return Value(json::Array{f[0], f[1], f[2]});
    }
    case LUA_TTABLE: {
        lua_rawcheckstack(L, 3);
        const int n = lua_objlen(L, idx);
        if (n > 0) {
            Value arr = Value::array();
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, idx, i);
                arr.push(to_json(L, -1, depth + 1, refs));
                lua_pop(L, 1);
            }
            return arr;
        }
        Value obj = Value::object();
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) obj.set(lua_tostring(L, -2), to_json(L, -1, depth + 1, refs));
            lua_pop(L, 1);
        }
        return obj;
    }
    case LUA_TUSERDATA: {
        if (!refs) return Value();
        auto* ref = static_cast<const EntityRef*>(lua_touserdatatagged(L, idx, kEntityTag));
        auto* h = static_cast<ScriptHost::Impl*>(lua_callbacks(L)->userdata);
        if (!ref || !h->scene) return Value();
        const Entity e{ref->index, ref->generation};
        if (!h->scene->world().alive(e)) return Value();
        Value o = Value::object();
        o.set("$entity", h->scene->uuid(e).to_string());
        return o;
    }
    default: return Value();
    }
}


// ---- Entity userdata ---------------------------------------------------------------------------

void push_entity(lua_State* L, Entity e) {
    if (e.is_null()) {
        lua_pushnil(L);
        return;
    }
    auto* ref = static_cast<EntityRef*>(lua_newuserdatataggedwithmetatable(L, sizeof(EntityRef), kEntityTag));
    ref->index = e.index;
    ref->generation = e.generation;
}


Entity opt_entity(lua_State* L, int idx) {
    auto* ref = static_cast<EntityRef*>(lua_touserdatatagged(L, idx, kEntityTag));
    return ref ? Entity{ref->index, ref->generation} : Entity{};
}


Entity check_entity(lua_State* L, int idx) {
    auto* ref = static_cast<EntityRef*>(lua_touserdatatagged(L, idx, kEntityTag));
    if (!ref) luaL_typeerror(L, idx, "Entity");
    return Entity{ref->index, ref->generation};
}


// An entity that must still exist in the active scene.
Entity check_live(lua_State* L, int idx, scene::Scene& s) {
    const Entity e = check_entity(L, idx);
    if (!s.world().alive(e)) luaL_error(L, "entity was destroyed");
    return e;
}


math::Vec3 check_vec3(lua_State* L, int idx) {
    const float* f = luaL_checkvector(L, idx);
    return {f[0], f[1], f[2]};
}


void push_vec3(lua_State* L, math::Vec3 v) { lua_pushvector(L, v.x, v.y, v.z); }


math::Vec3 euler_degrees(math::Quat q) {
    const math::Vec3 r = math::to_euler(q);
    return {math::degrees(r.x), math::degrees(r.y), math::degrees(r.z)};
}


math::Quat from_euler_degrees(math::Vec3 d) {
    return math::Quat::euler(math::radians(d.x), math::radians(d.y), math::radians(d.z));
}


const scene::ComponentSerializer* check_serializer(lua_State* L, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const auto* s = scene::SerializerRegistry::instance().by_name(name);
    if (!s) luaL_error(L, "unknown component '%s'", name);
    return s;
}


int entity_get(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* ser = check_serializer(L, 2);
    const void* p = s.world().get_raw(e, ser->id);
    if (!p) {
        lua_pushnil(L);
        return 1;
    }
    push_json(L, ser->save(p));
    return 1;
}


int entity_set(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* ser = check_serializer(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const Value v = to_json(L, 3);
    void* p = s.world().add_raw(e, ser->id); // keeps existing fields not present in v
    if (auto st = ser->load(p, v); !st.ok()) luaL_error(L, "%s: %s", ser->name.c_str(), st.error().message.c_str());
    return 0;
}


int entity_has(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* ser = check_serializer(L, 2);
    lua_pushboolean(L, s.world().get_raw(e, ser->id) != nullptr);
    return 1;
}


int entity_remove(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* ser = check_serializer(L, 2);
    if (ser->name == "Transform") luaL_error(L, "Transform cannot be removed");
    if (s.world().get_raw(e, ser->id)) s.world().remove_raw(e, ser->id);
    return 0;
}


int entity_destroy(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_entity(L, 1);
    if (s.world().alive(e)) s.destroy(e);
    return 0;
}


int entity_find_child(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* name = luaL_checkstring(L, 2);
    for (const Entity c : s.children(e)) {
        const auto* n = s.world().get<scene::Name>(c);
        if (n && n->value == name) {
            push_entity(L, c);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}


int entity_children(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto kids = s.children(e);
    lua_createtable(L, static_cast<int>(kids.size()), 0);
    for (std::size_t i = 0; i < kids.size(); ++i) {
        push_entity(L, kids[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}


int entity_set_parent(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const Entity p = lua_isnoneornil(L, 2) ? Entity{} : check_live(L, 2, s);
    const bool keep_world = lua_toboolean(L, 3) != 0;
    lua_pushboolean(L, s.set_parent(e, p, keep_world));
    return 1;
}


int entity_translate(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* t = s.world().get<scene::Transform>(e);
    t->position = t->position + check_vec3(L, 2);
    return 0;
}


int entity_rotate(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 axis = check_vec3(L, 2);
    const float deg = static_cast<float>(luaL_checknumber(L, 3));
    if (math::length(axis) <= 0) luaL_argerror(L, 2, "axis must not be zero");
    auto* t = s.world().get<scene::Transform>(e);
    t->rotation = math::normalize(t->rotation * math::Quat::axis_angle(axis, math::radians(deg)));
    return 0;
}


int entity_look_at(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* t = s.world().get<scene::Transform>(e);
    math::Vec3 dir = check_vec3(L, 2) - t->position;
    if (math::length(dir) <= 1e-6f) return 0;
    dir = math::normalize(dir);
    const float yaw = std::atan2(-dir.x, -dir.z);
    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    t->rotation = math::Quat::euler(pitch, yaw, 0.0f);
    return 0;
}


namespace {
using PropertyGet = void (*)(lua_State*, scene::Scene&, Entity);
using PropertySet = void (*)(lua_State*, scene::Scene&, Entity, int value);
struct EntityPropertyImpl {
    EntityProperty info;
    PropertyGet get;
    PropertySet set; // nullptr: read-only
};

// The members of Entity besides its methods. Part of the versioned script API (script/api.hpp):
// renaming or removing one, or making it read-only, needs a major API version.
const EntityPropertyImpl kEntityPropertyImpls[] = {
    {{"position", true}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world().get<scene::Transform>(e)->position); },
     [](lua_State* L, scene::Scene& s, Entity e, int v) { s.world().get<scene::Transform>(e)->position = check_vec3(L, v); }},
    {{"rotation", true}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, euler_degrees(s.world().get<scene::Transform>(e)->rotation)); },
     [](lua_State* L, scene::Scene& s, Entity e, int v) { s.world().get<scene::Transform>(e)->rotation = from_euler_degrees(check_vec3(L, v)); }},
    {{"scale", true}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world().get<scene::Transform>(e)->scale); },
     [](lua_State* L, scene::Scene& s, Entity e, int v) { s.world().get<scene::Transform>(e)->scale = check_vec3(L, v); }},
    {{"forward", false}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world().get<scene::Transform>(e)->rotation.rotate({0, 0, -1})); }, nullptr},
    {{"right", false}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world().get<scene::Transform>(e)->rotation.rotate({1, 0, 0})); }, nullptr},
    {{"up", false}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world().get<scene::Transform>(e)->rotation.rotate({0, 1, 0})); }, nullptr},
    {{"world_position", false}, [](lua_State* L, scene::Scene& s, Entity e) { push_vec3(L, s.world_matrix(e).transform_point({0, 0, 0})); }, nullptr},
    {{"name", true},
     [](lua_State* L, scene::Scene& s, Entity e) {
         const auto* n = s.world().get<scene::Name>(e);
         lua_pushstring(L, n ? n->value.c_str() : "");
     },
     [](lua_State* L, scene::Scene& s, Entity e, int v) {
         if (auto* n = s.world().get<scene::Name>(e)) n->value = luaL_checkstring(L, v);
     }},
    {{"parent", false}, [](lua_State* L, scene::Scene& s, Entity e) { push_entity(L, s.parent(e)); }, nullptr},
    {{"id", false}, [](lua_State* L, scene::Scene& s, Entity e) { lua_pushstring(L, s.uuid(e).to_string().c_str()); }, nullptr},
    {{"velocity", true},
     [](lua_State* L, scene::Scene& s, Entity e) {
         const auto* w = physics::world_of(s);
         push_vec3(L, w ? w->linear_velocity(e) : math::Vec3{});
     },
     [](lua_State* L, scene::Scene& s, Entity e, int v) {
         if (auto* w = physics::world_of(s)) w->set_linear_velocity(e, check_vec3(L, v));
     }},
    {{"angular_velocity", true},
     [](lua_State* L, scene::Scene& s, Entity e) {
         const auto* w = physics::world_of(s);
         push_vec3(L, w ? w->angular_velocity(e) : math::Vec3{});
     },
     [](lua_State* L, scene::Scene& s, Entity e, int v) {
         if (auto* w = physics::world_of(s)) w->set_angular_velocity(e, check_vec3(L, v));
     }},
    {{"grounded", false},
     [](lua_State* L, scene::Scene& s, Entity e) {
         const auto* w = physics::world_of(s);
         lua_pushboolean(L, w && w->character_state(e).grounded);
     },
     nullptr},
};

const EntityPropertyImpl* find_property(const char* key) {
    for (const auto& p : kEntityPropertyImpls)
        if (std::strcmp(p.info.name, key) == 0) return &p;
    return nullptr;
}
} // namespace

std::vector<EntityProperty> entity_properties() {
    std::vector<EntityProperty> out{{"valid", false}};
    for (const auto& p : kEntityPropertyImpls) out.push_back(p.info);
    return out;
}

int entity_index(lua_State* L) {
    const Entity e = check_entity(L, 1);
    const char* key = luaL_checkstring(L, 2);
    auto& s = scene_of(L);
    const bool alive = s.world().alive(e);
    if (std::strcmp(key, "valid") == 0) { // the one member that works on destroyed entities
        lua_pushboolean(L, alive);
        return 1;
    }
    // Methods (upvalue 1 is the method table).
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    if (!alive) luaL_error(L, "entity was destroyed (reading '%s')", key);
    const auto* p = find_property(key);
    if (!p) luaL_error(L, "Entity has no member '%s'", key);
    p->get(L, s, e);
    return 1;
}


int entity_newindex(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* key = luaL_checkstring(L, 2);
    const auto* p = find_property(key);
    if (!p || !p->set) luaL_error(L, "Entity member '%s' is read-only or unknown", key);
    p->set(L, s, e, 3);
    return 0;
}


int entity_eq(lua_State* L) {
    lua_pushboolean(L, opt_entity(L, 1) == opt_entity(L, 2));
    return 1;
}


int entity_tostring(lua_State* L) {
    const Entity e = check_entity(L, 1);
    auto& h = host_of(L);
    std::string name = "?";
    if (h.scene && h.scene->world().alive(e)) {
        if (const auto* n = h.scene->world().get<scene::Name>(e)) name = n->value;
    } else {
        name = "destroyed";
    }
    lua_pushstring(L, std::format("Entity({})", name).c_str());
    return 1;
}


namespace {
// VICTUS_SCRIPT_SLOW_CALLS=<ms>: every native function exposed to scripts logs a warning when
// one call takes longer than <ms>, naming it (diagnoses "exceeded its time budget" errors).
double slow_call_threshold() {
    static const double t = [] {
        const char* v = std::getenv("VICTUS_SCRIPT_SLOW_CALLS");
        return v && *v ? std::atof(v) / 1000.0 : 0.0;
    }();
    return t;
}

int timed_native(lua_State* L) {
    const auto fn = reinterpret_cast<lua_CFunction>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    const char* name = static_cast<const char*>(lua_tolightuserdata(L, lua_upvalueindex(2)));
    const double start = now_seconds();
    const int n = fn(L);
    const double spent = now_seconds() - start;
    if (spent > slow_call_threshold()) VICTUS_LOG_WARN("script", "slow native call {}: {:.1f} ms", name, spent * 1000.0);
    return n;
}
} // namespace

void push_native(lua_State* L, lua_CFunction fn, const char* name) {
    if (slow_call_threshold() <= 0) {
        lua_pushcfunction(L, fn, name);
        return;
    }
    lua_pushlightuserdata(L, reinterpret_cast<void*>(fn));
    lua_pushlightuserdata(L, const_cast<char*>(name));
    lua_pushcclosure(L, timed_native, name, 2);
}

void register_lib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        push_native(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, name);
}


// json.encode(value, pretty = false) -> string; json.decode(string) -> value
int json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    const std::string text = json::stringify(to_json(L, 1), lua_toboolean(L, 2) != 0);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int json_decode(lua_State* L) {
    std::size_t n = 0;
    const char* text = luaL_checklstring(L, 1, &n);
    auto doc = json::parse(std::string_view(text, n));
    if (!doc) luaL_error(L, "json.decode: %s", doc.error().message.c_str());
    push_json(L, *doc);
    return 1;
}


bool editable(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNUMBER || t == LUA_TBOOLEAN || t == LUA_TSTRING || t == LUA_TVECTOR;
}

} // namespace victus::script::detail
