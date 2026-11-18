// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/animation.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/script/script.hpp"
#include "victus/graph/graph.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/audio/audio.hpp"
#include "victus/physics/physics.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <unordered_map>

namespace victus::script {

using json::Value;
using scene::Entity;
namespace sfs = std::filesystem;
using graph::annotate_graph_error;
using graph::compile_graph;
using graph::CompiledGraph;
using graph::GraphCompileOptions;
using graph::parse_graph;
using graph::ScriptGraph;

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
    std::unordered_map<std::string, PrefabSource> prefabs;
    // Asset paths resolved by the rendering API, both ways (material() reports the path).
    std::unordered_map<std::string, Uuid> asset_uuids;
    std::unordered_map<Uuid, std::string, UuidHash> asset_paths;
    HostStats counters;
    double next_watch{0};

    // Saves: created on first use when options.save_directory is set.
    std::unique_ptr<gameplay::SaveStore> save_store;
    std::unordered_map<std::uint32_t, int> migration_refs; // from-version -> Luau function ref
    std::optional<Value> pending_restore;                  // checkpoint applied after update
    std::unordered_map<std::string, Value> pending_states; // entity id -> load_state() argument
    // Scene transitions: the running scene's path, the params it was loaded with and a
    // validated transition applied at the end of the update.
    struct SceneRequest {
        std::string path;
        Value doc;
        Value params;
    };
    std::string current_scene;
    Value scene_params;
    std::optional<SceneRequest> pending_scene;
    lua_State* active{nullptr};                            // thread running save.read (migrations)

    // Helper processes opened with bridge.open (kept across scene transitions, closed with the host).
    struct Bridge {
        std::string name;
        std::unique_ptr<process::ChildProcess> child;
        std::vector<Value> inbox; // messages read while waiting for a request's reply
        std::uint64_t next_id{1};
        std::uint64_t requests{0};
        double wait_seconds{0};
    };
    std::unordered_map<int, Bridge> bridges;
    int next_bridge{1};
    // The AI system whose unregistered action leaves run script methods (see run_script_task).
    ai::AiSystem* bound_ai{nullptr};

    gameplay::SaveStore* store() {
        if (!save_store && !options.save_directory.empty())
            save_store = std::make_unique<gameplay::SaveStore>(options.save_directory, options.save_version);
        return save_store.get();
    }

    // Context of the current call.
    scene::Scene* scene{nullptr};
    gameplay::FrameTime time{};
    float fixed_dt{1.0f / 60.0f};
    const gameplay::InputSource* input{nullptr};
    const gameplay::InputSource* default_input{nullptr};

    // Selects the input `e`'s script reads: multiplayer sessions route remote players' inputs to
    // the entities they own (and give proxies no input at all).
    void route(Entity e) {
        input = default_input;
        if (!scene) return;
        if (const auto* r = scene->world().resource<gameplay::InputRouting>(); r && r->route)
            input = r->route(e, default_input);
    }

    // Scene resources that point at this host (capture probes) check it is still alive.
    std::shared_ptr<char> alive = std::make_shared<char>();

    ~Impl() {
        if (L) lua_close(L);
    }

    // Compiled visual script graphs by module path (runtime errors name the failing node).
    struct GraphModule {
        ScriptGraph graph;
        CompiledGraph compiled;
    };
    std::unordered_map<std::string, GraphModule> graphs;
    // Visual script debugging (HostOptions::trace_graphs): node run counts and the last error.
    std::unordered_map<std::string, std::unordered_map<std::uint32_t, std::uint64_t>> graph_hits;
    std::unordered_map<std::string, GraphRuntimeError> graph_errors;

    void report(std::string message) {
        if (message.find(".vgraph:") != std::string::npos)
            for (const auto& [path, gm] : graphs)
                if (message.find(path + ":") != std::string::npos) {
                    message = annotate_graph_error(message, gm.compiled, gm.graph);
                    GraphRuntimeError& ge = graph_errors[path];
                    ge.node = 0;
                    if (const auto at = message.rfind("[node "); at != std::string::npos)
                        ge.node = static_cast<std::uint32_t>(std::strtoul(message.c_str() + at + 6, nullptr, 10));
                    ge.message = message;
                    ge.serial = counters.errors + 1;
                    break;
                }
        ++counters.errors;
        VICTUS_LOG_ERROR("script", "{}", message);
        if (recent_errors.size() >= kMaxRecentErrors) recent_errors.erase(recent_errors.begin());
        recent_errors.push_back(std::move(message));
    }

