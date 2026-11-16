// SPDX-License-Identifier: Apache-2.0
#include "victus/script/script.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <unordered_map>

namespace victus::script {

using json::Value;
using scene::Entity;
namespace sfs = std::filesystem;

namespace {

constexpr int kEntityTag = 1;
constexpr int kMaxConvertDepth = 32;
constexpr std::size_t kMaxRecentErrors = 32;
constexpr std::uint64_t kMaxScriptBytes = 4ull << 20;

struct EntityRef {
    std::uint32_t index;
    std::uint32_t generation;
};

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct ScriptHost::Impl {
    struct Module {
        std::string path;
        int ref{LUA_NOREF};
        sfs::file_time_type mtime{};
        std::uint64_t version{0};
        bool ok{false};
    };
    struct Instance {
        int ref{LUA_NOREF};
        std::string path;
        bool started{false};
        bool faulted{false};
    };

    HostOptions options;
    lua_State* L{nullptr};
    std::size_t heap{0};
    double deadline{0};
    std::uint32_t interrupt_counter{0};
    std::unordered_map<std::string, Module> modules;
    std::unordered_map<std::uint64_t, Instance> instances;
    std::vector<std::string> recent_errors;
    HostStats counters;
    double next_watch{0};

    // Context of the current call.
    scene::Scene* scene{nullptr};
    gameplay::FrameTime time{};
    float fixed_dt{1.0f / 60.0f};
    const gameplay::InputSource* input{nullptr};

    ~Impl() {
        if (L) lua_close(L);
    }

    void report(std::string message) {
        ++counters.errors;
        VICTUS_LOG_ERROR("script", "{}", message);
        if (recent_errors.size() >= kMaxRecentErrors) recent_errors.erase(recent_errors.begin());
        recent_errors.push_back(std::move(message));
    }

    // Protected call with the time budget armed. On error the message is reported and popped.
    bool pcall(int nargs, int nresults, std::string_view what) {
        deadline = options.call_budget_seconds > 0 ? now_seconds() + options.call_budget_seconds : 0;
        interrupt_counter = 0;
        const int st = lua_pcall(L, nargs, nresults, 0);
        deadline = 0;
        ++counters.calls;
        if (st == LUA_OK) return true;
        const char* msg = lua_tostring(L, -1);
        report(std::format("{}: {}", what, msg ? msg : "unknown error"));
        lua_pop(L, 1);
        return false;
    }

    std::string chunk_name(std::string_view path) const { return "@" + std::string(path); }

    // Compiles `source` and leaves the loaded function on L's stack inside a fresh sandboxed
    // thread (also left on the stack below the function). Returns false with an error message.
    bool load_chunk(std::string_view source, const std::string& name, std::string& error) {
        lua_CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 1;
        std::size_t size = 0;
        char* bytecode = luau_compile(source.data(), source.size(), &opts, &size);
        if (!bytecode) {
            error = "compiler out of memory";
            return false;
        }
        lua_State* T = lua_newthread(L);
        luaL_sandboxthread(T);
        const int st = luau_load(T, name.c_str(), bytecode, size, 0);
        std::free(bytecode);
        if (st != 0) {
            const char* msg = lua_tostring(T, -1);
            error = msg ? msg : "load failed";
            lua_pop(L, 1); // thread
            return false;
        }
        lua_xmove(T, L, 1); // function now on top of L, thread below it
        return true;
    }

    Module* module(std::string_view path) {
        auto it = modules.find(std::string(path));
        if (it != modules.end()) return it->second.ok ? &it->second : nullptr;
        Module m;
        m.path = std::string(path);
        load_module(m);
        auto [pos, _] = modules.emplace(m.path, std::move(m));
        counters.modules = modules.size();
        return pos->second.ok ? &pos->second : nullptr;
    }

    // (Re)loads a module file. On success replaces m.ref; on failure keeps the previous ref.
    bool load_module(Module& m) {
        auto file = fs::resolve_under(options.assets_root, m.path);
        if (!file) {
            report(std::format("{}: {}", m.path, file.error().message));
            return false;
        }
        std::error_code ec;
        m.mtime = sfs::last_write_time(*file, ec);
        auto text = fs::read_text(*file, kMaxScriptBytes);
        if (!text) {
            report(std::format("{}: {}", m.path, text.error().message));
            return false;
        }
        std::string error;
        const int top = lua_gettop(L);
        if (!load_chunk(*text, chunk_name(m.path), error)) {
            report(error);
            lua_settop(L, top);
            return false;
        }
        if (!pcall(0, 1, m.path)) {
            lua_settop(L, top);
            return false;
        }
        if (!lua_istable(L, -1)) {
            report(std::format("{}: a script must return a table (got {})", m.path, luaL_typename(L, -1)));
            lua_settop(L, top);
            return false;
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index"); // instances use the module as metatable
        if (m.ref != LUA_NOREF) lua_unref(L, m.ref);
        m.ref = lua_ref(L, -1);
        m.ok = true;
        ++m.version;
        lua_settop(L, top);
        return true;
    }

    void drop(Instance& in) {
        if (in.ref != LUA_NOREF) lua_unref(L, in.ref);
        in.ref = LUA_NOREF;
    }

    // Calls instance method `fn` if the module defines it.
    bool call(Instance& in, const char* fn, const float* dt) {
        lua_getref(L, in.ref);
        lua_getfield(L, -1, fn);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            return true;
        }
        lua_pushvalue(L, -2);
        int nargs = 1;
        if (dt) {
            lua_pushnumber(L, *dt);
            ++nargs;
        }
        const bool ok = pcall(nargs, 0, std::format("{}:{}", in.path, fn));
        lua_pop(L, 1); // instance
        if (!ok) {
            in.faulted = true;
            VICTUS_LOG_WARN("script", "{} disabled until it is reloaded", in.path);
        }
        return ok;
    }
};

namespace {

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

// ---- JSON <-> Lua ----------------------------------------------------------------------------

void push_json(lua_State* L, const Value& v, int depth = 0) {
    if (depth > kMaxConvertDepth) luaL_error(L, "value nested too deeply");
    lua_rawcheckstack(L, 3);
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
            push_json(L, a[i], depth + 1);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return;
    }
    case json::Type::Object: {
        const auto& o = v.as_object();
        lua_createtable(L, 0, static_cast<int>(o.size()));
        for (const auto& [k, x] : o) {
            push_json(L, x, depth + 1);
            lua_setfield(L, -2, k.c_str());
        }
        return;
    }
    }
}

Value to_json(lua_State* L, int idx, int depth = 0) {
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
                arr.push(to_json(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        }
        Value obj = Value::object();
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) obj.set(lua_tostring(L, -2), to_json(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return obj;
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

int entity_index(lua_State* L) {
    const Entity e = check_entity(L, 1);
    const char* key = luaL_checkstring(L, 2);
    auto& s = scene_of(L);
    const bool alive = s.world().alive(e);
    if (std::strcmp(key, "valid") == 0) {
        lua_pushboolean(L, alive);
        return 1;
    }
    // Methods (upvalue 1 is the method table).
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    if (!alive) luaL_error(L, "entity was destroyed (reading '%s')", key);
    const auto* t = s.world().get<scene::Transform>(e);
    if (std::strcmp(key, "position") == 0) push_vec3(L, t->position);
    else if (std::strcmp(key, "rotation") == 0) push_vec3(L, euler_degrees(t->rotation));
    else if (std::strcmp(key, "scale") == 0) push_vec3(L, t->scale);
    else if (std::strcmp(key, "forward") == 0) push_vec3(L, t->rotation.rotate({0, 0, -1}));
    else if (std::strcmp(key, "right") == 0) push_vec3(L, t->rotation.rotate({1, 0, 0}));
    else if (std::strcmp(key, "up") == 0) push_vec3(L, t->rotation.rotate({0, 1, 0}));
    else if (std::strcmp(key, "world_position") == 0) push_vec3(L, s.world_matrix(e).transform_point({0, 0, 0}));
    else if (std::strcmp(key, "name") == 0) {
        const auto* n = s.world().get<scene::Name>(e);
        lua_pushstring(L, n ? n->value.c_str() : "");
    } else if (std::strcmp(key, "parent") == 0) push_entity(L, s.parent(e));
    else if (std::strcmp(key, "id") == 0) lua_pushstring(L, s.uuid(e).to_string().c_str());
    else luaL_error(L, "Entity has no member '%s'", key);
    return 1;
}

int entity_newindex(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* key = luaL_checkstring(L, 2);
    auto* t = s.world().get<scene::Transform>(e);
    if (std::strcmp(key, "position") == 0) t->position = check_vec3(L, 3);
    else if (std::strcmp(key, "rotation") == 0) t->rotation = from_euler_degrees(check_vec3(L, 3));
    else if (std::strcmp(key, "scale") == 0) t->scale = check_vec3(L, 3);
    else if (std::strcmp(key, "name") == 0) {
        auto* n = s.world().get<scene::Name>(e);
        if (n) n->value = luaL_checkstring(L, 3);
    } else luaL_error(L, "Entity member '%s' is read-only or unknown", key);
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

// ---- global libraries --------------------------------------------------------------------------

int scene_find(lua_State* L) {
    auto& s = scene_of(L);
    push_entity(L, s.find_by_name(luaL_checkstring(L, 1)));
    return 1;
}

int scene_spawn(lua_State* L) {
    auto& s = scene_of(L);
    const char* name = luaL_optstring(L, 1, "Entity");
    const Entity parent = lua_isnoneornil(L, 2) ? Entity{} : check_live(L, 2, s);
    push_entity(L, s.create(name, parent));
    return 1;
}

int scene_destroy(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_entity(L, 1);
    if (s.world().alive(e)) s.destroy(e);
    return 0;
}

int scene_query(lua_State* L) {
    auto& s = scene_of(L);
    const auto* ser = check_serializer(L, 1);
    std::vector<Entity> found;
    s.world().each<scene::Transform>([&](Entity e, scene::Transform&) {
        if (s.world().get_raw(e, ser->id)) found.push_back(e);
    });
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        push_entity(L, found[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int scene_count(lua_State* L) {
    lua_pushinteger(L, static_cast<int>(scene_of(L).size()));
    return 1;
}

int input_down(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushboolean(L, in && in->key_down(luaL_checkstring(L, 1)));
    return 1;
}
int input_pressed(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushboolean(L, in && in->key_pressed(luaL_checkstring(L, 1)));
    return 1;
}
int input_released(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushboolean(L, in && in->key_released(luaL_checkstring(L, 1)));
    return 1;
}
int input_mouse_down(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushboolean(L, in && in->mouse_down(luaL_optinteger(L, 1, 0)));
    return 1;
}
int input_mouse_pressed(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushboolean(L, in && in->mouse_pressed(luaL_optinteger(L, 1, 0)));
    return 1;
}
int input_mouse_position(lua_State* L) {
    const auto* in = host_of(L).input;
    const math::Vec2 p = in ? in->mouse_position() : math::Vec2{};
    lua_pushvector(L, p.x, p.y, 0);
    return 1;
}
int input_mouse_delta(lua_State* L) {
    const auto* in = host_of(L).input;
    const math::Vec2 p = in ? in->mouse_delta() : math::Vec2{};
    lua_pushvector(L, p.x, p.y, 0);
    return 1;
}
// input.axis("A", "D") -> -1, 0 or 1; extra pairs are alternatives: axis("A","D","Left","Right")
int input_axis(lua_State* L) {
    const auto* in = host_of(L).input;
    const int n = lua_gettop(L);
    float v = 0;
    for (int i = 1; i + 1 <= n; i += 2) {
        const char* neg = luaL_checkstring(L, i);
        const char* pos = luaL_checkstring(L, i + 1);
        if (in && in->key_down(neg)) v = -1;
        if (in && in->key_down(pos)) v = v < 0 ? 0 : 1;
        if (v != 0) break;
    }
    lua_pushnumber(L, v);
    return 1;
}

int time_delta(lua_State* L) {
    lua_pushnumber(L, host_of(L).time.dt);
    return 1;
}
int time_now(lua_State* L) {
    lua_pushnumber(L, host_of(L).time.time);
    return 1;
}
int time_frame(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(host_of(L).time.frame));
    return 1;
}
int time_fixed_delta(lua_State* L) {
    lua_pushnumber(L, host_of(L).fixed_dt);
    return 1;
}

std::string concat_args(lua_State* L) {
    std::string out;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        std::size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) out += ' ';
        out.append(s, len);
        lua_pop(L, 1);
    }
    return out;
}

int log_info(lua_State* L) {
    VICTUS_LOG_INFO("script", "{}", concat_args(L));
    return 0;
}
int log_warn(lua_State* L) {
    VICTUS_LOG_WARN("script", "{}", concat_args(L));
    return 0;
}
int log_error(lua_State* L) {
    VICTUS_LOG_ERROR("script", "{}", concat_args(L));
    return 0;
}

void register_lib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, name);
}

void open_victus_libs(lua_State* L) {
    // Entity metatable.
    lua_newtable(L); // metatable
    lua_newtable(L); // methods
    const luaL_Reg methods[] = {{"get", entity_get},
                                {"set", entity_set},
                                {"has", entity_has},
                                {"remove", entity_remove},
                                {"destroy", entity_destroy},
                                {"find_child", entity_find_child},
                                {"children", entity_children},
                                {"set_parent", entity_set_parent},
                                {"translate", entity_translate},
                                {"rotate", entity_rotate},
                                {"look_at", entity_look_at},
                                {nullptr, nullptr}};
    for (const luaL_Reg* f = methods; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_pushcclosure(L, entity_index, "Entity.__index", 1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, entity_newindex, "Entity.__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, entity_eq, "Entity.__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, entity_tostring, "Entity.__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, "Entity");
    lua_setfield(L, -2, "__type");
    lua_pushstring(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_setuserdatametatable(L, kEntityTag);

    const luaL_Reg scene_fns[] = {{"find", scene_find},     {"spawn", scene_spawn}, {"destroy", scene_destroy},
                                  {"query", scene_query},   {"count", scene_count}, {nullptr, nullptr}};
    register_lib(L, "scene", scene_fns);
    const luaL_Reg input_fns[] = {{"down", input_down},
                                  {"pressed", input_pressed},
                                  {"released", input_released},
                                  {"mouse_down", input_mouse_down},
                                  {"mouse_pressed", input_mouse_pressed},
                                  {"mouse_position", input_mouse_position},
                                  {"mouse_delta", input_mouse_delta},
                                  {"axis", input_axis},
                                  {nullptr, nullptr}};
    register_lib(L, "input", input_fns);
    const luaL_Reg time_fns[] = {{"delta", time_delta},
                                 {"now", time_now},
                                 {"frame", time_frame},
                                 {"fixed_delta", time_fixed_delta},
                                 {nullptr, nullptr}};
    register_lib(L, "time", time_fns);
    const luaL_Reg log_fns[] = {{"info", log_info}, {"warn", log_warn}, {"error", log_error}, {nullptr, nullptr}};
    register_lib(L, "log", log_fns);
    lua_pushcfunction(L, log_info, "print");
    lua_setglobal(L, "print");
}

bool editable(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNUMBER || t == LUA_TBOOLEAN || t == LUA_TSTRING || t == LUA_TVECTOR;
}

} // namespace

// ---- ScriptComponent registration ---------------------------------------------------------------

void register_components() {
    auto& r = scene::SerializerRegistry::instance();
    if (r.by_name("Script")) return;
    r.add<ScriptComponent>(
        "Script",
        [](const ScriptComponent& c) {
            Value v = Value::object();
            v.set("script", c.script);
            v.set("enabled", c.enabled);
            v.set("properties", c.properties.is_object() ? c.properties : Value::object());
            return v;
        },
        [](ScriptComponent& c, const Value& v) -> Status {
            c.script = v["script"].as_string_or(c.script);
            c.enabled = v["enabled"].as_bool(c.enabled);
            if (const Value* p = v.find("properties"); p && p->is_object()) c.properties = *p;
            return {};
        });
}

// ---- ScriptHost ----------------------------------------------------------------------------------

ScriptHost::ScriptHost(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ScriptHost::~ScriptHost() = default;

Result<std::unique_ptr<ScriptHost>> ScriptHost::create(HostOptions options) {
    register_components();
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    impl->L = lua_newstate(&lua_alloc, impl.get());
    if (!impl->L) return make_error(ErrorCode::OutOfMemory, "cannot create Luau VM");
    lua_State* L = impl->L;
    lua_callbacks(L)->userdata = impl.get();
    lua_callbacks(L)->interrupt = &interrupt;
    luaL_openlibs(L);
    open_victus_libs(L);
    luaL_sandbox(L);
    impl->next_watch = now_seconds() + impl->options.watch_interval_seconds;
    return std::unique_ptr<ScriptHost>(new ScriptHost(std::move(impl)));
}

void ScriptHost::update(scene::Scene& scene, const gameplay::FrameTime& time, const gameplay::InputSource* input) {
    VICTUS_PROFILE_SCOPE("script.update");
    Impl& h = *impl_;
    h.scene = &scene;
    h.time = time;
    h.input = input;
    if (h.options.watch_files && now_seconds() >= h.next_watch) {
        reload_changed();
        h.next_watch = now_seconds() + h.options.watch_interval_seconds;
    }

    struct Item {
        Entity e;
        std::string path;
        bool enabled;
    };
    std::vector<Item> items;
    scene.world().each<ScriptComponent>(
        [&](Entity e, ScriptComponent& c) { items.push_back({e, c.script, c.enabled}); });

    // Drop instances whose entity, component or script changed.
    for (auto it = h.instances.begin(); it != h.instances.end();) {
        const auto* sc = [&]() -> const ScriptComponent* {
            const Entity e{static_cast<std::uint32_t>(it->first), static_cast<std::uint32_t>(it->first >> 32)};
            return scene.world().alive(e) ? scene.world().get<ScriptComponent>(e) : nullptr;
        }();
        if (!sc || sc->script != it->second.path) {
            h.drop(it->second);
            it = h.instances.erase(it);
        } else {
            ++it;
        }
    }

    const float dt = time.dt;
    for (const Item& item : items) {
        if (!item.enabled || item.path.empty()) continue;
        if (!scene.world().alive(item.e)) continue; // destroyed by an earlier script this frame
        auto it = h.instances.find(item.e.bits());
        if (it == h.instances.end()) {
            Impl::Module* m = h.module(item.path);
            if (!m) continue;
            const auto* sc = scene.world().get<ScriptComponent>(item.e);
            if (!sc) continue;
            lua_State* L = h.L;
            lua_createtable(L, 0, 4);
            if (sc->properties.is_object()) {
                for (const auto& [k, v] : sc->properties.as_object()) {
                    if (k == "entity") continue;
                    push_json(L, v);
                    lua_setfield(L, -2, k.c_str());
                }
            }
            push_entity(L, item.e);
            lua_setfield(L, -2, "entity");
            lua_getref(L, m->ref);
            lua_setmetatable(L, -2);
            Impl::Instance in;
            in.path = item.path;
            in.ref = lua_ref(L, -1);
            lua_pop(L, 1);
            it = h.instances.emplace(item.e.bits(), std::move(in)).first;
        }
        Impl::Instance& in = it->second;
        if (in.faulted) continue;
        if (!in.started) {
            in.started = true;
            if (!h.call(in, "start", nullptr)) continue;
            if (!scene.world().alive(item.e)) continue;
        }
        (void)h.call(in, "update", &dt);
    }
    h.counters.instances = h.instances.size();
}

void ScriptHost::fixed_update(scene::Scene& scene, const gameplay::FrameTime& time,
                              const gameplay::InputSource* input) {
    VICTUS_PROFILE_SCOPE("script.fixed_update");
    Impl& h = *impl_;
    h.scene = &scene;
    h.time = time;
    h.fixed_dt = time.dt;
    h.input = input;
    std::vector<std::uint64_t> keys;
    keys.reserve(h.instances.size());
    for (const auto& [k, in] : h.instances)
        if (in.started && !in.faulted) keys.push_back(k);
    const float dt = time.dt;
    for (const std::uint64_t k : keys) {
        const Entity e{static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32)};
        if (!scene.world().alive(e)) continue;
        const auto* sc = scene.world().get<ScriptComponent>(e);
        if (!sc || !sc->enabled) continue;
        auto it = h.instances.find(k);
        if (it == h.instances.end() || it->second.faulted) continue;
        (void)h.call(it->second, "fixed_update", &dt);
    }
}

std::size_t ScriptHost::reload_changed() {
    Impl& h = *impl_;
    std::size_t reloaded = 0;
    for (auto& [path, m] : h.modules) {
        auto file = fs::resolve_under(h.options.assets_root, path);
        if (!file) continue;
        std::error_code ec;
        const auto t = sfs::last_write_time(*file, ec);
        if (ec || t == m.mtime) continue;
        if (!h.load_module(m)) continue; // keeps the previous version running
        ++reloaded;
        ++h.counters.reloads;
        VICTUS_LOG_INFO("script", "reloaded {}", path);
        for (auto& [key, in] : h.instances) {
            if (in.path != path) continue;
            lua_getref(h.L, in.ref);
            lua_getref(h.L, m.ref);
            lua_setmetatable(h.L, -2);
            lua_pop(h.L, 1);
            in.faulted = false;
            if (in.started && h.scene) (void)h.call(in, "reload", nullptr);
        }
    }
    return reloaded;
}

Result<std::vector<PropertyInfo>> ScriptHost::describe(std::string_view script_path) {
    Impl& h = *impl_;
    Impl::Module* m = h.module(script_path);
    if (!m) {
        // Report the most recent error for this file if there is one.
        for (auto it = h.recent_errors.rbegin(); it != h.recent_errors.rend(); ++it)
            if (it->find(script_path) != std::string::npos) return make_error(ErrorCode::ScriptError, *it);
        return make_error(ErrorCode::ScriptError, "cannot load " + std::string(script_path));
    }
    lua_State* L = h.L;
    std::vector<PropertyInfo> out;
    lua_getref(L, m->ref);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && editable(L, -1)) {
            const char* k = lua_tostring(L, -2);
            if (k[0] != '_') out.push_back({k, to_json(L, -1)});
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::sort(out.begin(), out.end(), [](const PropertyInfo& a, const PropertyInfo& b) { return a.name < b.name; });
    return out;
}

Result<json::Value> ScriptHost::eval(std::string_view source, scene::Scene* scene, std::string_view chunk_name) {
    Impl& h = *impl_;
    lua_State* L = h.L;
    const int top = lua_gettop(L);
    scene::Scene* prev = h.scene;
    if (scene) h.scene = scene;
    std::string error;
    if (!h.load_chunk(source, "=" + std::string(chunk_name), error)) {
        h.scene = prev;
        lua_settop(L, top);
        return make_error(ErrorCode::ScriptError, error);
    }
    const std::uint64_t errors_before = h.counters.errors;
    const bool ok = h.pcall(0, 1, chunk_name);
    h.scene = prev;
    if (!ok) {
        lua_settop(L, top);
        return make_error(ErrorCode::ScriptError,
                          h.counters.errors > errors_before && !h.recent_errors.empty() ? h.recent_errors.back()
                                                                                         : "script error");
    }
    Value v = to_json(L, -1);
    lua_settop(L, top);
    return v;
}

void ScriptHost::reset_instances() {
    for (auto& [k, in] : impl_->instances) impl_->drop(in);
    impl_->instances.clear();
}

HostStats ScriptHost::stats() const {
    HostStats s = impl_->counters;
    s.modules = impl_->modules.size();
    s.instances = impl_->instances.size();
    s.faulted = static_cast<std::size_t>(
        std::count_if(impl_->instances.begin(), impl_->instances.end(), [](const auto& p) { return p.second.faulted; }));
    s.heap_bytes = impl_->heap;
    return s;
}

const std::vector<std::string>& ScriptHost::recent_errors() const noexcept { return impl_->recent_errors; }
const HostOptions& ScriptHost::options() const noexcept { return impl_->options; }

void add_script_systems(gameplay::Simulation& sim, ScriptHost& host) {
    gameplay::Simulation* simp = &sim;
    sim.add_system("script.fixed_update", gameplay::Phase::FixedUpdate,
                   [&host, simp](scene::Scene& s, const gameplay::FrameTime& t) { host.fixed_update(s, t, simp->input); });
    sim.add_system("script.update", gameplay::Phase::Update,
                   [&host, simp](scene::Scene& s, const gameplay::FrameTime& t) { host.update(s, t, simp->input); });
}

Status check_syntax(std::string_view source, std::string_view chunk_name) {
    lua_CompileOptions opts{};
    std::size_t size = 0;
    char* bc = luau_compile(source.data(), source.size(), &opts, &size);
    if (!bc) return make_error(ErrorCode::OutOfMemory, "compiler out of memory");
    Status st;
    if (size > 0 && bc[0] == 0) {
        // Error bytecode: version byte 0 followed by the message, which starts with ":line:".
        st = make_error(ErrorCode::ScriptError, std::string(chunk_name) + std::string(bc + 1, size - 1));
    }
    std::free(bc);
    return st;
}

} // namespace victus::script