    // Protected call with the time budget armed. On error the message is reported and popped.
    // Nested calls (a script's save.checkpoint calling other scripts' save_state) share the
    // outer call's budget.
    bool pcall(int nargs, int nresults, std::string_view what) { return pcall_on(L, nargs, nresults, what); }
    bool pcall_on(lua_State* T, int nargs, int nresults, std::string_view what) {
        const double outer = deadline;
        if (outer <= 0) {
            deadline = options.call_budget_seconds > 0 ? now_seconds() + options.call_budget_seconds : 0;
            interrupt_counter = 0;
        }
        const int st = lua_pcall(T, nargs, nresults, 0);
        deadline = outer;
        ++counters.calls;
        if (st == LUA_OK) return true;
        const char* msg = lua_tostring(T, -1);
        report(std::format("{}: {}", what, msg ? msg : "unknown error"));
        lua_pop(T, 1);
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

    std::vector<std::string> loading; // modules being loaded (require cycles)

    Module* module(std::string_view path) {
        auto it = modules.find(std::string(path));
        if (it != modules.end()) return it->second.ok ? &it->second : nullptr;
        if (std::find(loading.begin(), loading.end(), path) != loading.end()) return nullptr;
        loading.emplace_back(path);
        struct Pop {
            std::vector<std::string>& v;
            ~Pop() { v.pop_back(); }
        } pop{loading};
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
        if (m.path.ends_with(".vgraph")) { // visual script: compile the graph to Luau (ADR-0058)
            auto graph = parse_graph(*text);
            if (!graph) {
                report(std::format("{}: {}", m.path, graph.error().message));
                return false;
            }
            GraphCompileOptions gopt;
            gopt.source_name = m.path;
            gopt.trace = options.trace_graphs;
            CompiledGraph compiled = compile_graph(*graph, gopt);
            if (!compiled.ok()) {
                report(std::format("{}: {}", m.path, compiled.error_text()));
                return false;
            }
            *text = compiled.source;
            graphs[m.path] = {std::move(*graph), std::move(compiled)};
            graph_errors.erase(m.path);
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

    // Calls instance method `fn(self, ...)` if defined; `push` pushes extra arguments and returns
    // how many it pushed.
    template <typename Push>
    bool call_with(Instance& in, const char* fn, Push&& push) {
        lua_getref(L, in.ref);
        lua_getfield(L, -1, fn);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            return true;
        }
        lua_pushvalue(L, -2);
        const int nargs = 1 + push();
        const bool ok = pcall(nargs, 0, std::format("{}:{}", in.path, fn));
        lua_pop(L, 1); // instance
        if (!ok) {
            in.faulted = true;
            VICTUS_LOG_WARN("script", "{} disabled until it is reloaded", in.path);
        }
        return ok;
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

void push_entity(lua_State* L, Entity e);

// `refs`: objects of the form {"$entity": id} become entities of the active scene (save data).
void push_json(lua_State* L, const Value& v, int depth = 0, bool refs = false) {
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
Value to_json(lua_State* L, int idx, int depth = 0, bool refs = false) {
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
    else if (std::strcmp(key, "velocity") == 0) {
        const auto* w = physics::world_of(s);
        push_vec3(L, w ? w->linear_velocity(e) : math::Vec3{});
    } else if (std::strcmp(key, "angular_velocity") == 0) {
        const auto* w = physics::world_of(s);
        push_vec3(L, w ? w->angular_velocity(e) : math::Vec3{});
    } else if (std::strcmp(key, "grounded") == 0) {
        const auto* w = physics::world_of(s);
        lua_pushboolean(L, w && w->character_state(e).grounded);
    } else luaL_error(L, "Entity has no member '%s'", key);
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
    } else if (std::strcmp(key, "velocity") == 0) {
        if (auto* w = physics::world_of(s)) w->set_linear_velocity(e, check_vec3(L, 3));
    } else if (std::strcmp(key, "angular_velocity") == 0) {
        if (auto* w = physics::world_of(s)) w->set_angular_velocity(e, check_vec3(L, 3));
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

} // namespace

Result<PrefabSource> read_prefab(const std::filesystem::path& assets_root, std::string_view path) {
    auto file = fs::resolve_under(assets_root, path);
    if (!file) return std::move(file).error();
    auto prefab = scene::load_prefab_file(*file);
    if (!prefab) return std::move(prefab).error();
    PrefabSource src;
    src.prefab = std::move(*prefab);
    sfs::path meta = *file;
    meta += ".meta";
    if (auto text = fs::read_text(meta, 1u << 20)) {
        if (auto doc = json::parse(*text); doc && (*doc)["uuid"].is_string()) (void)Uuid::parse((*doc)["uuid"].as_string(), src.asset);
    }
    return src;
}

namespace {

// scene.instantiate(path, {position = vector, parent = Entity, name = string}?) -> Entity
int scene_instantiate(lua_State* L) {
    auto& h = host_of(L);
    auto& s = scene_of(L);
    const std::string path = luaL_checkstring(L, 1);
    scene::InstantiateOptions o;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "position");
        if (!lua_isnil(L, -1)) o.position = check_vec3(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "parent");
        if (!lua_isnil(L, -1)) o.parent = check_live(L, -1, s);
        lua_pop(L, 1);
        lua_getfield(L, 2, "name");
        if (!lua_isnil(L, -1)) o.name = luaL_checkstring(L, -1);
        lua_pop(L, 1);
    }
    auto it = h.prefabs.find(path);
    if (it == h.prefabs.end()) {
        auto loaded = h.options.prefab_loader ? h.options.prefab_loader(path) : read_prefab(h.options.assets_root, path);
        if (!loaded) luaL_error(L, "scene.instantiate: cannot load prefab '%s': %s", path.c_str(), loaded.error().message.c_str());
        it = h.prefabs.emplace(path, std::move(*loaded)).first;
    }
    auto e = scene::instantiate(s, it->second.prefab, it->second.asset, o);
    if (!e) luaL_error(L, "scene.instantiate('%s'): %s", path.c_str(), e.error().message.c_str());
    push_entity(L, *e);
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
// input.mouse_wheel() -> wheel notches this frame (positive away from the user)
int input_mouse_wheel(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushnumber(L, in ? in->mouse_wheel() : 0.0);
    return 1;
}
// input.touch_count() -> fingers currently on the screen
int input_touch_count(lua_State* L) {
    const auto* in = host_of(L).input;
    lua_pushinteger(L, in ? in->touch_count() : 0);
    return 1;
}
// input.touch(i) -> vector(x, y, pressure) normalized to the window, and the finger id; nil when
// there is no i-th finger (1-based, in touch order)
int input_touch(lua_State* L) {
    const auto* in = host_of(L).input;
    const int i = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    if (!in || i < 0 || i >= in->touch_count()) {
        lua_pushnil(L);
        return 1;
    }
    const gameplay::Touch t = in->touch(i);
    lua_pushvector(L, t.position.x, t.position.y, t.pressure);
    lua_pushnumber(L, static_cast<double>(t.id));
    return 2;
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
        if (in && in->key_down(pos)) v = v < 0 ? 0.0f : 1.0f;
        if (v != 0) break;
    }
    lua_pushnumber(L, v);
    return 1;
}

const gameplay::InputAction& action_arg(lua_State* L, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const auto* a = host_of(L).options.input_map.find(name);
    if (!a) luaL_error(L, "unknown input action '%s' (see the project's input actions)", name);
    return *a;
}
// input.action(name) -> held; action_pressed / action_released -> this frame's edges
int input_action(lua_State* L) {
    lua_pushboolean(L, gameplay::InputMap::down(host_of(L).input, action_arg(L, 1)));
    return 1;
}
int input_action_pressed(lua_State* L) {
    lua_pushboolean(L, gameplay::InputMap::pressed(host_of(L).input, action_arg(L, 1)));
    return 1;
}
int input_action_released(lua_State* L) {
    lua_pushboolean(L, gameplay::InputMap::released(host_of(L).input, action_arg(L, 1)));
    return 1;
}
// input.value(name) -> -1..1 (analog sticks give partial values)
int input_value(lua_State* L) {
    lua_pushnumber(L, gameplay::InputMap::value(host_of(L).input, action_arg(L, 1)));
    return 1;
}
// input.vector(x_action, y_action) -> vector(x, y, 0) with length <= 1
int input_vector(lua_State* L) {
    const auto* in = host_of(L).input;
    float x = gameplay::InputMap::value(in, action_arg(L, 1));
    float y = gameplay::InputMap::value(in, action_arg(L, 2));
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) x /= len, y /= len;
    lua_pushvector(L, x, y, 0);
    return 1;
}
// input.actions() -> {names}; input.bindings(name) -> {"Space", "Pad.South", ...} (negative
// bindings are prefixed with "-")
int input_actions(lua_State* L) {
    const auto& list = host_of(L).options.input_map.actions;
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        lua_pushstring(L, list[i].name.c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}
int input_bindings(lua_State* L) {
    const auto& a = action_arg(L, 1);
    lua_createtable(L, static_cast<int>(a.positive.size() + a.negative.size()), 0);
    int n = 0;
    for (const auto& b : a.positive) {
        lua_pushstring(L, gameplay::binding_to_string(b).c_str());
        lua_rawseti(L, -2, ++n);
    }
    for (const auto& b : a.negative) {
        lua_pushstring(L, ("-" + gameplay::binding_to_string(b)).c_str());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// input.rebind(name, {bindings...}) -> ok, error. Creates the action if needed; "-X" entries
// are negative bindings. Lasts for this session (projects keep their saved bindings).
int input_rebind(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    gameplay::InputAction a;
    a.name = name;
    if (const auto* old = host_of(L).options.input_map.find(name)) a.dead_zone = old->dead_zone;
    const int n = lua_objlen(L, 2);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        const char* t = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (!t) luaL_error(L, "input.rebind: bindings must be strings");
        std::string_view text = t;
        const bool negative = text.size() > 1 && text[0] == '-';
        if (negative) text.remove_prefix(1);
        auto b = gameplay::parse_binding(text);
        if (!b) {
            lua_pushboolean(L, false);
            lua_pushstring(L, b.error().message.c_str());
            return 2;
        }
        (negative ? a.negative : a.positive).push_back(std::move(*b));
    }
    auto& actions = host_of(L).options.input_map.actions;
    auto it = std::find_if(actions.begin(), actions.end(), [&](const auto& x) { return x.name == a.name; });
    if (it != actions.end()) *it = std::move(a);
    else actions.push_back(std::move(a));
    lua_pushboolean(L, true);
    return 1;
}
int pad_arg(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return -1;
    const int p = luaL_checkinteger(L, idx);
    if (p < 0 || p >= gameplay::kMaxGamepads) luaL_error(L, "gamepad index %d out of range 0..%d", p, gameplay::kMaxGamepads - 1);
    return p;
}
// input.gamepad_connected(pad?) -> bool (any gamepad when pad is nil)
int input_gamepad_connected(lua_State* L) {
    const auto* in = host_of(L).input;
    const int pad = pad_arg(L, 1);
    bool c = false;
    for (int p = 0; in && p < gameplay::kMaxGamepads; ++p)
        if ((pad < 0 || p == pad) && in->gamepad_connected(p)) c = true;
    lua_pushboolean(L, c);
    return 1;
}
// input.gamepad_button("South", pad?) -> held on that (or any) gamepad
int input_gamepad_button(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const int b = gameplay::gamepad_button_index(name);
    if (b < 0) luaL_error(L, "unknown gamepad button '%s'", name);
    const auto* in = host_of(L).input;
    const int pad = pad_arg(L, 2);
    bool down = false;
    for (int p = 0; in && p < gameplay::kMaxGamepads; ++p)
        if ((pad < 0 || p == pad) && in->gamepad_connected(p) && in->gamepad_button(p, b)) down = true;
    lua_pushboolean(L, down);
    return 1;
}
// input.gamepad_axis("LeftX", pad?) -> -1..1 (triggers 0..1); the strongest pad when nil
int input_gamepad_axis(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const int a = gameplay::gamepad_axis_index(name);
    if (a < 0) luaL_error(L, "unknown gamepad axis '%s'", name);
    const auto* in = host_of(L).input;
    const int pad = pad_arg(L, 2);
    float v = 0;
    for (int p = 0; in && p < gameplay::kMaxGamepads; ++p) {
        if ((pad >= 0 && p != pad) || !in->gamepad_connected(p)) continue;
        const float x = in->gamepad_axis(p, a);
        if (std::fabs(x) > std::fabs(v)) v = x;
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

// Visual script tracing (ADR-0060): `__vg_trace_for(graph)` returns `function(node)` counting runs.
int graph_trace_hit(lua_State* L) {
    const char* graph = lua_tostring(L, lua_upvalueindex(1));
    const auto node = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    auto* h = static_cast<ScriptHost::Impl*>(lua_callbacks(L)->userdata);
    if (h && graph && h->options.trace_graphs) ++h->graph_hits[graph][node];
    return 0;
}
int graph_trace_for(lua_State* L) {
    luaL_checkstring(L, 1);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, graph_trace_hit, "__vg_trace", 1);
    return 1;
}

void register_lib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, name);
}

// ---- physics ----

physics::PhysicsWorld* physics_of(lua_State* L) { return physics::world_of(scene_of(L)); }

physics::PhysicsWorld& check_physics(lua_State* L) {
    auto* w = physics_of(L);
    if (!w) luaL_error(L, "physics is not running for this scene");
    return *w;
}

std::uint32_t opt_mask(lua_State* L, int idx) {
    return static_cast<std::uint32_t>(luaL_optinteger(L, idx, physics::kAllLayers));
}

void push_hit(lua_State* L, const std::optional<physics::RaycastHit>& hit) {
    if (!hit) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, hit->entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->distance);
    lua_setfield(L, -2, "distance");
}

void push_entities(lua_State* L, const std::vector<Entity>& list) {
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        push_entity(L, list[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

// physics.raycast(origin, direction, max_distance = 1000, mask = all, ignore = nil)
int physics_raycast(lua_State* L) {
    auto& w = check_physics(L);
    push_hit(L, w.raycast(check_vec3(L, 1), check_vec3(L, 2), static_cast<float>(luaL_optnumber(L, 3, 1000.0)),
                          opt_mask(L, 4), opt_entity(L, 5)));
    return 1;
}
// physics.raycast2d(origin, direction, ...) - x/y of the vectors are used.
int physics_raycast2d(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 o = check_vec3(L, 1), d = check_vec3(L, 2);
    push_hit(L, w.raycast_2d({o.x, o.y}, {d.x, d.y}, static_cast<float>(luaL_optnumber(L, 3, 1000.0)), opt_mask(L, 4),
                             opt_entity(L, 5)));
    return 1;
}
int physics_overlap_sphere(lua_State* L) {
    auto& w = check_physics(L);
    push_entities(L, w.overlap_sphere(check_vec3(L, 1), static_cast<float>(luaL_checknumber(L, 2)), opt_mask(L, 3)));
    return 1;
}
int physics_overlap_box(lua_State* L) {
    auto& w = check_physics(L);
    const math::Quat q = lua_isnoneornil(L, 3) ? math::Quat{} : from_euler_degrees(check_vec3(L, 3));
    push_entities(L, w.overlap_box(check_vec3(L, 1), check_vec3(L, 2), q, opt_mask(L, 4)));
    return 1;
}
int physics_overlap_circle(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 c = check_vec3(L, 1);
    push_entities(L, w.overlap_circle_2d({c.x, c.y}, static_cast<float>(luaL_checknumber(L, 2)), opt_mask(L, 3)));
    return 1;
}
int physics_overlap_box2d(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 c = check_vec3(L, 1), h = check_vec3(L, 2);
    const float angle = math::radians(static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    push_entities(L, w.overlap_box_2d({c.x, c.y}, {h.x, h.y}, angle, opt_mask(L, 4)));
    return 1;
}
int physics_gravity(lua_State* L) {
    push_vec3(L, check_physics(L).gravity());
    return 1;
}
int physics_set_gravity(lua_State* L) {
    check_physics(L).set_gravity(check_vec3(L, 1));
    return 0;
}
int physics_gravity2d(lua_State* L) {
    const math::Vec2 g = check_physics(L).gravity_2d();
    lua_pushvector(L, g.x, g.y, 0);
    return 1;
}
int physics_set_gravity2d(lua_State* L) {
    const math::Vec3 g = check_vec3(L, 1);
    check_physics(L).set_gravity_2d({g.x, g.y});
    return 0;
}
int physics_available(lua_State* L) {
    lua_pushboolean(L, physics_of(L) != nullptr);
    return 1;
}

template <void (physics::PhysicsWorld::*Fn)(Entity, math::Vec3)>
int entity_physics_apply(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* w = physics::world_of(s)) (w->*Fn)(e, check_vec3(L, 2));
    return 0;
}

// entity:move_character(velocity): desired walking velocity (persists until changed).
int entity_move_character(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* w = physics::world_of(s)) w->move_character(e, check_vec3(L, 2));
    return 0;
}

// entity:jump(speed) -> bool (false when not grounded)
int entity_jump(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* w = physics::world_of(s);
    lua_pushboolean(L, w && w->jump_character(e, static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}

// entity:drop_through(seconds?) -> bool: a 2D character ignores one-way platforms for `seconds`
// (default 0.25) so it falls through the one it stands on. False for anything but a 2D character.
int entity_drop_through(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* w = physics::world_of(s);
    lua_pushboolean(L, w && w->drop_through(e, static_cast<float>(luaL_optnumber(L, 2, 0.25))));
    return 1;
}

// ---- In-game UI ---------------------------------------------------------------------------------

// entity:set_text(value): sets the entity's UiText (numbers and booleans are converted).
int entity_set_text(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    size_t len = 0;
    const char* text = luaL_tolstring(L, 2, &len);
    auto* t = s.world().get<gameui::UiTextComponent>(e);
    if (!t) luaL_error(L, "entity has no UiText component");
    t->text.assign(text, len);
    return 0;
}
int entity_text(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* t = s.world().get<gameui::UiTextComponent>(e);
    if (t) lua_pushlstring(L, t->text.data(), t->text.size());
    else lua_pushnil(L);
    return 1;
}
// entity:set_visible(bool): the UiCanvas when the entity is a canvas root, else its UiRect, else
// its MeshRenderer.
int entity_set_visible(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool v = lua_toboolean(L, 2) != 0;
    if (auto* c = s.world().get<gameui::UiCanvasComponent>(e)) c->visible = v;
    else if (auto* r = s.world().get<gameui::UiRectComponent>(e)) r->visible = v;
    else if (auto* m = s.world().get<scene::MeshRendererComponent>(e)) m->visible = v;
    else luaL_error(L, "entity has no UiRect, UiCanvas or MeshRenderer component");
    return 0;
}
int entity_visible(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (const auto* c = s.world().get<gameui::UiCanvasComponent>(e)) lua_pushboolean(L, c->visible);
    else if (const auto* r = s.world().get<gameui::UiRectComponent>(e)) lua_pushboolean(L, r->visible);
    else if (const auto* m = s.world().get<scene::MeshRendererComponent>(e)) lua_pushboolean(L, m->visible);
    else lua_pushnil(L);
    return 1;
}
// ---- Rendering ----------------------------------------------------------------------------------

constexpr const char* kPrimitives[] = {"cube", "sphere", "plane", "cylinder"};

bool is_primitive(std::string_view name) {
    return std::ranges::any_of(kPrimitives, [&](const char* p) { return name == p; });
}

// Resolves an asset path (or a UUID string) to the asset's UUID; raises a Luau error on failure.
Uuid resolve_asset(lua_State* L, std::string_view path, const char* fn) {
    auto& h = host_of(L);
    const std::string key(path);
    if (const auto it = h.asset_uuids.find(key); it != h.asset_uuids.end()) return it->second;
    Uuid id;
    if (!Uuid::parse(path, id)) {
        if (h.options.asset_resolver) {
            auto r = h.options.asset_resolver(path);
            if (!r) luaL_error(L, "%s: cannot resolve '%s': %s", fn, key.c_str(), r.error().message.c_str());
            id = *r;
        } else {
            auto file = fs::resolve_under(h.options.assets_root, path);
            if (!file) luaL_error(L, "%s: cannot resolve '%s': %s", fn, key.c_str(), file.error().message.c_str());
            sfs::path meta = *file;
            meta += ".meta";
            bool found = false;
            if (auto text = fs::read_text(meta, 1u << 20))
                if (auto doc = json::parse(*text); doc && (*doc)["uuid"].is_string()) found = Uuid::parse((*doc)["uuid"].as_string(), id);
            if (!found) luaL_error(L, "%s: '%s' is not an imported asset (no UUID in its .meta file)", fn, key.c_str());
        }
        if (id.is_nil()) luaL_error(L, "%s: '%s' has no asset UUID", fn, key.c_str());
    }
    h.asset_uuids.emplace(key, id);
    h.asset_paths.emplace(id, key);
    return id;
}

void push_asset(lua_State* L, const Uuid& id) {
    if (id.is_nil()) {
        lua_pushnil(L);
        return;
    }
    auto& h = host_of(L);
    const auto it = h.asset_paths.find(id);
    const std::string text = it != h.asset_paths.end() ? it->second : id.to_string();
    lua_pushlstring(L, text.data(), text.size());
}

scene::MeshRendererComponent& check_mesh_renderer(lua_State* L, scene::Scene& s, Entity e) {
    auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) luaL_error(L, "entity has no MeshRenderer component");
    return *m;
}

// entity:set_material(path | uuid | nil): the MeshRenderer's .vmat material (nil: the mesh's own).
int entity_set_material(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = check_mesh_renderer(L, s, e);
    if (lua_isnoneornil(L, 2)) {
        m.material_asset = {};
        return 0;
    }
    const std::string_view path = luaL_checkstring(L, 2);
    Uuid tmp;
    if (!Uuid::parse(path, tmp) && !path.ends_with(".vmat")) luaL_error(L, "set_material: '%s' is not a .vmat material", std::string(path).c_str());
    m.material_asset = resolve_asset(L, path, "set_material");
    return 0;
}
// entity:material() -> path (or UUID string when set outside scripts) | nil
int entity_material(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) lua_pushnil(L);
    else push_asset(L, m->material_asset);
    return 1;
}
// ---- procedural meshes ---------------------------------------------------------------------------

// One attribute of a packed vertex layout ("position:f32x3,normal:s8x4,color:u8x4,uv:f32x2,skip:4").
struct PackedAttribute {
    enum class Kind : std::uint8_t { Position, Normal, Uv, Color, Skip } kind{Kind::Skip};
    enum class Type : std::uint8_t { F32, S8, U8, S16, U16, Bytes } type{Type::Bytes};
    std::uint32_t count{0};  // components
    std::uint32_t offset{0}; // bytes from the vertex start
};

std::uint32_t component_bytes(PackedAttribute::Type t) {
    switch (t) {
    case PackedAttribute::Type::F32: return 4;
    case PackedAttribute::Type::S16:
    case PackedAttribute::Type::U16: return 2;
    default: return 1;
    }
}

// Parses a layout string; returns the stride or 0 with `error` set.
std::uint32_t parse_layout(std::string_view text, std::vector<PackedAttribute>& out, std::string& error) {
    out.clear();
    std::uint32_t offset = 0;
    bool position = false;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            error = std::format("layout item '{}' is not name:type", item);
            return 0;
        }
        const std::string_view name = item.substr(0, colon), type = item.substr(colon + 1);
        PackedAttribute a;
        a.offset = offset;
        if (name == "skip") {
            int n = 0;
            for (const char c : type) {
                if (c < '0' || c > '9' || n > 4096) {
                    error = std::format("layout: skip needs a byte count, got '{}'", type);
                    return 0;
                }
                n = n * 10 + (c - '0');
            }
            a.kind = PackedAttribute::Kind::Skip;
            a.count = static_cast<std::uint32_t>(n);
            offset += a.count;
            out.push_back(a);
            continue;
        }
        if (name == "position") a.kind = PackedAttribute::Kind::Position;
        else if (name == "normal") a.kind = PackedAttribute::Kind::Normal;
        else if (name == "uv") a.kind = PackedAttribute::Kind::Uv;
        else if (name == "color") a.kind = PackedAttribute::Kind::Color;
        else {
            error = std::format("layout: unknown attribute '{}' (position, normal, uv, color, skip)", name);
            return 0;
        }
        const std::size_t x = type.find('x');
        const std::string_view base = type.substr(0, x);
        const int count = x == std::string_view::npos || x + 2 != type.size() ? 0 : type[x + 1] - '0';
        if (base == "f32") a.type = PackedAttribute::Type::F32;
        else if (base == "s8") a.type = PackedAttribute::Type::S8;
        else if (base == "u8") a.type = PackedAttribute::Type::U8;
        else if (base == "s16") a.type = PackedAttribute::Type::S16;
        else if (base == "u16") a.type = PackedAttribute::Type::U16;
        const int need = a.kind == PackedAttribute::Kind::Uv ? 2 : 3;
        if (a.type == PackedAttribute::Type::Bytes || count < need || count > 4) {
            error = std::format("layout: '{}' needs a type like f32x{} / s8x4 / u8x4, got '{}'", name, need, type);
            return 0;
        }
        if (a.kind == PackedAttribute::Kind::Color && a.type != PackedAttribute::Type::U8 && a.type != PackedAttribute::Type::F32) {
            error = "layout: color must be u8x3, u8x4, f32x3 or f32x4";
            return 0;
        }
        if (a.kind == PackedAttribute::Kind::Position && a.type != PackedAttribute::Type::F32) {
            error = "layout: position must be f32x3";
            return 0;
        }
        position = position || a.kind == PackedAttribute::Kind::Position;
        a.count = static_cast<std::uint32_t>(count);
        offset += a.count * component_bytes(a.type);
        out.push_back(a);
    }
    if (!position) {
        error = "layout has no position";
        return 0;
    }
    return offset;
}

float read_component(const std::uint8_t* p, PackedAttribute::Type t, std::uint32_t i) {
    switch (t) {
    case PackedAttribute::Type::F32: {
        float f;
        std::memcpy(&f, p + i * 4, 4);
        return std::isfinite(f) ? f : 0.0f;
    }
    case PackedAttribute::Type::S8: return std::max(-1.0f, static_cast<float>(static_cast<std::int8_t>(p[i])) / 127.0f);
    case PackedAttribute::Type::U8: return static_cast<float>(p[i]) / 255.0f;
    case PackedAttribute::Type::S16: {
        std::int16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return std::max(-1.0f, static_cast<float>(v) / 32767.0f);
    }
    case PackedAttribute::Type::U16: {
        std::uint16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return static_cast<float>(v) / 65535.0f;
    }
    default: return 0.0f;
    }
}

std::uint32_t pack_linear_color(float r, float g, float b, float a) {
    const auto enc = [](float c, bool srgb) {
        c = std::clamp(c, 0.0f, 1.0f);
        if (srgb) c = math::linear_to_srgb(c);
        return static_cast<std::uint32_t>(std::lround(c * 255.0f));
    };
    return enc(r, true) | (enc(g, true) << 8) | (enc(b, true) << 16) | (enc(a, false) << 24);
}

// Bytes of a vertices/indices field: a base64 string (encoding "base64", the default) or raw bytes.
std::vector<std::uint8_t> field_bytes(lua_State* L, int spec, const char* field, bool base64) {
    lua_getfield(L, spec, field);
    std::size_t n = 0;
    const char* text = lua_tolstring(L, -1, &n);
    if (!text) luaL_error(L, "set_mesh_data: '%s' must be a string", field);
    std::vector<std::uint8_t> out;
    if (base64) {
        if (!base64_decode(std::string_view(text, n), out)) luaL_error(L, "set_mesh_data: '%s' is not valid base64", field);
    } else {
        out.assign(reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + n);
    }
    lua_pop(L, 1);
    return out;
}

constexpr std::size_t kMaxProceduralVertices = 4u << 20;

// Packed form: {vertices = string, indices = string, layout = "position:f32x3,...",
// index = "u32" | "u16", encoding = "base64" | "binary"}.
scene::ProceduralMesh mesh_from_packed(lua_State* L, int spec) {
    lua_getfield(L, spec, "encoding");
    const std::string encoding = luaL_optstring(L, -1, "base64");
    lua_pop(L, 1);
    if (encoding != "base64" && encoding != "binary") luaL_error(L, "set_mesh_data: encoding must be 'base64' or 'binary'");
    lua_getfield(L, spec, "layout");
    const std::string layout_text = luaL_optstring(L, -1, "position:f32x3,normal:f32x3");
    lua_pop(L, 1);
    lua_getfield(L, spec, "index");
    const std::string index_type = luaL_optstring(L, -1, "u32");
    lua_pop(L, 1);
    if (index_type != "u32" && index_type != "u16") luaL_error(L, "set_mesh_data: index must be 'u32' or 'u16'");
    std::vector<PackedAttribute> layout;
    std::string error;
    const std::uint32_t stride = parse_layout(layout_text, layout, error);
    if (stride == 0) luaL_error(L, "set_mesh_data: %s", error.c_str());
    const bool b64 = encoding == "base64";
    const std::vector<std::uint8_t> vb = field_bytes(L, spec, "vertices", b64);
    const std::vector<std::uint8_t> ib = field_bytes(L, spec, "indices", b64);
    if (vb.size() % stride != 0)
        luaL_error(L, "set_mesh_data: %d vertex bytes is not a multiple of the %d-byte layout", static_cast<int>(vb.size()), static_cast<int>(stride));
    const std::size_t n = vb.size() / stride;
    if (n > kMaxProceduralVertices) luaL_error(L, "set_mesh_data: too many vertices (%d)", static_cast<int>(n));
    const std::size_t isize = index_type == "u32" ? 4 : 2;
    if (ib.size() % isize != 0) luaL_error(L, "set_mesh_data: index bytes are not a multiple of %d", static_cast<int>(isize));
    scene::ProceduralMesh m;
    bool has_normal = false, has_uv = false, has_color = false;
    for (const auto& a : layout) {
        has_normal = has_normal || a.kind == PackedAttribute::Kind::Normal;
        has_uv = has_uv || a.kind == PackedAttribute::Kind::Uv;
        has_color = has_color || a.kind == PackedAttribute::Kind::Color;
    }
    m.positions.resize(n);
    if (has_normal) m.normals.resize(n);
    if (has_uv) m.uvs.resize(n);
    if (has_color) m.colors.resize(n);
    for (std::size_t v = 0; v < n; ++v) {
        const std::uint8_t* base = vb.data() + v * stride;
        for (const auto& a : layout) {
            const std::uint8_t* p = base + a.offset;
            switch (a.kind) {
            case PackedAttribute::Kind::Position:
                m.positions[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1), read_component(p, a.type, 2)};
                break;
            case PackedAttribute::Kind::Normal:
                m.normals[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1), read_component(p, a.type, 2)};
                break;
            case PackedAttribute::Kind::Uv: m.uvs[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1)}; break;
            case PackedAttribute::Kind::Color:
                if (a.type == PackedAttribute::Type::U8) // bytes are taken as sRGB-encoded, alpha linear
                    m.colors[v] = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                                  (std::uint32_t{a.count == 4 ? p[3] : std::uint8_t{255}} << 24);
                else
                    m.colors[v] = pack_linear_color(read_component(p, a.type, 0), read_component(p, a.type, 1),
                                                    read_component(p, a.type, 2), a.count == 4 ? read_component(p, a.type, 3) : 1.0f);
                break;
            case PackedAttribute::Kind::Skip: break;
            }
        }
    }
    m.indices.resize(ib.size() / isize);
    for (std::size_t i = 0; i < m.indices.size(); ++i) {
        if (isize == 4) std::memcpy(&m.indices[i], ib.data() + i * 4, 4);
        else {
            std::uint16_t x;
            std::memcpy(&x, ib.data() + i * 2, 2);
            m.indices[i] = x;
        }
    }
    return m;
}

// Table form: {positions = {vector...}, indices = {integer...} (1-based), normals = {vector...}?,
// colors = {vector...}? (linear RGB), uvs = {{u, v}...}?}.
scene::ProceduralMesh mesh_from_tables(lua_State* L, int spec) {
    scene::ProceduralMesh m;
    lua_getfield(L, spec, "positions");
    luaL_checktype(L, -1, LUA_TTABLE);
    const int n = lua_objlen(L, -1);
    if (static_cast<std::size_t>(n) > kMaxProceduralVertices) luaL_error(L, "set_mesh_data: too many vertices");
    m.positions.reserve(static_cast<std::size_t>(n));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        m.positions.push_back(check_vec3(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "indices");
    luaL_checktype(L, -1, LUA_TTABLE);
    const int ni = lua_objlen(L, -1);
    for (int i = 1; i <= ni; ++i) {
        lua_rawgeti(L, -1, i);
        const auto idx = luaL_checkinteger(L, -1);
        if (idx < 1) luaL_error(L, "set_mesh_data: indices are 1-based (got %d)", static_cast<int>(idx));
        m.indices.push_back(static_cast<std::uint32_t>(idx - 1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "normals");
    if (lua_istable(L, -1)) {
        const int nn = lua_objlen(L, -1);
        for (int i = 1; i <= nn; ++i) {
            lua_rawgeti(L, -1, i);
            m.normals.push_back(check_vec3(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "colors");
    if (lua_istable(L, -1)) {
        const int nc = lua_objlen(L, -1);
        for (int i = 1; i <= nc; ++i) {
            lua_rawgeti(L, -1, i);
            const math::Vec3 c = check_vec3(L, -1);
            m.colors.push_back(pack_linear_color(c.x, c.y, c.z, 1.0f));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "uvs");
    if (lua_istable(L, -1)) {
        const int nu = lua_objlen(L, -1);
        for (int i = 1; i <= nu; ++i) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            m.uvs.push_back({static_cast<float>(luaL_checknumber(L, -2)), static_cast<float>(luaL_checknumber(L, -1))});
            lua_pop(L, 3);
        }
    }
    lua_pop(L, 1);
    return m;
}

// entity:set_mesh_data(spec) -> vertex count, triangle count. Builds a procedural mesh drawn by
// the entity's MeshRenderer (added when missing). Optional spec fields: scale (vector applied to
// positions; a mirroring scale also flips the winding), flip_winding (bool).
int entity_set_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "vertices");
    const bool packed = !lua_isnil(L, -1);
    lua_pop(L, 1);
    scene::ProceduralMesh m = packed ? mesh_from_packed(L, 2) : mesh_from_tables(L, 2);
    lua_getfield(L, 2, "scale");
    const math::Vec3 scale = lua_isnil(L, -1) ? math::Vec3{1, 1, 1} : check_vec3(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "flip_winding");
    bool flip = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (scale.x != 1 || scale.y != 1 || scale.z != 1) {
        if (scale.x == 0 || scale.y == 0 || scale.z == 0) luaL_error(L, "set_mesh_data: scale components must be nonzero");
        for (auto& p : m.positions) p = {p.x * scale.x, p.y * scale.y, p.z * scale.z};
        for (auto& nrm : m.normals) {
            const math::Vec3 t{nrm.x / scale.x, nrm.y / scale.y, nrm.z / scale.z};
            const float l = math::length(t);
            nrm = l > 1e-12f ? t * (1.0f / l) : math::Vec3{0, 1, 0};
        }
        if (scale.x * scale.y * scale.z < 0) flip = !flip;
    }
    if (flip)
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) std::swap(m.indices[t + 1], m.indices[t + 2]);
    const auto verts = m.positions.size();
    const auto tris = m.indices.size() / 3;
    auto published = scene::publish_mesh(std::move(m));
    if (!published) luaL_error(L, "set_mesh_data: %s", published.error().message.c_str());
    auto& w = s.world();
    if (!w.get<scene::MeshRendererComponent>(e)) w.add<scene::MeshRendererComponent>(e);
    if (auto* pm = w.get<scene::ProceduralMeshComponent>(e)) pm->mesh = std::move(*published);
    else w.add<scene::ProceduralMeshComponent>(e).mesh = std::move(*published);
    lua_pushinteger(L, static_cast<int>(verts));
    lua_pushinteger(L, static_cast<int>(tris));
    return 2;
}

// entity:clear_mesh_data(): drops the procedural mesh (the MeshRenderer draws its asset again).
int entity_clear_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (s.world().get<scene::ProceduralMeshComponent>(e)) s.world().remove<scene::ProceduralMeshComponent>(e);
    return 0;
}

// entity:mesh_data() -> {vertices, triangles, min, max} | nil
int entity_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* pm = s.world().get<scene::ProceduralMeshComponent>(e);
    if (!pm || !pm->mesh) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, static_cast<int>(pm->mesh->positions.size()));
    lua_setfield(L, -2, "vertices");
    lua_pushinteger(L, static_cast<int>(pm->mesh->indices.size() / 3));
    lua_setfield(L, -2, "triangles");
    if (pm->mesh->bounds.valid()) {
        lua_pushvector(L, pm->mesh->bounds.min.x, pm->mesh->bounds.min.y, pm->mesh->bounds.min.z);
        lua_setfield(L, -2, "min");
        lua_pushvector(L, pm->mesh->bounds.max.x, pm->mesh->bounds.max.y, pm->mesh->bounds.max.z);
        lua_setfield(L, -2, "max");
    }
    return 1;
}

// entity:set_mesh(primitive | model_path, mesh_index?)
int entity_set_mesh(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = check_mesh_renderer(L, s, e);
    const std::string_view name = luaL_checkstring(L, 2);
    const auto index = static_cast<std::uint32_t>(std::max(0, luaL_optinteger(L, 3, 0)));
    if (is_primitive(name)) {
        m.mesh_asset = {};
        m.primitive = std::string(name);
    } else {
        m.mesh_asset = resolve_asset(L, name, "set_mesh");
        m.primitive.clear();
    }
    m.mesh_index = index;
    return 0;
}
// entity:mesh() -> primitive name | model path, mesh index  (nil without a MeshRenderer)
int entity_mesh(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) {
        lua_pushnil(L);
        return 1;
    }
    if (m->mesh_asset.is_nil()) lua_pushlstring(L, m->primitive.data(), m->primitive.size());
    else push_asset(L, m->mesh_asset);
    lua_pushinteger(L, static_cast<int>(m->mesh_index));
    return 2;
}
// entity:set_color(vector, alpha?): MeshRenderer base color (linear tint) or Sprite tint.
int entity_set_color(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 c = check_vec3(L, 2);
    math::Color* target = nullptr;
    if (auto* m = s.world().get<scene::MeshRendererComponent>(e)) target = &m->base_color;
    else if (auto* sp = s.world().get<scene::SpriteComponent>(e)) target = &sp->tint;
    else luaL_error(L, "entity has no MeshRenderer or Sprite component");
    const float a = lua_isnoneornil(L, 3) ? target->a : static_cast<float>(luaL_checknumber(L, 3));
    *target = {c.x, c.y, c.z, a};
    return 0;
}
// entity:color() -> vector, alpha | nil
int entity_color(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Color* c = nullptr;
    if (const auto* m = s.world().get<scene::MeshRendererComponent>(e)) c = &m->base_color;
    else if (const auto* sp = s.world().get<scene::SpriteComponent>(e)) c = &sp->tint;
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvector(L, c->r, c->g, c->b);
    lua_pushnumber(L, c->a);
    return 2;
}
// entity:set_sprite(texture_path | nil): the Sprite's texture.
int entity_set_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* sp = s.world().get<scene::SpriteComponent>(e);
    if (!sp) luaL_error(L, "entity has no Sprite component");
    sp->texture_asset = lua_isnoneornil(L, 2) ? Uuid{} : resolve_asset(L, luaL_checkstring(L, 2), "set_sprite");
    return 0;
}
// entity:sprite() -> texture path | nil
int entity_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* sp = s.world().get<scene::SpriteComponent>(e);
    if (!sp) lua_pushnil(L);
    else push_asset(L, sp->texture_asset);
    return 1;
}
// entity:set_cast_shadows(bool)
int entity_set_cast_shadows(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    check_mesh_renderer(L, s, e).cast_shadows = lua_toboolean(L, 2) != 0;
    return 0;
}

gameplay::MaterialOverridesComponent& overrides_of(scene::Scene& s, Entity e) {
    if (auto* o = s.world().get<gameplay::MaterialOverridesComponent>(e)) return *o;
    return s.world().add<gameplay::MaterialOverridesComponent>(e);
}

// entity:set_uv_offset(vector): per-entity texture offset (x, y) on top of the material's,
// stored in MaterialOverrides (added when missing).
int entity_set_uv_offset(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 v = check_vec3(L, 2);
    overrides_of(s, e).uv_offset = {v.x, v.y};
    return 0;
}
// ---- tilemaps ------------------------------------------------------------------------------
tilemap::TilemapComponent& tilemap_of(lua_State* L, scene::Scene& s, Entity e) {
    auto* m = s.world().get<tilemap::TilemapComponent>(e);
    if (!m) luaL_error(L, "entity has no Tilemap component");
    return *m;
}
std::int32_t check_cell(lua_State* L, int idx) {
    const double v = luaL_checknumber(L, idx);
    if (!(v >= -tilemap::kMaxCoordinate && v <= tilemap::kMaxCoordinate))
        luaL_error(L, "tile coordinate %f is out of range", v);
    return static_cast<std::int32_t>(std::floor(v));
}
std::int32_t opt_tile(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return -1;
    const double v = luaL_checknumber(L, idx);
    if (!(v >= -1 && v < tilemap::kMaxTiles)) luaL_error(L, "tile index %f is out of range", v);
    return static_cast<std::int32_t>(v);
}
// entity:set_tile(x, y, tile | nil, flip_x?, flip_y?): paints (or erases with nil) one cell.
int entity_set_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = tilemap_of(L, s, e);
    const std::int32_t x = check_cell(L, 2), y = check_cell(L, 3);
    tilemap::set_tile(m, x, y, opt_tile(L, 4), lua_toboolean(L, 5) != 0, lua_toboolean(L, 6) != 0);
    return 0;
}
// entity:tile(x, y) -> tile index or nil, flip_x, flip_y
int entity_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const tilemap::Cell* c = m.cell(check_cell(L, 2), check_cell(L, 3));
    if (!c || c->tile < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, c->tile);
    lua_pushboolean(L, c->flip_x);
    lua_pushboolean(L, c->flip_y);
    return 3;
}
// entity:fill_tiles(x0, y0, x1, y1, tile | nil) -> cells changed (inclusive rectangle).
int entity_fill_tiles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = tilemap_of(L, s, e);
    const std::int32_t x0 = check_cell(L, 2), y0 = check_cell(L, 3), x1 = check_cell(L, 4), y1 = check_cell(L, 5);
    const std::uint64_t area = static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(x1) - x0) + 1) *
                               static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(y1) - y0) + 1);
    if (area > tilemap::kMaxFillCells) luaL_error(L, "fill_tiles: rectangle has more than %d cells", static_cast<int>(tilemap::kMaxFillCells));
    lua_pushnumber(L, static_cast<double>(tilemap::fill_tiles(m, x0, y0, x1, y1, opt_tile(L, 6))));
    return 1;
}
// entity:clear_tiles()
int entity_clear_tiles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    tilemap::clear_tiles(tilemap_of(L, s, e));
    return 0;
}
// entity:paint_terrain(x, y, name | nil): autotiled painting with the tile set's terrain rules.
int entity_paint_terrain(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    tilemap_of(L, s, e);
    const std::int32_t x = check_cell(L, 2), y = check_cell(L, 3);
    const auto* rt = s.world().get<tilemap::TilemapRuntime>(e);
    if (!rt || !rt->tileset) luaL_error(L, "paint_terrain: the tilemap's tile set is not loaded");
    std::int32_t terrain = -1;
    if (!lua_isnoneornil(L, 4)) {
        const char* name = luaL_checkstring(L, 4);
        terrain = rt->tileset->terrain_index(name);
        if (terrain < 0) luaL_error(L, "paint_terrain: the tile set has no terrain '%s'", name);
    }
    const auto ts = rt->tileset; // keep alive while painting
    tilemap::paint_terrain(tilemap_of(L, s, e), *ts, x, y, terrain);
    return 0;
}
// entity:world_to_tile(point) -> x, y of the cell containing a world position.
int entity_world_to_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const math::Vec3 local = math::inverse(s.world_matrix(e)).transform_point(check_vec3(L, 2));
    lua_pushinteger(L, static_cast<int>(std::floor(local.x / std::max(m.cell_size.x, 1e-6f))));
    lua_pushinteger(L, static_cast<int>(std::floor(local.y / std::max(m.cell_size.y, 1e-6f))));
    return 2;
}
// entity:tile_to_world(x, y) -> world position of the cell's centre.
int entity_tile_to_world(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const double x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
    const math::Vec3 local{(static_cast<float>(std::floor(x)) + 0.5f) * m.cell_size.x,
                           (static_cast<float>(std::floor(y)) + 0.5f) * m.cell_size.y, 0.0f};
    push_vec3(L, s.world_matrix(e).transform_point(local));
    return 1;
}
// entity:tile_bounds() -> min_x, min_y, max_x, max_y (inclusive) or nil when empty.
int entity_tile_bounds(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const tilemap::TileBounds b = tilemap::bounds(tilemap_of(L, s, e));
    if (b.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, b.min_x);
    lua_pushinteger(L, b.min_y);
    lua_pushinteger(L, b.max_x);
    lua_pushinteger(L, b.max_y);
    return 4;
}
// entity:set_uv_scroll(vector): UV units per second.
int entity_set_uv_scroll(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 v = check_vec3(L, 2);
    overrides_of(s, e).uv_scroll = {v.x, v.y};
    return 0;
}
// entity:set_emissive(vector | nil, strength?): overrides the material's emissive colour (nil
// restores the material's).
int entity_set_emissive(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (lua_isnoneornil(L, 2)) {
        if (auto* o = s.world().get<gameplay::MaterialOverridesComponent>(e)) o->override_emissive = false;
        return 0;
    }
    const math::Vec3 c = check_vec3(L, 2);
    auto& o = overrides_of(s, e);
    o.override_emissive = true;
    o.emissive = {c.x, c.y, c.z, 1};
    if (!lua_isnoneornil(L, 3)) o.emissive_strength = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 3)));
    return 0;
}

// entity:ui_state() -> {visible, hovered, pressed, focused, x, y, width, height, clicks}
// (pixels, from the last layout) or nil before the first layout.
int entity_ui_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* st = s.world().get<gameui::UiStateComponent>(e);
    if (!st) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 9);
    const auto set_b = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    const auto set_n = [&](const char* k, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    };
    set_b("visible", st->visible);
    set_b("hovered", st->hovered);
    set_b("pressed", st->pressed);
    set_b("focused", st->focused);
    set_n("x", st->rect.x);
    set_n("y", st->rect.y);
    set_n("width", st->rect.w);
    set_n("height", st->rect.h);
    set_n("clicks", static_cast<double>(st->clicks));
    return 1;
}
// ui.screen_size() -> width, height (pixels the UI is laid out for).
int ui_screen_size(lua_State* L) {
    const math::Vec2 sz = gameui::screen_size(scene_of(L));
    lua_pushnumber(L, sz.x);
    lua_pushnumber(L, sz.y);
    return 2;
}
int ui_pointer_over(lua_State* L) {
    lua_pushboolean(L, gameui::pointer_over_ui(scene_of(L)));
    return 1;
}
int ui_focus(lua_State* L) {
    auto& s = scene_of(L);
    gameui::focus(s, lua_isnoneornil(L, 1) ? Entity{} : check_live(L, 1, s));
    return 0;
}
int ui_focused(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = gameui::focused(s);
    push_entity(L, s.world().alive(e) ? e : Entity{});
    return 1;
}
int ui_click(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, gameui::click(s, check_live(L, 1, s)));
    return 1;
}

// ---- networking (net.* and entity network methods) ----
const char* role_name(net::Role r) {
    switch (r) {
    case net::Role::Server: return "server";
    case net::Role::Client: return "client";
    case net::Role::Offline: break;
    }
    return "offline";
}
int net_role(lua_State* L) {
    lua_pushstring(L, role_name(net::role_of(scene_of(L))));
    return 1;
}
int net_is_server(lua_State* L) {
    lua_pushboolean(L, net::role_of(scene_of(L)) == net::Role::Server);
    return 1;
}
int net_is_client(lua_State* L) {
    lua_pushboolean(L, net::role_of(scene_of(L)) == net::Role::Client);
    return 1;
}
// net.authority() -> true offline or on the server (where shared game state may change)
int net_authority(lua_State* L) {
    lua_pushboolean(L, net::role_of(scene_of(L)) != net::Role::Client);
    return 1;
}
int net_id(lua_State* L) {
    const auto* c = net::context_of(scene_of(L));
    lua_pushinteger(L, c ? c->local_id() : 0);
    return 1;
}
int net_connected(lua_State* L) {
    const auto* c = net::context_of(scene_of(L));
    lua_pushboolean(L, c && c->connected());
    return 1;
}
// net.local_player() -> the entity this machine's input drives (client or listen-server host), or nil
int net_local_player(lua_State* L) {
    auto& s = scene_of(L);
    const auto* c = net::context_of(s);
    const Entity e = c ? c->local_player() : Entity{};
    if (s.world().alive(e)) push_entity(L, e);
    else lua_pushnil(L);
    return 1;
}
// net.send(name, value?, to?) -> ok, error   (clients always send to the server)
int net_send(lua_State* L) {
    auto& s = scene_of(L);
    const char* name = luaL_checkstring(L, 1);
    const Value v = lua_isnoneornil(L, 2) ? Value{} : to_json(L, 2);
    const auto to = lua_isnoneornil(L, 3) ? net::kAllClients : static_cast<std::uint16_t>(luaL_checkinteger(L, 3));
    auto* c = net::context_of(s);
    if (!c) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "not in a network session");
        return 2;
    }
    const Status st = c->send(name, v, to);
    lua_pushboolean(L, st.ok());
    if (st.ok()) lua_pushnil(L);
    else lua_pushstring(L, st.error().message.c_str());
    return 2;
}
// net.clients() -> {{id, name, player, rtt_ms}...}
int net_clients(lua_State* L) {
    auto& s = scene_of(L);
    const auto* c = net::context_of(s);
    const std::vector<net::ClientInfo> list = c ? c->clients() : std::vector<net::ClientInfo>{};
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& ci = list[i];
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, ci.id);
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, ci.name.data(), ci.name.size());
        lua_setfield(L, -2, "name");
        if (s.world().alive(ci.player)) {
            push_entity(L, ci.player);
            lua_setfield(L, -2, "player");
        }
        lua_pushnumber(L, ci.rtt_ms);
        lua_setfield(L, -2, "rtt_ms");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}
// net.stats() -> diagnostics table
int net_stats(lua_State* L) {
    const auto* c = net::context_of(scene_of(L));
    const net::NetDiagnostics d = c ? c->diagnostics() : net::NetDiagnostics{};
    lua_createtable(L, 0, 12);
    auto num = [&](const char* k, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    };
    lua_pushstring(L, role_name(d.role));
    lua_setfield(L, -2, "role");
    lua_pushboolean(L, d.connected);
    lua_setfield(L, -2, "connected");
    num("tick", d.tick);
    num("clients", d.clients);
    num("entities", d.entities);
    num("rtt_ms", d.rtt_ms);
    num("send_kbps", d.send_kbps);
    num("receive_kbps", d.receive_kbps);
    num("snapshot_bytes", d.snapshot_bytes);
    num("delta_snapshots", static_cast<double>(d.delta_snapshots));
    num("prediction_error", d.prediction_error);
    num("corrections", static_cast<double>(d.corrections));
    lua_pushlstring(L, d.status.data(), d.status.size());
    lua_setfield(L, -2, "status");
    return 1;
}
// entity:set_net(key, value) -> ok, error   (authority only; replicated to every client)
int entity_set_net(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* key = luaL_checkstring(L, 2);
    const Value v = lua_isnoneornil(L, 3) ? Value{} : to_json(L, 3);
    const Status st = net::set_var(s, e, key, v);
    lua_pushboolean(L, st.ok());
    if (st.ok()) lua_pushnil(L);
    else lua_pushstring(L, st.error().message.c_str());
    return 2;
}
int entity_get_net(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    push_json(L, net::get_var(s, e, luaL_checkstring(L, 2)));
    return 1;
}
int entity_has_authority(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, net::has_authority(s, check_live(L, 1, s)));
    return 1;
}
int entity_is_local(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, net::is_local(s, check_live(L, 1, s)));
    return 1;
}
int entity_owner(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushinteger(L, net::owner_of(s, check_live(L, 1, s)));
    return 1;
}

// ---- terrain (terrain.*; read-only queries over resolved Terrain entities) ----
std::optional<Entity> opt_terrain_entity(lua_State* L, int idx, scene::Scene& s) {
    if (lua_isnoneornil(L, idx)) return std::nullopt;
    const Entity e = check_live(L, idx, s);
    if (!s.world().has<terrain::TerrainComponent>(e)) luaL_error(L, "entity has no Terrain component");
    return e;
}
// terrain.height(x, z, entity?) -> world height or nil outside every (or the given) terrain.
int terrain_height(lua_State* L) {
    auto& s = scene_of(L);
    const auto x = static_cast<float>(luaL_checknumber(L, 1)), z = static_cast<float>(luaL_checknumber(L, 2));
    const auto e = opt_terrain_entity(L, 3, s);
    const auto h = e ? terrain::height_at(s, *e, x, z) : terrain::height_at(s, x, z);
    if (h) lua_pushnumber(L, *h);
    else lua_pushnil(L);
    return 1;
}
// terrain.sample(x, z, entity?) -> {entity, height, normal, weights = {w0, w1, w2, w3}} or nil.
int terrain_sample(lua_State* L) {
    auto& s = scene_of(L);
    const auto x = static_cast<float>(luaL_checknumber(L, 1)), z = static_cast<float>(luaL_checknumber(L, 2));
    const auto e = opt_terrain_entity(L, 3, s);
    const auto r = e ? terrain::sample_at(s, *e, x, z) : terrain::sample_at(s, x, z);
    if (!r) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, r->entity);
    lua_setfield(L, -2, "entity");
    lua_pushnumber(L, r->height);
    lua_setfield(L, -2, "height");
    push_vec3(L, r->normal);
    lua_setfield(L, -2, "normal");
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushnumber(L, r->weights[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "weights");
    return 1;
}
// terrain.raycast(origin, direction, max_distance = 1000) -> {entity, point, normal, distance} or nil.
// Exact against the heightfields; needs no physics world.
int terrain_raycast(lua_State* L) {
    auto& s = scene_of(L);
    const math::Vec3 o = check_vec3(L, 1), d = check_vec3(L, 2);
    const auto max = static_cast<float>(luaL_optnumber(L, 3, 1000.0));
    if (!(max >= 0.0f) || !std::isfinite(max)) luaL_error(L, "terrain.raycast: max_distance must be finite and >= 0");
    const auto hit = terrain::raycast(s, o, d, std::min(max, 1e6f));
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, hit->entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->distance);
    lua_setfield(L, -2, "distance");
    return 1;
}

// ---- navigation (nav.* and entity navigation methods) ----
navigation::NavigationSystem* nav_of(lua_State* L) {
    auto& s = scene_of(L);
    navigation::NavigationSystem* n = navigation::navigation_of(s);
    if (n && !n->valid()) (void)n->ensure_built(s);
    return n;
}
int nav_available(lua_State* L) {
    const auto* n = nav_of(L);
    lua_pushboolean(L, n && n->valid());
    return 1;
}
// nav.find_path(from, to) -> {vector...}, partial  (nil when either end is off the navmesh)
int nav_find_path(lua_State* L) {
    const math::Vec3 from = check_vec3(L, 1), to = check_vec3(L, 2);
    const auto* n = nav_of(L);
    std::vector<math::Vec3> path;
    bool partial = false;
    if (!n || !n->find_path(from, to, path, &partial)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(path.size()), 0);
    for (std::size_t i = 0; i < path.size(); ++i) {
        push_vec3(L, path[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_pushboolean(L, partial);
    return 2;
}
// nav.nearest(point, extent?) -> vector or nil
int nav_nearest(lua_State* L) {
    const math::Vec3 p = check_vec3(L, 1);
    const math::Vec3 extent = lua_isnoneornil(L, 2) ? math::Vec3{2, 4, 2} : check_vec3(L, 2);
    const auto* n = nav_of(L);
    const auto r = n ? n->nearest(p, extent) : std::nullopt;
    if (r) push_vec3(L, *r);
    else lua_pushnil(L);
    return 1;
}
// nav.raycast(from, to) -> hit, point, normal (straight walk along the surface)
int nav_raycast(lua_State* L) {
    const math::Vec3 from = check_vec3(L, 1), to = check_vec3(L, 2);
    const auto* n = nav_of(L);
    if (!n) {
        lua_pushboolean(L, true);
        push_vec3(L, from);
        push_vec3(L, {});
        return 3;
    }
    const navigation::RaycastResult r = n->raycast(from, to);
    lua_pushboolean(L, r.hit);
    push_vec3(L, r.point);
    push_vec3(L, r.normal);
    return 3;
}
// nav.random_point(seed?) -> vector or nil
int nav_random_point(lua_State* L) {
    const auto* n = nav_of(L);
    static std::uint32_t counter = 0x9e3779b9u; // successive calls without a seed give different points
    counter = counter * 1664525u + 1013904223u;
    const auto seed = lua_isnoneornil(L, 1) ? counter : static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    const auto r = n ? n->random_point(seed) : std::nullopt;
    if (r) push_vec3(L, *r);
    else lua_pushnil(L);
    return 1;
}
// nav.rebuild() -> bool
int nav_rebuild(lua_State* L) {
    auto& s = scene_of(L);
    auto* n = navigation::navigation_of(s);
    lua_pushboolean(L, n && n->build(s).ok() && n->valid());
    return 1;
}
// entity:set_destination(point) -> bool
int entity_set_destination(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 p = check_vec3(L, 2);
    auto* n = navigation::navigation_of(s);
    lua_pushboolean(L, n && n->set_destination(s, e, p));
    return 1;
}
int entity_stop(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* n = navigation::navigation_of(s)) n->stop(s, e);
    return 0;
}
// entity:nav_state() -> {active, has_destination, reached, partial, destination, velocity, remaining_distance}
// ---- AI: blackboards, perception, noises -----------------------------------------------------

void push_ai_value(lua_State* L, const ai::Value& v) {
    if (const bool* b = std::get_if<bool>(&v)) lua_pushboolean(L, *b);
    else if (const double* d = std::get_if<double>(&v)) lua_pushnumber(L, *d);
    else if (const math::Vec3* p = std::get_if<math::Vec3>(&v)) push_vec3(L, *p);
    else if (const Entity* e = std::get_if<Entity>(&v)) push_entity(L, *e);
    else if (const std::string* str = std::get_if<std::string>(&v)) lua_pushlstring(L, str->data(), str->size());
    else lua_pushnil(L);
}

ai::Value check_ai_value(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
    case LUA_TNONE: return {};
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
    case LUA_TNUMBER: return lua_tonumber(L, idx);
    case LUA_TVECTOR: return check_vec3(L, idx);
    case LUA_TSTRING: {
        std::size_t len = 0;
        const char* str = lua_tolstring(L, idx, &len);
        return std::string(str, len);
    }
    default:
        if (lua_touserdatatagged(L, idx, kEntityTag)) return opt_entity(L, idx);
        luaL_error(L, "blackboard values must be nil, boolean, number, string, vector or Entity"); // does not return
    }
}

ai::Blackboard& check_blackboard(lua_State* L, scene::Scene& s, Entity e) {
    auto* a = ai::ai_of(s);
    if (!a) luaL_error(L, "AI is not running in this scene");
    ai::Blackboard* bb = a->blackboard(s, e);
    if (!bb) luaL_error(L, "entity was destroyed");
    return *bb;
}

// entity:bb_get(key) -> value (nil when unset or when AI is not running)
int entity_bb_get(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* key = luaL_checkstring(L, 2);
    auto* a = ai::ai_of(s);
    const ai::Blackboard* bb = a ? a->blackboard(s, e) : nullptr;
    const ai::Value* v = bb ? bb->find(key) : nullptr;
    if (v) push_ai_value(L, *v);
    else lua_pushnil(L);
    return 1;
}

// entity:bb_set(key, value): nil erases the key.
int entity_bb_set(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const char* key = luaL_checkstring(L, 2);
    ai::Value v = check_ai_value(L, 3);
    ai::Blackboard& bb = check_blackboard(L, s, e);
    if (std::holds_alternative<std::monostate>(v)) bb.erase(key);
    else bb.set(key, std::move(v));
    return 0;
}

// entity:blackboard() -> {key = value} snapshot ({} when AI is not running)
int entity_blackboard(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* a = ai::ai_of(s);
    const ai::Blackboard* bb = a ? a->blackboard(s, e) : nullptr;
    lua_newtable(L);
    if (!bb) return 1;
    for (const auto& [k, v] : bb->entries()) {
        push_ai_value(L, v);
        lua_setfield(L, -2, k.c_str());
    }
    return 1;
}

// entity:perception() -> {target, visible, last_position, since_seen, seen = {...}, heard,
// noise_position, since_heard} or nil when the entity has no PerceptionComponent state yet.
int entity_perception(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* a = ai::ai_of(s);
    const ai::PerceptionState* p = a ? a->perception(s, e) : nullptr;
    if (!p) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 8);
    push_entity(L, s.world().alive(p->target) ? p->target : Entity{});
    lua_setfield(L, -2, "target");
    lua_pushboolean(L, p->visible);
    lua_setfield(L, -2, "visible");
    push_vec3(L, p->last_position);
    lua_setfield(L, -2, "last_position");
    lua_pushnumber(L, p->since_seen);
    lua_setfield(L, -2, "since_seen");
    lua_createtable(L, static_cast<int>(p->seen.size()), 0);
    int i = 1;
    for (Entity t : p->seen) {
        if (!s.world().alive(t)) continue;
        push_entity(L, t);
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "seen");
    lua_pushboolean(L, p->heard);
    lua_setfield(L, -2, "heard");
    push_vec3(L, p->noise_position);
    lua_setfield(L, -2, "noise_position");
    lua_pushnumber(L, p->since_heard);
    lua_setfield(L, -2, "since_heard");
    return 1;
}

// ai.available() -> whether behaviour trees and perception run in this scene
int ai_available(lua_State* L) {
    lua_pushboolean(L, ai::ai_of(scene_of(L)) != nullptr);
    return 1;
}

// ai.report_noise(position, loudness = 0, source = nil)
int ai_report_noise(lua_State* L) {
    auto& s = scene_of(L);
    const math::Vec3 p = check_vec3(L, 1);
    const auto loud = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    const Entity src = opt_entity(L, 3);
    if (auto* a = ai::ai_of(s)) a->report_noise(p, loud, src);
    return 0;
}

// ---- sequencer ----------------------------------------------------------------------------------

// entity:play_sequence(path = nil) -> bool: (re)starts the entity's SequencePlayer (adding one
// when missing); `path` replaces its sequence asset.
int entity_play_sequence(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::size_t n = 0;
    const char* path = luaL_optlstring(L, 2, "", &n);
    auto* q = sequencer::sequencer_of(s);
    lua_pushboolean(L, q && q->play(s, e, std::string_view(path, n)));
    return 1;
}

// entity:stop_sequence(): stops and rewinds (camera cuts are undone).
int entity_stop_sequence(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* q = sequencer::sequencer_of(s)) q->stop(s, e);
    return 0;
}

// entity:pause_sequence(paused = true)
int entity_pause_sequence(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool paused = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    if (auto* q = sequencer::sequencer_of(s)) q->pause(s, e, paused);
    return 0;
}

// entity:seek_sequence(time) -> bool: moves the playhead without firing events or audio.
int entity_seek_sequence(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto t = static_cast<float>(luaL_checknumber(L, 2));
    auto* q = sequencer::sequencer_of(s);
    lua_pushboolean(L, q && q->seek(s, e, t));
    return 1;
}

// entity:sequence_state() -> {valid, sequence, error, time, duration, playing, paused, finished,
// loops, camera} or nil without a sequencer.
int entity_sequence_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* q = sequencer::sequencer_of(s);
    if (!q) {
        lua_pushnil(L);
        return 1;
    }
    const sequencer::PlaybackState st = q->state(s, e);
    lua_createtable(L, 0, 10);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushlstring(L, st.sequence.data(), st.sequence.size());
    lua_setfield(L, -2, "sequence");
    lua_pushlstring(L, st.error.data(), st.error.size());
    lua_setfield(L, -2, "error");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, st.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, st.paused);
    lua_setfield(L, -2, "paused");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    lua_pushinteger(L, static_cast<int>(st.loops));
    lua_setfield(L, -2, "loops");
    lua_pushlstring(L, st.camera.data(), st.camera.size());
    lua_setfield(L, -2, "camera");
    return 1;
}

// sequencer.available() -> whether timelines play in this scene
int sequencer_available(lua_State* L) {
    lua_pushboolean(L, sequencer::sequencer_of(scene_of(L)) != nullptr);
    return 1;
}

int entity_nav_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* n = navigation::navigation_of(s);
    const navigation::AgentState st = n ? n->agent_state(e) : navigation::AgentState{};
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, st.active);
    lua_setfield(L, -2, "active");
    lua_pushboolean(L, st.has_destination);
    lua_setfield(L, -2, "has_destination");
    lua_pushboolean(L, st.reached);
    lua_setfield(L, -2, "reached");
    lua_pushboolean(L, st.partial);
    lua_setfield(L, -2, "partial");
    push_vec3(L, st.destination);
    lua_setfield(L, -2, "destination");
    push_vec3(L, st.velocity);
    lua_setfield(L, -2, "velocity");
    lua_pushnumber(L, st.remaining_distance);
    lua_setfield(L, -2, "remaining_distance");
    return 1;
}

// entity:character_state() -> {valid, grounded, on_steep_slope, ground_normal, velocity, ground}
int entity_character_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const physics::CharacterState st = w ? w->character_state(e) : physics::CharacterState{};
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, st.grounded);
    lua_setfield(L, -2, "grounded");
    lua_pushboolean(L, st.on_steep_slope);
    lua_setfield(L, -2, "on_steep_slope");
    push_vec3(L, st.ground_normal);
    lua_setfield(L, -2, "ground_normal");
    push_vec3(L, st.velocity);
    lua_setfield(L, -2, "velocity");
    if (!st.ground.is_null()) {
        push_entity(L, st.ground);
        lua_setfield(L, -2, "ground");
    }
    return 1;
}

// entity:play_animation(clip, fade?, restart?) -> bool. Cross-fades the entity's Animator to
// `clip` over `fade` seconds (default: the Animator's crossfade). Before the animation system
// has run on this scene the clip is only recorded on the component (applied next update).
int entity_play_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string clip = luaL_checkstring(L, 2);
    const float fade = static_cast<float>(luaL_optnumber(L, 3, -1.0));
    const bool restart = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4) != 0;
    if (auto* sys = animation::system_of(s)) {
        lua_pushboolean(L, sys->play(s, e, clip, fade, restart));
        return 1;
    }
    auto* a = s.world().get<animation::AnimatorComponent>(e);
    if (a) {
        a->clip = clip;
        a->playing = true;
    }
    lua_pushboolean(L, a != nullptr);
    return 1;
}

// entity:animation() -> {valid, clip, time, duration, playing, looping, finished, blend}
int entity_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    animation::AnimatorStatus st;
    if (auto* sys = animation::system_of(s)) st = sys->status(s, e);
    else if (const auto* a = s.world().get<animation::AnimatorComponent>(e)) {
        st.clip = a->clip;
        st.time = a->time;
        st.playing = a->playing;
        st.looping = a->loop;
    }
    lua_createtable(L, 0, 8);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushstring(L, st.clip.c_str());
    lua_setfield(L, -2, "clip");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, st.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, st.looping);
    lua_setfield(L, -2, "looping");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    lua_pushnumber(L, st.blend);
    lua_setfield(L, -2, "blend");
    return 1;
}

// entity:animation_clips() -> {names...} (empty until the model resolves)
int entity_animation_clips(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::vector<std::string> names;
    if (auto* sys = animation::system_of(s)) names = sys->clips(s, e);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// entity:play_sprite(clip, restart?) -> bool. Plays a clip of the entity's SpriteAnimator sheet.
int entity_play_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string clip = luaL_checkstring(L, 2);
    const bool restart = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    if (auto* sys = animation::system_of(s)) {
        lua_pushboolean(L, sys->play_sprite(s, e, clip, restart));
        return 1;
    }
    auto* a = s.world().get<animation::SpriteAnimatorComponent>(e);
    if (a) {
        if (a->clip != clip || restart) a->time = 0;
        a->clip = clip;
        a->playing = true;
    }
    lua_pushboolean(L, a != nullptr);
    return 1;
}

// entity:sprite_animation() -> {valid, clip, frame, clip_frame, time, duration, playing, looping, finished}
int entity_sprite_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    animation::SpriteAnimatorStatus st;
    if (auto* sys = animation::system_of(s)) st = sys->sprite_status(s, e);
    else if (const auto* a = s.world().get<animation::SpriteAnimatorComponent>(e)) {
        st.clip = a->clip;
        st.time = a->time;
        st.playing = a->playing;
    }
    lua_createtable(L, 0, 9);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushstring(L, st.clip.c_str());
    lua_setfield(L, -2, "clip");
    lua_pushnumber(L, st.frame);
    lua_setfield(L, -2, "frame");
    lua_pushnumber(L, st.clip_frame);
    lua_setfield(L, -2, "clip_frame");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, st.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, st.looping);
    lua_setfield(L, -2, "looping");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    return 1;
}

// entity:sprite_clips() -> {names...} (empty until the sheet resolves)
int entity_sprite_clips(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::vector<std::string> names;
    if (auto* sys = animation::system_of(s)) names = sys->sprite_clips(s, e);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// entity:emit_particles(count) -> bool. Queues `count` particles on the entity's
// ParticleEmitter for the next update (works while the emitter is not emitting: bursts on demand).
int entity_emit_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const double n = luaL_checknumber(L, 2);
    const auto count = static_cast<std::uint32_t>(std::clamp(n, 0.0, 65536.0));
    lua_pushboolean(L, particles::emit(s, e, count));
    return 1;
}

// entity:set_emitting(on) -> bool. Starts/stops continuous emission (live particles finish).
int entity_set_emitting(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* c = s.world().get<particles::ParticleEmitterComponent>(e);
    if (c) c->emitting = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, c != nullptr);
    return 1;
}

// entity:reset_particles() -> bool. Kills live particles and restarts the emitter cycle.
int entity_reset_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    lua_pushboolean(L, particles::reset(s, e));
    return 1;
}

// entity:particles() -> {valid, emitting, alive, emitted, time, finished}
int entity_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const particles::EmitterStatus st = particles::status(s, e);
    const auto* c = s.world().get<particles::ParticleEmitterComponent>(e);
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, c && c->emitting);
    lua_setfield(L, -2, "emitting");
    lua_pushnumber(L, st.alive);
    lua_setfield(L, -2, "alive");
    lua_pushnumber(L, static_cast<double>(st.emitted));
    lua_setfield(L, -2, "emitted");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    return 1;
}

// entity:joint_state() -> {valid, active, broken, angle, position, force}
int entity_joint_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const physics::JointState st = w ? w->joint_state(e) : physics::JointState{};
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, st.active);
    lua_setfield(L, -2, "active");
    lua_pushboolean(L, st.broken);
    lua_setfield(L, -2, "broken");
    lua_pushnumber(L, st.angle);
    lua_setfield(L, -2, "angle");
    lua_pushnumber(L, st.position);
    lua_setfield(L, -2, "position");
    lua_pushnumber(L, st.force);
    lua_setfield(L, -2, "force");
    return 1;
}

// entity:set_motor(enabled, speed): hinge/revolute/wheel (deg/s) or slider/prismatic (m/s) motor, 3D or 2D.
int entity_set_motor(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool enabled = lua_toboolean(L, 2) != 0;
    const float speed = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    if (auto* w = physics::world_of(s)) {
        w->set_joint_motor(s, e, enabled, speed);
    } else if (auto* j = s.world().get<physics::JointComponent>(e)) {
        j->motor = enabled;
        j->motor_speed = speed;
    } else if (auto* j2 = s.world().get<physics::Joint2DComponent>(e)) {
        j2->motor = enabled;
        j2->motor_speed = speed;
    }
    return 0;
}

// ---- audio ----
// Audio calls are no-ops (returning false/0) when no audio engine runs for the scene, so the same
// scripts work in headless simulation and on servers.

audio::AudioEngine* audio_of(lua_State* L) { return audio::engine_of(scene_of(L)); }

audio::Bus check_bus(lua_State* L, int idx, audio::Bus fallback) {
    if (lua_isnoneornil(L, idx)) return fallback;
    const char* name = luaL_checkstring(L, idx);
    const auto b = audio::parse_bus(name);
    if (!b) luaL_error(L, "unknown audio bus '%s' (Master, Music, Sfx, Ui, Ambient, Voice)", name);
    return *b;
}

// audio.play(clip, { volume=, pitch=, bus=, position= }) -> boolean
int audio_play(lua_State* L) {
    const char* clip = luaL_checkstring(L, 1);
    audio::OneShot o;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "volume");
        if (!lua_isnil(L, -1)) o.volume = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 2, "pitch");
        if (!lua_isnil(L, -1)) o.pitch = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 2, "bus");
        o.bus = check_bus(L, -1, audio::Bus::Sfx);
        lua_pop(L, 1);
        lua_getfield(L, 2, "position");
        if (!lua_isnil(L, -1)) o.position = check_vec3(L, -1);
        lua_pop(L, 1);
    } else if (!lua_isnoneornil(L, 2)) {
        luaL_typeerror(L, 2, "table");
    }
    auto* a = audio_of(L);
    lua_pushboolean(L, a && a->play_one_shot(clip, o).ok());
    return 1;
}
int audio_set_volume(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    const float v = static_cast<float>(luaL_checknumber(L, 2));
    if (auto* a = audio_of(L)) a->set_bus_volume(b, v);
    return 0;
}
int audio_volume(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    auto* a = audio_of(L);
    lua_pushnumber(L, a ? a->bus_volume(b) : 0.0);
    return 1;
}
int audio_set_muted(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    const bool m = lua_toboolean(L, 2) != 0;
    if (auto* a = audio_of(L)) a->set_bus_muted(b, m);
    return 0;
}
int audio_muted(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    auto* a = audio_of(L);
    lua_pushboolean(L, a && a->bus_muted(b));
    return 1;
}
int audio_available(lua_State* L) {
    lua_pushboolean(L, audio_of(L) != nullptr);
    return 1;
}
int entity_play_audio(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* a = audio::engine_of(s);
    lua_pushboolean(L, a && a->play(s, e).ok());
    return 1;
}
int entity_stop_audio(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* a = audio::engine_of(s)) a->stop(e);
    return 0;
}
int entity_pause_audio(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool paused = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    if (auto* a = audio::engine_of(s)) a->set_paused(e, paused);
    return 0;
}
int entity_audio_playing(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* a = audio::engine_of(s);
    lua_pushboolean(L, a && a->is_playing(e));
    return 1;
}

// ---- save games ----------------------------------------------------------------------------------

int save_fail(lua_State* L, const std::string& message) {
    lua_pushnil(L);
    lua_pushstring(L, message.c_str());
    return 2;
}

constexpr const char* kNoSaves = "saves are not available (no save directory)";

int save_available(lua_State* L) {
    lua_pushboolean(L, host_of(L).store() != nullptr);
    return 1;
}

int save_version(lua_State* L) {
    lua_pushinteger(L, static_cast<int>(host_of(L).options.save_version));
    return 1;
}

// save.write(slot, data, label?) -> true | nil, error
int save_write(lua_State* L) {
    auto& h = host_of(L);
    const std::string slot = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    const std::string label = luaL_optstring(L, 3, "");
    auto* store = h.store();
    if (!store) return save_fail(L, kNoSaves);
    const Value data = to_json(L, 2, 0, true);
    if (auto st = store->write(slot, data, label); !st) return save_fail(L, st.error().message);
    lua_pushboolean(L, 1);
    return 1;
}

// save.read(slot) -> data, info | nil, error
int save_read(lua_State* L) {
    auto& h = host_of(L);
    const std::string slot = luaL_checkstring(L, 1);
    auto* store = h.store();
    if (!store) return save_fail(L, kNoSaves);
    lua_State* outer = h.active;
    h.active = L;
    auto file = store->read(slot);
    h.active = outer;
    if (!file) return save_fail(L, file.error().message);
    push_json(L, file->data, 0, true);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, static_cast<int>(file->data_version));
    lua_setfield(L, -2, "version");
    lua_pushnumber(L, static_cast<double>(file->saved_at));
    lua_setfield(L, -2, "saved_at");
    lua_pushstring(L, file->label.c_str());
    lua_setfield(L, -2, "label");
    lua_pushboolean(L, file->recovered);
    lua_setfield(L, -2, "recovered");
    lua_pushboolean(L, file->migrated);
    lua_setfield(L, -2, "migrated");
    return 2;
}

int save_exists(lua_State* L) {
    auto* store = host_of(L).store();
    lua_pushboolean(L, store && store->exists(luaL_checkstring(L, 1)));
    return 1;
}

int save_delete(lua_State* L) {
    const std::string slot = luaL_checkstring(L, 1);
    auto* store = host_of(L).store();
    if (!store) return save_fail(L, kNoSaves);
    if (auto st = store->remove(slot); !st) return save_fail(L, st.error().message);
    lua_pushboolean(L, 1);
    return 1;
}

// save.list() -> { {slot, label, saved_at, version, damaged}, ... } newest first
int save_list(lua_State* L) {
    auto* store = host_of(L).store();
    const auto slots = store ? store->list() : std::vector<gameplay::SlotInfo>{};
    lua_createtable(L, static_cast<int>(slots.size()), 0);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& s = slots[i];
        lua_createtable(L, 0, 5);
        lua_pushstring(L, s.slot.c_str());
        lua_setfield(L, -2, "slot");
        lua_pushstring(L, s.label.c_str());
        lua_setfield(L, -2, "label");
        lua_pushnumber(L, static_cast<double>(s.saved_at));
        lua_setfield(L, -2, "saved_at");
        lua_pushinteger(L, static_cast<int>(s.data_version));
        lua_setfield(L, -2, "version");
        lua_pushboolean(L, s.damaged);
        lua_setfield(L, -2, "damaged");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// save.migrate(from, fn): fn(data) returns the data upgraded from version `from` to `from + 1`.
int save_migrate(lua_State* L) {
    auto& h = host_of(L);
    const int from = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (from < 1) luaL_error(L, "save.migrate: versions start at 1");
    auto* store = h.store();
    if (!store) return save_fail(L, kNoSaves);
    const auto key = static_cast<std::uint32_t>(from);
    if (auto it = h.migration_refs.find(key); it != h.migration_refs.end()) lua_unref(L, it->second);
    h.migration_refs[key] = lua_ref(L, 2);
    ScriptHost::Impl* hp = &h;
    store->add_migration(key, [hp, key](Value data) -> Result<Value> {
        lua_State* T = hp->active ? hp->active : hp->L;
        const int top = lua_gettop(T);
        lua_getref(T, hp->migration_refs[key]);
        push_json(T, data, 0, true);
        if (!hp->pcall_on(T, 1, 1, std::format("save.migrate({})", key))) {
            lua_settop(T, top);
            return make_error(ErrorCode::ScriptError, "the migration function raised an error");
        }
        Value out = to_json(T, -1, 0, true);
        lua_settop(T, top);
        if (out.is_null()) return make_error(ErrorCode::ScriptError, "the migration function returned nil");
        return out;
    });
    lua_pushboolean(L, 1);
    return 1;
}

bool multiplayer(scene::Scene& s) { return net::role_of(s) != net::Role::Offline; }

// save.checkpoint(slot, label?) -> true | nil, error. Also used by ScriptHost::checkpoint.
// A checkpoint's scene copy stores each simulated rigid body's live velocity as its initial
// velocity (RigidBody / RigidBody2D `linear_velocity`, `angular_velocity`), so restored bodies
// continue moving instead of restarting at rest. Characters keep only their position.
void stamp_body_velocities(scene::Scene& s, Value& doc) {
    physics::PhysicsWorld* w = physics::world_of(s);
    if (!w || !doc.is_object()) return;
    for (auto& [key, entities] : doc.as_object()) {
        if (key != "entities" || !entities.is_array()) continue;
        for (Value& ent : entities.as_array()) {
            if (!ent.is_object()) continue;
            Uuid id;
            if (!Uuid::parse(ent["uuid"].as_string_or(""), id)) continue;
            const Entity e = s.find(id);
            if (e.is_null() || !w->has_body(e)) continue;
            for (auto& [ckey, comps] : ent.as_object()) {
                if (ckey != "components" || !comps.is_object()) continue;
                for (auto& [name, comp] : comps.as_object()) {
                    if (!comp.is_object()) continue;
                    const math::Vec3 v = w->linear_velocity(e), a = w->angular_velocity(e);
                    if (name == "RigidBody") {
                        comp.set("linear_velocity", Value(json::Array{Value(v.x), Value(v.y), Value(v.z)}));
                        comp.set("angular_velocity", Value(json::Array{Value(a.x), Value(a.y), Value(a.z)}));
                    } else if (name == "RigidBody2D") {
                        comp.set("linear_velocity", Value(json::Array{Value(v.x), Value(v.y)}));
                        comp.set("angular_velocity", Value(a.z));
                    }
                }
            }
        }
    }
}

int save_checkpoint(lua_State* L) {
    auto& h = host_of(L);
    auto& s = scene_of(L);
    const std::string slot = luaL_checkstring(L, 1);
    const std::string label = luaL_optstring(L, 2, "");
    if (multiplayer(s)) return save_fail(L, "checkpoints are not available in multiplayer sessions");
    auto* store = h.store();
    if (!store) return save_fail(L, kNoSaves);
    std::vector<std::uint64_t> keys;
    for (const auto& [k, in] : h.instances)
        if (in.started && !in.faulted) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    Value scripts = Value::object();
    for (const std::uint64_t k : keys) {
        auto it = h.instances.find(k);
        if (it == h.instances.end()) continue;
        const Entity e{static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32)};
        if (!s.world().alive(e)) continue;
        lua_getref(L, it->second.ref);
        lua_getfield(L, -1, "save_state");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            continue;
        }
        lua_pushvalue(L, -2);
        const std::string path = it->second.path;
        if (h.pcall_on(L, 1, 1, std::format("{}:save_state", path))) {
            Value state = to_json(L, -1, 0, true);
            lua_pop(L, 1);
            if (!state.is_null()) scripts.set(s.uuid(e).to_string(), std::move(state));
        }
        lua_pop(L, 1); // instance
    }
    Value data = Value::object();
    data.set("victus_checkpoint", 1);
    Value scene_doc = s.save();
    stamp_body_velocities(s, scene_doc);
    data.set("scene", std::move(scene_doc));
    data.set("scripts", std::move(scripts));
    if (!h.current_scene.empty()) data.set("scene_path", h.current_scene);
    if (!h.scene_params.is_null()) data.set("scene_params", h.scene_params);
    if (auto st = store->write(slot, data, label); !st) return save_fail(L, st.error().message);
    lua_pushboolean(L, 1);
    return 1;
}

Result<Value> read_checkpoint(ScriptHost::Impl& h, scene::Scene& s, std::string_view slot) {
    if (multiplayer(s)) return make_error(ErrorCode::Unsupported, "checkpoints are not available in multiplayer sessions");
    auto* store = h.store();
    if (!store) return make_error(ErrorCode::Unsupported, kNoSaves);
    auto file = store->read(slot, false);
    if (!file) return std::move(file).error();
    if (file->data["victus_checkpoint"].as_int(0) != 1 || !file->data["scene"].is_object())
        return make_error(ErrorCode::InvalidArgument, std::format("save slot '{}' is not a checkpoint", slot));
    // Validate the scene before the live one is replaced.
    scene::Scene probe;
    if (auto st = probe.load(file->data["scene"]); !st)
        return make_error(st.error().code, std::format("checkpoint '{}': {}", slot, st.error().message));
    return std::move(file->data);
}

// save.restore(slot) -> true | nil, error. The scene is replaced at the end of this update.
int save_restore(lua_State* L) {
    auto& h = host_of(L);
    auto& s = scene_of(L);
    auto data = read_checkpoint(h, s, luaL_checkstring(L, 1));
    if (!data) return save_fail(L, data.error().message);
    h.pending_scene.reset(); // the newest request wins
    h.pending_restore = std::move(*data);
    lua_pushboolean(L, 1);
    return 1;
}

int scene_find_id(lua_State* L) {
    auto& s = scene_of(L);
    Uuid id;
    if (!Uuid::parse(luaL_checkstring(L, 1), id)) {
        lua_pushnil(L);
        return 1;
    }
    push_entity(L, s.find(id));
    return 1;
}

// Reads and validates the scene at `path` before anything is replaced.
Result<Value> read_scene(ScriptHost::Impl& h, std::string_view path) {
    if (path.empty()) return make_error(ErrorCode::InvalidArgument, "scene path is empty");
    Result<Value> doc = make_error(ErrorCode::Unknown, "unreachable");
    if (h.options.scene_loader) {
        doc = h.options.scene_loader(path);
    } else {
        const fs::Path root = !h.options.project_root.empty() ? h.options.project_root : h.options.assets_root.parent_path();
        auto file = fs::resolve_under(root, path);
        if (!file) return std::move(file).error();
        auto text = fs::read_text(*file);
        if (!text) return make_error(ErrorCode::NotFound, std::format("no scene at '{}'", path));
        auto parsed = json::parse(*text);
        if (!parsed) return make_error(ErrorCode::ParseError, std::format("{}: {}", path, parsed.error().message));
        doc = std::move(*parsed);
    }
    if (!doc) return std::move(doc).error();
    scene::Scene probe;
    if (auto st = probe.load(*doc); !st) return make_error(st.error().code, std::format("{}: {}", path, st.error().message));
    return doc;
}

Status apply_scene(ScriptHost::Impl& h, scene::Scene& s, ScriptHost::Impl::SceneRequest req) {
    for (auto& [k, in] : h.instances) h.drop(in);
    h.instances.clear();
    h.pending_states.clear();
    h.pending_restore.reset();
    if (auto st = s.load(req.doc); !st) return st;
    s.update_transforms();
    h.current_scene = std::move(req.path);
    h.scene_params = std::move(req.params);
    ++h.counters.scene_loads;
    VICTUS_LOG_INFO("script", "scene changed to {}", h.current_scene);
    if (h.options.on_scene_loaded) h.options.on_scene_loaded(s, h.current_scene);
    return {};
}

// scene.load(path, params?) -> true | nil, error. The scene is replaced at the end of this update.
int scene_load_impl(lua_State* L, std::string path, int params_idx) {
    auto& h = host_of(L);
    Value params;
    if (!lua_isnoneornil(L, params_idx)) {
        luaL_checktype(L, params_idx, LUA_TTABLE);
        params = to_json(L, params_idx, 0, true);
    }
    auto doc = read_scene(h, path);
    if (!doc) {
        lua_pushnil(L);
        lua_pushstring(L, doc.error().message.c_str());
        return 2;
    }
    h.pending_restore.reset(); // the newest request wins
    h.pending_scene = ScriptHost::Impl::SceneRequest{std::move(path), std::move(*doc), std::move(params)};
    lua_pushboolean(L, 1);
    return 1;
}

int scene_load(lua_State* L) { return scene_load_impl(L, luaL_checkstring(L, 1), 2); }

int scene_reload(lua_State* L) {
    auto& h = host_of(L);
    if (h.current_scene.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, "the running scene has no path");
        return 2;
    }
    return scene_load_impl(L, h.current_scene, 1);
}

int scene_current(lua_State* L) {
    auto& h = host_of(L);
    if (h.current_scene.empty()) lua_pushnil(L);
    else lua_pushstring(L, h.current_scene.c_str());
    return 1;
}

int scene_params(lua_State* L) {
    auto& h = host_of(L);
    if (h.scene_params.is_null()) lua_pushnil(L);
    else push_json(L, h.scene_params, 0, true);
    return 1;
}

Status apply_checkpoint(ScriptHost::Impl& h, scene::Scene& s, const Value& data) {
    for (auto& [k, in] : h.instances) h.drop(in);
    h.instances.clear();
    if (auto st = s.load(data["scene"]); !st) return st;
    s.update_transforms();
    if (const Value* p = data.find("scene_path"); p && p->is_string()) {
        h.current_scene = p->as_string();
        const Value* params = data.find("scene_params");
        h.scene_params = params ? *params : Value{};
    }
    h.pending_states.clear();
    if (const Value* scripts = data.find("scripts"); scripts && scripts->is_object())
        for (const auto& [id, state] : scripts->as_object()) h.pending_states.emplace(id, state);
    return {};
}

// ---- bridges: helper processes speaking JSON lines ------------------------------------------------

constexpr int kBridgeTag = 2;

struct BridgeRef {
    int id;
};

ScriptHost::Impl::Bridge& check_bridge(lua_State* L, int idx) {
    auto* ref = static_cast<BridgeRef*>(lua_touserdatatagged(L, idx, kBridgeTag));
    if (!ref) luaL_typeerror(L, idx, "Bridge");
    auto& h = host_of(L);
    const auto it = h.bridges.find(ref->id);
    if (it == h.bridges.end() || !it->second.child) luaL_error(L, "bridge is closed");
    return it->second;
}

void push_bridge(lua_State* L, int id) {
    auto* ref = static_cast<BridgeRef*>(lua_newuserdatatagged(L, sizeof(BridgeRef), kBridgeTag));
    ref->id = id;
    lua_getuserdatametatable(L, kBridgeTag);
    lua_setmetatable(L, -2);
}

// Waiting for the helper does not count against the script's time budget.
struct BudgetPause {
    ScriptHost::Impl& h;
    ScriptHost::Impl::Bridge& b;
    double start{now_seconds()};
    ~BudgetPause() {
        const double waited = now_seconds() - start;
        b.wait_seconds += waited;
        if (h.deadline > 0) h.deadline += waited;
    }
};

// Reads one message; nullopt on timeout. Raises a Luau error when the helper exits or sends
// something that is not a JSON object.
std::optional<Value> bridge_read(lua_State* L, ScriptHost::Impl::Bridge& b, int timeout_ms) {
    auto line = b.child->read_line(timeout_ms);
    if (!line) {
        const std::string msg = line.error().message;
        const auto code = b.child->wait(200);
        luaL_error(L, "bridge '%s' stopped (exit code %d): %s", b.name.c_str(), code ? *code : -1, msg.c_str());
    }
    if (!*line) return std::nullopt;
    auto doc = json::parse(**line);
    if (!doc || !doc->is_object()) {
        std::string head = (*line)->substr(0, 120);
        luaL_error(L, "bridge '%s' sent a line that is not a JSON object: %s", b.name.c_str(), head.c_str());
    }
    return std::move(*doc);
}

int timeout_ms_arg(lua_State* L, int idx, double fallback_seconds) {
    const double seconds = luaL_optnumber(L, idx, fallback_seconds);
    if (seconds < 0) return -1;
    return static_cast<int>(std::min(seconds, 86400.0) * 1000.0);
}

std::uint64_t bridge_write(lua_State* L, ScriptHost::Impl::Bridge& b, int idx, bool assign_id) {
    luaL_checktype(L, idx, LUA_TTABLE);
    Value msg = to_json(L, idx);
    if (!msg.is_object()) luaL_error(L, "bridge messages are tables with named fields");
    std::uint64_t id = 0;
    if (assign_id) {
        if (const Value* given = msg.find("id"); given && given->is_number()) id = static_cast<std::uint64_t>(given->as_int());
        else {
            id = b.next_id++;
            msg.set("id", Value(static_cast<double>(id)));
        }
    }
    if (auto st = b.child->write_line(json::stringify(msg, false)); !st.ok())
        luaL_error(L, "bridge '%s': %s", b.name.c_str(), st.error().message.c_str());
    return id;
}

// bridge.open(name) -> Bridge | nil, error. The same name returns the running helper.
int bridge_open(lua_State* L) {
    auto& h = host_of(L);
    const std::string name = luaL_checkstring(L, 1);
    for (auto& [id, b] : h.bridges)
        if (b.name == name && b.child && b.child->running()) {
            push_bridge(L, id);
            return 1;
        }
    if (!h.options.bridge_launcher) {
        lua_pushnil(L);
        lua_pushstring(L, "bridges are not available here (no project bridges)");
        return 2;
    }
    auto launch = h.options.bridge_launcher(name);
    if (!launch) {
        lua_pushnil(L);
        lua_pushstring(L, launch.error().message.c_str());
        return 2;
    }
    auto child = process::ChildProcess::spawn(*launch);
    if (!child) {
        lua_pushnil(L);
        lua_pushstring(L, std::format("bridge '{}': {}", name, child.error().message).c_str());
        return 2;
    }
    VICTUS_LOG_INFO("script", "bridge '{}' started (pid {})", name, (*child)->pid());
    const int id = h.next_bridge++;
    auto& b = h.bridges[id];
    b.name = name;
    b.child = std::move(*child);
    push_bridge(L, id);
    return 1;
}

// b:request(message, timeout_seconds = 60) -> reply. Sends one JSON line (with an "id" added
// when missing) and waits for the reply echoing it; other messages are queued for receive().
int bridge_request(lua_State* L) {
    auto& h = host_of(L);
    auto& b = check_bridge(L, 1);
    const int timeout = timeout_ms_arg(L, 3, 60.0);
    const std::uint64_t id = bridge_write(L, b, 2, true);
    ++b.requests;
    std::optional<Value> reply;
    {
        BudgetPause pause{h, b};
        const double give_up = timeout < 0 ? 0 : now_seconds() + timeout / 1000.0;
        while (!reply) {
            const int left = timeout < 0 ? -1 : std::max(0, static_cast<int>((give_up - now_seconds()) * 1000.0));
            auto msg = bridge_read(L, b, left);
            if (!msg) luaL_error(L, "bridge '%s': no reply within %.1f s", b.name.c_str(), timeout / 1000.0);
            const Value* rid = msg->find("id");
            if (rid && rid->is_number() && static_cast<std::uint64_t>(rid->as_int()) == id) reply = std::move(msg);
            else if (b.inbox.size() < 4096) b.inbox.push_back(std::move(*msg));
        }
    }
    push_json(L, *reply);
    return 1;
}

// b:send(message) -> id (no wait; the reply arrives through receive()).
int bridge_send(lua_State* L) {
    auto& b = check_bridge(L, 1);
    lua_pushnumber(L, static_cast<double>(bridge_write(L, b, 2, true)));
    return 1;
}

// b:receive(timeout_seconds = 0) -> message | nil
int bridge_receive(lua_State* L) {
    auto& h = host_of(L);
    auto& b = check_bridge(L, 1);
    if (!b.inbox.empty()) {
        push_json(L, b.inbox.front());
        b.inbox.erase(b.inbox.begin());
        return 1;
    }
    const int timeout = timeout_ms_arg(L, 2, 0.0);
    std::optional<Value> msg;
    {
        BudgetPause pause{h, b};
        msg = bridge_read(L, b, timeout);
    }
    if (msg) push_json(L, *msg);
    else lua_pushnil(L);
    return 1;
}

int bridge_running(lua_State* L) {
    auto* ref = static_cast<BridgeRef*>(lua_touserdatatagged(L, 1, kBridgeTag));
    if (!ref) luaL_typeerror(L, 1, "Bridge");
    auto& h = host_of(L);
    const auto it = h.bridges.find(ref->id);
    lua_pushboolean(L, it != h.bridges.end() && it->second.child && it->second.child->running());
    return 1;
}

// b:close(timeout_seconds = 2): closes the helper's input, waits, then stops it.
int bridge_close(lua_State* L) {
    auto* ref = static_cast<BridgeRef*>(lua_touserdatatagged(L, 1, kBridgeTag));
    if (!ref) luaL_typeerror(L, 1, "Bridge");
    auto& h = host_of(L);
    const auto it = h.bridges.find(ref->id);
    if (it == h.bridges.end()) return 0;
    const int timeout = timeout_ms_arg(L, 2, 2.0);
    if (it->second.child) {
        it->second.child->close_stdin();
        if (!it->second.child->wait(timeout)) it->second.child->kill();
    }
    h.bridges.erase(it);
    return 0;
}

// b:stats() -> {name, requests, wait_seconds, queued, pid}
int bridge_stats(lua_State* L) {
    auto& b = check_bridge(L, 1);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, b.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, static_cast<double>(b.requests));
    lua_setfield(L, -2, "requests");
    lua_pushnumber(L, b.wait_seconds);
    lua_setfield(L, -2, "wait_seconds");
    lua_pushinteger(L, static_cast<int>(b.inbox.size()));
    lua_setfield(L, -2, "queued");
    lua_pushnumber(L, static_cast<double>(b.child->pid()));
    lua_setfield(L, -2, "pid");
    return 1;
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

// require(path) -> the table a library script returns, shared by every script that requires it
// (loaded once per host). `path` is relative to the assets folder; ".luau" may be left out.
int script_require(lua_State* L) {
    auto& h = host_of(L);
    std::string path = luaL_checkstring(L, 1);
    if (!path.ends_with(".luau")) path += ".luau";
    if (std::find(h.loading.begin(), h.loading.end(), path) != h.loading.end())
        luaL_error(L, "require: '%s' is already being loaded (circular require)", path.c_str());
    auto* m = h.module(path);
    if (!m) luaL_error(L, "require: cannot load '%s' (see the script errors above)", path.c_str());
    lua_getref(L, m->ref);
    return 1;
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
                                {"add_force", entity_physics_apply<&physics::PhysicsWorld::add_force>},
                                {"add_impulse", entity_physics_apply<&physics::PhysicsWorld::add_impulse>},
                                {"add_torque", entity_physics_apply<&physics::PhysicsWorld::add_torque>},
                                {"add_angular_impulse", entity_physics_apply<&physics::PhysicsWorld::add_angular_impulse>},
                                {"move_character", entity_move_character},
                                {"jump", entity_jump},
                                {"drop_through", entity_drop_through},
                                {"character_state", entity_character_state},
                                {"play_animation", entity_play_animation},
                                {"play_sprite", entity_play_sprite},
                                {"sprite_animation", entity_sprite_animation},
                                {"sprite_clips", entity_sprite_clips},
                                {"animation", entity_animation},
                                {"animation_clips", entity_animation_clips},
                                {"emit_particles", entity_emit_particles},
                                {"set_emitting", entity_set_emitting},
                                {"reset_particles", entity_reset_particles},
                                {"particles", entity_particles},
                                {"joint_state", entity_joint_state},
                                {"set_motor", entity_set_motor},
                                {"play_audio", entity_play_audio},
                                {"stop_audio", entity_stop_audio},
                                {"pause_audio", entity_pause_audio},
                                {"audio_playing", entity_audio_playing},
                                {"set_text", entity_set_text},
                                {"text", entity_text},
                                {"set_visible", entity_set_visible},
                                {"visible", entity_visible},
                                {"ui_state", entity_ui_state},
                                {"set_material", entity_set_material},
                                {"material", entity_material},
                                {"set_mesh", entity_set_mesh},
                                {"mesh", entity_mesh},
                                {"set_mesh_data", entity_set_mesh_data},
                                {"clear_mesh_data", entity_clear_mesh_data},
                                {"mesh_data", entity_mesh_data},
                                {"set_color", entity_set_color},
                                {"color", entity_color},
                                {"set_sprite", entity_set_sprite},
                                {"sprite", entity_sprite},
                                {"set_cast_shadows", entity_set_cast_shadows},
                                {"set_tile", entity_set_tile},
                                {"tile", entity_tile},
                                {"fill_tiles", entity_fill_tiles},
                                {"clear_tiles", entity_clear_tiles},
                                {"paint_terrain", entity_paint_terrain},
                                {"world_to_tile", entity_world_to_tile},
                                {"tile_to_world", entity_tile_to_world},
                                {"tile_bounds", entity_tile_bounds},
                                {"set_uv_offset", entity_set_uv_offset},
                                {"set_uv_scroll", entity_set_uv_scroll},
                                {"set_emissive", entity_set_emissive},
                                {"set_destination", entity_set_destination},
                                {"stop", entity_stop},
                                {"nav_state", entity_nav_state},
                                {"bb_get", entity_bb_get},
                                {"bb_set", entity_bb_set},
                                {"blackboard", entity_blackboard},
                                {"perception", entity_perception},
                                {"play_sequence", entity_play_sequence},
                                {"stop_sequence", entity_stop_sequence},
                                {"pause_sequence", entity_pause_sequence},
                                {"seek_sequence", entity_seek_sequence},
                                {"sequence_state", entity_sequence_state},
                                {"set_net", entity_set_net},
                                {"get_net", entity_get_net},
                                {"has_authority", entity_has_authority},
                                {"is_local", entity_is_local},
                                {"owner", entity_owner},
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
                                  {"query", scene_query},   {"count", scene_count}, {"instantiate", scene_instantiate},
                                  {"find_id", scene_find_id}, {"load", scene_load},
                                  {"reload", scene_reload},   {"current", scene_current},
                                  {"params", scene_params},   {nullptr, nullptr}};
    register_lib(L, "scene", scene_fns);
    {
        lua_newtable(L); // Bridge metatable
        lua_newtable(L); // methods
        const luaL_Reg bridge_methods[] = {{"request", bridge_request}, {"send", bridge_send},
                                           {"receive", bridge_receive}, {"running", bridge_running},
                                           {"close", bridge_close},     {"stats", bridge_stats},
                                           {nullptr, nullptr}};
        for (const luaL_Reg* f = bridge_methods; f->name; ++f) {
            lua_pushcfunction(L, f->func, f->name);
            lua_setfield(L, -2, f->name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "Bridge");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
        lua_setreadonly(L, -1, 1);
        lua_setuserdatametatable(L, kBridgeTag);
        const luaL_Reg bridge_fns[] = {{"open", bridge_open}, {nullptr, nullptr}};
        register_lib(L, "bridge", bridge_fns);
        const luaL_Reg json_fns[] = {{"encode", json_encode}, {"decode", json_decode}, {nullptr, nullptr}};
        register_lib(L, "json", json_fns);
    }
    const luaL_Reg input_fns[] = {{"down", input_down},
                                  {"pressed", input_pressed},
                                  {"released", input_released},
                                  {"mouse_down", input_mouse_down},
                                  {"mouse_pressed", input_mouse_pressed},
                                  {"mouse_position", input_mouse_position},
                                  {"mouse_wheel", input_mouse_wheel}, {"mouse_delta", input_mouse_delta},
                                  {"touch_count", input_touch_count}, {"touch", input_touch},
                                  {"axis", input_axis},
                                  {"action", input_action},
                                  {"action_pressed", input_action_pressed},
                                  {"action_released", input_action_released},
                                  {"value", input_value},
                                  {"vector", input_vector},
                                  {"actions", input_actions},
                                  {"bindings", input_bindings},
                                  {"rebind", input_rebind},
                                  {"gamepad_connected", input_gamepad_connected},
                                  {"gamepad_button", input_gamepad_button},
                                  {"gamepad_axis", input_gamepad_axis},
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
    const luaL_Reg physics_fns[] = {{"available", physics_available},
                                    {"raycast", physics_raycast},
                                    {"raycast2d", physics_raycast2d},
                                    {"overlap_sphere", physics_overlap_sphere},
                                    {"overlap_box", physics_overlap_box},
                                    {"overlap_circle", physics_overlap_circle},
                                    {"overlap_box2d", physics_overlap_box2d},
                                    {"gravity", physics_gravity},
                                    {"set_gravity", physics_set_gravity},
                                    {"gravity2d", physics_gravity2d},
                                    {"set_gravity2d", physics_set_gravity2d},
                                    {nullptr, nullptr}};
    register_lib(L, "physics", physics_fns);
    const luaL_Reg audio_fns[] = {{"available", audio_available}, {"play", audio_play},
                                  {"set_volume", audio_set_volume}, {"volume", audio_volume},
                                  {"set_muted", audio_set_muted}, {"muted", audio_muted},
                                  {nullptr, nullptr}};
    register_lib(L, "audio", audio_fns);
    const luaL_Reg ui_fns[] = {{"screen_size", ui_screen_size}, {"pointer_over_ui", ui_pointer_over},
                               {"focus", ui_focus},             {"focused", ui_focused},
                               {"click", ui_click},             {nullptr, nullptr}};
    register_lib(L, "ui", ui_fns);
    const luaL_Reg nav_fns[] = {{"available", nav_available},       {"find_path", nav_find_path},
                                {"nearest", nav_nearest},           {"raycast", nav_raycast},
                                {"random_point", nav_random_point}, {"rebuild", nav_rebuild},
                                {nullptr, nullptr}};
    register_lib(L, "nav", nav_fns);
    const luaL_Reg terrain_fns[] = {{"height", terrain_height}, {"sample", terrain_sample},
                                    {"raycast", terrain_raycast}, {nullptr, nullptr}};
    register_lib(L, "terrain", terrain_fns);
    const luaL_Reg ai_fns[] = {{"available", ai_available}, {"report_noise", ai_report_noise}, {nullptr, nullptr}};
    register_lib(L, "ai", ai_fns);
    const luaL_Reg sequencer_fns[] = {{"available", sequencer_available}, {nullptr, nullptr}};
    register_lib(L, "sequencer", sequencer_fns);
    const luaL_Reg net_fns[] = {{"role", net_role},
                                {"is_server", net_is_server},
                                {"is_client", net_is_client},
                                {"authority", net_authority},
                                {"id", net_id},
                                {"connected", net_connected},
                                {"local_player", net_local_player},
                                {"send", net_send},
                                {"clients", net_clients},
                                {"stats", net_stats},
                                {nullptr, nullptr}};
    register_lib(L, "net", net_fns);
    const luaL_Reg save_fns[] = {{"available", save_available}, {"version", save_version},
                                 {"write", save_write},         {"read", save_read},
                                 {"exists", save_exists},       {"delete", save_delete},
                                 {"list", save_list},           {"migrate", save_migrate},
                                 {"checkpoint", save_checkpoint}, {"restore", save_restore},
                                 {nullptr, nullptr}};
    register_lib(L, "save", save_fns);
    lua_pushcfunction(L, log_info, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, script_require, "require");
    lua_setglobal(L, "require");
    lua_pushcfunction(L, graph_trace_for, "__vg_trace_for");
    lua_setglobal(L, "__vg_trace_for");
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

// ---- capture probe -------------------------------------------------------------------------------

namespace {

struct HostBinding {
    ScriptHost::Impl* host{nullptr};
    std::weak_ptr<char> alive;
};

void bind_host(scene::Scene& s, ScriptHost::Impl& h) {
    auto* b = s.world().resource<HostBinding>();
    if (!b) b = &s.world().set_resource<HostBinding>();
    if (b->host == &h && !b->alive.expired()) return;
    b->host = &h;
    b->alive = h.alive;
}

constexpr std::size_t kProbeStringLimit = 64;

// Capture digests: every started script's own fields (self.x = ...) of type number, boolean,
// string or vector, as "script.<field>" lines. Fields starting with '_' are private and skipped.
void script_probe(const scene::Scene& cs, std::vector<std::string>& lines) {
    auto& s = const_cast<scene::Scene&>(cs); // read-only
    const auto* b = s.world().resource<HostBinding>();
    if (!b || !b->host) return;
    const auto alive = b->alive.lock();
    if (!alive) return;
    ScriptHost::Impl& h = *b->host;
    lua_State* L = h.L;
    if (!L || !lua_checkstack(L, 4)) return;
    for (const auto& [k, in] : h.instances) {
        if (!in.started || in.faulted || in.ref == LUA_NOREF) continue;
        const Entity e{static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32)};
        if (!s.world().alive(e)) continue;
        lua_getref(L, in.ref);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) { // raw traversal: no metamethods run
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::size_t n = 0;
                const char* key = lua_tolstring(L, -2, &n);
                std::string value;
                bool keep = n > 0 && key[0] != '_';
                switch (lua_type(L, -1)) {
                case LUA_TNUMBER: value = gameplay::probe_number(lua_tonumber(L, -1)); break;
                case LUA_TBOOLEAN: value = lua_toboolean(L, -1) ? "true" : "false"; break;
                case LUA_TSTRING: {
                    std::size_t len = 0;
                    const char* str = lua_tolstring(L, -1, &len);
                    value = "\"" + std::string(str, std::min(len, kProbeStringLimit)) + (len > kProbeStringLimit ? "...\"" : "\"");
                    break;
                }
                case LUA_TVECTOR: {
                    const float* v = lua_tovector(L, -1);
                    value = gameplay::probe_vector(math::Vec3{v[0], v[1], v[2]});
                    break;
                }
                default: keep = false; break;
                }
                if (keep) lines.push_back(gameplay::probe_line(s, e, "script." + std::string(key, n), value));
            }
            lua_pop(L, 1); // value; keep the key for lua_next
        }
        lua_pop(L, 1); // instance
    }
}

} // namespace

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
    if (const std::uint64_t seed = impl->options.random_seed) {
        // math.randomseed takes a 32-bit integer; fold the 64-bit seed into it.
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "randomseed");
        lua_pushinteger(L, static_cast<int>(static_cast<std::uint32_t>(seed ^ (seed >> 32)) & 0x7fffffff));
        lua_call(L, 1, 0);
        lua_pop(L, 1);
    }
    luaL_sandbox(L);
    impl->next_watch = now_seconds() + impl->options.watch_interval_seconds;
    return std::unique_ptr<ScriptHost>(new ScriptHost(std::move(impl)));
}

namespace {

// Behaviour tree action leaves with no C++ action run `self:<action>(dt, first)` on the entity's
// script, which returns "running", "success" or "failure" (true/nil = success, false = failure).
// An interrupted running leaf calls `self:on_task_abort(action)` when the script defines it.
ai::NodeStatus run_script_task(ScriptHost::Impl& h, ai::ActionContext& c) {
    auto it = h.instances.find(c.entity.bits());
    if (it == h.instances.end() || it->second.faulted || !it->second.started) return ai::NodeStatus::Idle;
    auto& in = it->second;
    lua_State* L = h.L;
    h.scene = &c.scene;
    h.route(c.entity);
    const std::string& name = c.node.action;
    lua_getref(L, in.ref);
    lua_getfield(L, -1, c.aborted ? "on_task_abort" : name.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return ai::NodeStatus::Idle;
    }
    lua_pushvalue(L, -2);
    int nargs = 1;
    if (c.aborted) {
        lua_pushlstring(L, name.data(), name.size());
        nargs = 2;
    } else {
        lua_pushnumber(L, c.dt);
        lua_pushboolean(L, c.first);
        nargs = 3;
    }
    if (!h.pcall(nargs, 1, std::format("{}:{}", in.path, c.aborted ? "on_task_abort" : name))) {
        lua_pop(L, 1); // instance
        in.faulted = true;
        VICTUS_LOG_WARN("script", "{} disabled until it is reloaded", in.path);
        return ai::NodeStatus::Failure;
    }
    ai::NodeStatus r = ai::NodeStatus::Success;
    if (lua_isboolean(L, -1)) {
        r = lua_toboolean(L, -1) ? ai::NodeStatus::Success : ai::NodeStatus::Failure;
    } else if (lua_isstring(L, -1) && !lua_isnumber(L, -1)) {
        const std::string_view v = lua_tostring(L, -1);
        if (v == "running") r = ai::NodeStatus::Running;
        else if (v == "failure") r = ai::NodeStatus::Failure;
        else if (v != "success") {
            h.report(std::format("{}:{}: task returned \"{}\" (expected running, success or failure)", in.path,
                                 name, v));
            r = ai::NodeStatus::Failure;
        }
    }
    lua_pop(L, 2); // result, instance
    return r;
}

void bind_ai_tasks(scene::Scene& scene, ScriptHost::Impl& h) {
    ai::AiSystem* a = ai::ai_of(scene);
    if (!a || (a == h.bound_ai && a->has_fallback_action())) return;
    h.bound_ai = a;
    a->set_fallback_action([&h](ai::ActionContext& c) { return run_script_task(h, c); });
}

} // namespace

void ScriptHost::update(scene::Scene& scene, const gameplay::FrameTime& time, const gameplay::InputSource* input) {
    VICTUS_PROFILE_SCOPE("script.update");
    Impl& h = *impl_;
    bind_host(scene, h);
    bind_ai_tasks(scene, h);
    h.scene = &scene;
    h.time = time;
    h.input = input;
    h.default_input = input;
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
        h.route(item.e);
        if (!in.started) {
            in.started = true;
            if (!h.call(in, "start", nullptr)) continue;
            if (!scene.world().alive(item.e)) continue;
            if (!h.pending_states.empty()) {
                if (auto ps = h.pending_states.find(scene.uuid(item.e).to_string()); ps != h.pending_states.end()) {
                    const Value state = std::move(ps->second);
                    h.pending_states.erase(ps);
                    lua_State* L = h.L;
                    if (!h.call_with(in, "load_state", [&] {
                            push_json(L, state, 0, true);
                            return 1;
                        }))
                        continue;
                    if (!scene.world().alive(item.e)) continue;
                }
            }
        }
        (void)h.call(in, "update", &dt);
    }
    dispatch_physics_events(scene);
    dispatch_ui_events(scene);
    dispatch_sequence_events(scene);
    dispatch_net_messages(scene);
    h.pending_states.clear(); // states of entities that no longer have a script
    if (h.pending_restore) {
        const Value data = std::move(*h.pending_restore);
        h.pending_restore.reset();
        if (auto st = apply_checkpoint(h, scene, data); !st) h.report(std::format("save.restore: {}", st.error().message));
    }
    if (h.pending_scene) {
        auto req = std::move(*h.pending_scene);
        h.pending_scene.reset();
        const std::string path = req.path;
        if (auto st = apply_scene(h, scene, std::move(req)); !st) h.report(std::format("scene.load({}): {}", path, st.error().message));
    }
    h.input = h.default_input;
    h.counters.instances = h.instances.size();
}

void ScriptHost::dispatch_physics_events(scene::Scene& scene) {
    const auto* world = physics::world_of(scene);
    if (!world || world->events().empty()) return;
    Impl& h = *impl_;
    // Copy: callbacks may spawn/destroy entities, which never mutates the event list, but a
    // script could step the world in principle.
    const std::vector<physics::ContactEvent> events = world->events();
    auto notify = [&](Entity self, Entity other, const physics::ContactEvent& ev, bool flip) {
        if (!scene.world().alive(self)) return;
        const auto* sc = scene.world().get<ScriptComponent>(self);
        if (!sc || !sc->enabled) return;
        auto it = h.instances.find(self.bits());
        if (it == h.instances.end() || it->second.faulted || !it->second.started) return;
        const bool begin = ev.kind == physics::ContactEvent::Kind::Begin;
        const char* fn = ev.trigger ? (begin ? "on_trigger_enter" : "on_trigger_exit")
                                    : (begin ? "on_collision_enter" : "on_collision_exit");
        lua_State* L = h.L;
        h.route(self);
        (void)h.call_with(it->second, fn, [&] {
            push_entity(L, scene.world().alive(other) ? other : Entity{});
            if (ev.trigger || !begin) return 1;
            lua_createtable(L, 0, 2);
            push_vec3(L, ev.point);
            lua_setfield(L, -2, "point");
            push_vec3(L, flip ? ev.normal * -1.0f : ev.normal);
            lua_setfield(L, -2, "normal");
            return 2;
        });
    };
    for (const auto& ev : events) {
        notify(ev.a, ev.b, ev, false);
        notify(ev.b, ev.a, ev, true);
    }
}

void ScriptHost::dispatch_ui_events(scene::Scene& scene) {
    if (gameui::events(scene).empty()) return;
    Impl& h = *impl_;
    // Copy: on_click handlers may click other buttons (those events are seen next frame).
    const std::vector<gameui::ClickEvent> events = gameui::events(scene);
    gameui::mark_events_delivered(scene);
    for (const auto& ev : events) {
        // The button and then each ancestor, so a menu script on a parent handles its buttons.
        for (Entity e = ev.button; !e.is_null() && scene.world().alive(e); e = scene.parent(e)) {
            const auto* sc = scene.world().get<ScriptComponent>(e);
            if (!sc || !sc->enabled) continue;
            auto it = h.instances.find(e.bits());
            if (it == h.instances.end() || it->second.faulted || !it->second.started) continue;
            lua_State* L = h.L;
            h.route(e);
            (void)h.call_with(it->second, "on_click", [&] {
                push_entity(L, scene.world().alive(ev.button) ? ev.button : Entity{});
                lua_pushlstring(L, ev.action.data(), ev.action.size());
                return 2;
            });
        }
    }
}

void ScriptHost::dispatch_sequence_events(scene::Scene& scene) {
    const std::uint64_t done = sequencer::delivered_through(scene);
    std::vector<sequencer::SequenceEvent> pending;
    for (const auto& ev : sequencer::events(scene))
        if (ev.id > done) pending.push_back(ev);
    if (pending.empty()) return;
    // Copy and mark first: handlers may play sequences whose events are seen next frame.
    sequencer::mark_delivered(scene, pending.back().id);
    Impl& h = *impl_;
    for (const auto& ev : pending) {
        // The track target, then the player (once when they are the same entity).
        const Entity receivers[2] = {ev.target, ev.target == ev.player ? Entity{} : ev.player};
        for (const Entity e : receivers) {
            if (e.is_null() || !scene.world().alive(e)) continue;
            const auto* sc = scene.world().get<ScriptComponent>(e);
            if (!sc || !sc->enabled) continue;
            auto it = h.instances.find(e.bits());
            if (it == h.instances.end() || it->second.faulted || !it->second.started) continue;
            lua_State* L = h.L;
            h.route(e);
            (void)h.call_with(it->second, "on_sequence_event", [&] {
                lua_pushlstring(L, ev.name.data(), ev.name.size());
                lua_pushlstring(L, ev.payload.data(), ev.payload.size());
                push_entity(L, scene.world().alive(ev.player) ? ev.player : Entity{});
                return 3;
            });
        }
    }
}

void ScriptHost::fixed_update(scene::Scene& scene, const gameplay::FrameTime& time,
                              const gameplay::InputSource* input) {
    VICTUS_PROFILE_SCOPE("script.fixed_update");
    Impl& h = *impl_;
    bind_host(scene, h);
    h.scene = &scene;
    h.time = time;
    h.fixed_dt = time.dt;
    h.input = input;
    h.default_input = input;
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
        h.route(e);
        (void)h.call(it->second, "fixed_update", &dt);
    }
    h.input = h.default_input;
}

void ScriptHost::dispatch_net_messages(scene::Scene& scene) {
    net::NetContext* ctx = net::context_of(scene);
    if (!ctx) return;
    const std::vector<net::NetMessage> messages = ctx->take_messages();
    if (messages.empty()) return;
    Impl& h = *impl_;
    std::vector<std::uint64_t> keys;
    for (const auto& [k, in] : h.instances)
        if (in.started && !in.faulted) keys.push_back(k);
    for (const auto& m : messages) {
        for (const std::uint64_t k : keys) {
            const Entity e{static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32)};
            if (!scene.world().alive(e)) continue;
            const auto* sc = scene.world().get<ScriptComponent>(e);
            if (!sc || !sc->enabled) continue;
            auto it = h.instances.find(k);
            if (it == h.instances.end() || it->second.faulted) continue;
            lua_State* L = h.L;
            h.route(e);
            (void)h.call_with(it->second, "on_net_message", [&] {
                lua_pushlstring(L, m.name.data(), m.name.size());
                push_json(L, m.value);
                lua_pushinteger(L, m.sender);
                return 3;
            });
        }
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

Status ScriptHost::checkpoint(scene::Scene& scene, std::string_view slot, std::string_view label) {
    Impl& h = *impl_;
    scene::Scene* outer = h.scene;
    h.scene = &scene;
    lua_State* L = h.L;
    const int top = lua_gettop(L);
    lua_pushcfunction(L, save_checkpoint, "save.checkpoint");
    lua_pushstring(L, std::string(slot).c_str()); // (an empty string_view may carry a null pointer)
    lua_pushstring(L, std::string(label).c_str());
    Status result;
    if (!h.pcall(2, 2, "checkpoint")) {
        result = make_error(ErrorCode::ScriptError, "checkpoint failed (see the script errors)");
    } else if (!lua_toboolean(L, -2)) {
        const char* msg = lua_tostring(L, -1);
        result = make_error(ErrorCode::IoError, msg ? msg : "checkpoint failed");
    }
    lua_settop(L, top);
    h.scene = outer;
    return result;
}

Status ScriptHost::restore(scene::Scene& scene, std::string_view slot) {
    Impl& h = *impl_;
    auto data = read_checkpoint(h, scene, slot);
    if (!data) return std::move(data).error();
    h.pending_restore.reset();
    return apply_checkpoint(h, scene, *data);
}

gameplay::SaveStore* ScriptHost::saves() { return impl_->store(); }

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

std::unordered_map<std::uint32_t, std::uint64_t> ScriptHost::graph_node_hits(std::string_view path) const {
    const auto it = impl_->graph_hits.find(std::string(path));
    return it == impl_->graph_hits.end() ? std::unordered_map<std::uint32_t, std::uint64_t>{} : it->second;
}

std::optional<GraphRuntimeError> ScriptHost::last_graph_error(std::string_view path) const {
    const auto it = impl_->graph_errors.find(std::string(path));
    if (it == impl_->graph_errors.end()) return std::nullopt;
    return it->second;
}
void ScriptHost::forget_prefabs() { impl_->prefabs.clear(); }
void ScriptHost::set_input_map(gameplay::InputMap map) { impl_->options.input_map = std::move(map); }
void ScriptHost::set_current_scene(std::string path) { impl_->current_scene = std::move(path); }
const std::string& ScriptHost::current_scene() const noexcept { return impl_->current_scene; }

Status ScriptHost::load_scene(scene::Scene& scene, std::string_view path, json::Value params) {
    Impl& h = *impl_;
    auto doc = read_scene(h, path);
    if (!doc) return std::move(doc).error();
    h.pending_scene.reset();
    return apply_scene(h, scene, Impl::SceneRequest{std::string(path), std::move(*doc), std::move(params)});
}

void add_script_systems(gameplay::Simulation& sim, ScriptHost& host) {
    gameplay::register_state_probe("script", script_probe);
    gameplay::Simulation* simp = &sim;
    sim.add_system("script.fixed_update", gameplay::Phase::FixedUpdate,
                   [&host, simp](scene::Scene& s, const gameplay::FrameTime& t) { host.fixed_update(s, t, simp->input); });
    sim.add_system("script.update", gameplay::Phase::Update,
                   [&host, simp](scene::Scene& s, const gameplay::FrameTime& t) { host.update(s, t, simp->input); });
}

Result<std::string> luau_source(std::string_view path, std::string text) {
    if (!path.ends_with(".vgraph")) return text;
    auto graph = parse_graph(text);
    if (!graph) return make_error(graph.error().code, std::format("{}: {}", path, graph.error().message));
    GraphCompileOptions opt;
    opt.source_name = std::string(path);
    CompiledGraph compiled = compile_graph(*graph, opt);
    if (!compiled.ok()) return make_error(ErrorCode::ScriptError, std::format("{}: {}", path, compiled.error_text()));
    return std::move(compiled.source);
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

std::vector<std::string> scene_references(std::string_view source) {
    std::vector<std::string> out;
    constexpr std::string_view kCall = "scene.load";
    for (std::size_t at = source.find(kCall); at != std::string_view::npos; at = source.find(kCall, at + 1)) {
        if (at > 0) {
            const char before = source[at - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '.') continue;
        }
        std::size_t i = at + kCall.size();
        while (i < source.size() && (source[i] == ' ' || source[i] == '\t')) ++i;
        if (i >= source.size() || source[i] != '(') continue;
        ++i;
        while (i < source.size() && (source[i] == ' ' || source[i] == '\t')) ++i;
        if (i >= source.size() || (source[i] != '"' && source[i] != '\'')) continue;
        const char quote = source[i++];
        const std::size_t end = source.find(quote, i);
        if (end == std::string_view::npos) continue;
        const std::string_view path = source.substr(i, end - i);
        if (path.find('\\') != std::string_view::npos || path.empty()) continue; // escapes: leave to runtime
        if (std::find(out.begin(), out.end(), path) == out.end()) out.emplace_back(path);
    }
    return out;
}

} // namespace victus::script
