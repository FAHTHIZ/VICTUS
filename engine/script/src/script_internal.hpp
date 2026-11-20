// SPDX-License-Identifier: Apache-2.0
// Internal to engine/script: the ScriptHost implementation state and the helpers the binding
// translation units share. Not installed; include only from engine/script/src.
#pragma once
#include "victus/core/log.hpp"
#include "victus/animation/animation.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/gameui/localization.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/script/script.hpp"
#include "victus/script/behaviours.hpp"
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
#include <atomic>
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

namespace detail {

constexpr int kEntityTag = 1;
constexpr int kMaxConvertDepth = 32;
constexpr std::size_t kMaxRecentErrors = 32;
constexpr std::uint64_t kMaxScriptBytes = 4ull << 20;

struct EntityRef {
    std::uint32_t index;
    std::uint32_t generation;
};

inline double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace detail

using namespace detail;  // internal header: the binding TUs use detail:: names unqualified

// ---------------------------------------------------------------------------------------------

struct ScriptHost::Impl {
    struct Module {
        std::string path;
        int ref{LUA_NOREF};
        sfs::file_time_type mtime{};
        std::uint64_t version{0};
        bool ok{false};
        int chunk{LUA_NOREF}; // the compiled main function (breakpoints are set through it)
        int lines{0};         // source lines
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

    // Source debugger state (ADR-0099; debugger.cpp).
    struct DebugBreakpoint {
        SourceBreakpoint request;
        int line{0};
        bool verified{false};
        std::string message;
        std::uint64_t hits{0};
    };
    struct Debugger {
        DebugHandler handler;
        DebugOutput output;
        std::unordered_map<std::string, std::vector<DebugBreakpoint>> breakpoints;
        bool break_on_error{false};
        std::atomic<bool> pause{false};
        // Stepping: the action, the thread, stack depth and line it started from (line -1: stop
        // at the next line anywhere).
        StepAction step{StepAction::Continue};
        lua_State* step_thread{nullptr};
        int step_depth{0};
        int step_line{0};
        bool singlestep{false}; // the running top-level call executes in single-step mode
        bool flooded{false};    // every line of every module has a breakpoint (step from a breakpoint)
        bool in_handler{false};
        int call_depth{0};
        lua_State* lines_thread{nullptr};
        std::vector<std::pair<int, int>> lines; // (proto id, line) per stack depth (single step)
        DebugStats stats;
    };
    Debugger dbg;
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
    // Behaviours (ADR-0120; behaviours.cpp): the scene as it was when play (or the last scene
    // load) started, used by RestartScene and game over, and a deterministic random stream.
    struct Behaviours {
        Value start_doc;
        bool has_start{false};
        std::uint64_t rng{0x9E3779B97F4A7C15ull};
        std::unordered_map<std::uint64_t, bool> warned; // entity bits -> reported a problem
        // Game Rules values handed from one level to the next (carry_over), and the values the
        // current level started with (what game over restarts with, lives refilled).
        struct Carry {
            float score{0};
            std::uint32_t lives_left{0}; // 0 = refill to the level's lives
            float elapsed{0};
        };
        std::optional<Carry> carry, entry;
        // The Trigger message on screen: its text, seconds left and, for "while touching"
        // messages, the trigger that keeps it up.
        std::string message;
        float message_left{0};
        Uuid message_owner;
        bool message_shown{false};
    bool start_pending{false};
    std::optional<float> best_score; // read from the save folder once
    bool title_shown{false};
    } behaviours;
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

    // String tables behind `loc` and keyed UiText: read on first use, kept across scene loads
    // so loc.set_locale lasts for the session.
    std::shared_ptr<gameui::Localization> localization;
    gameui::Localization& loc() {
        if (!localization) {
            localization = options.assets_root.empty()
                               ? std::make_shared<gameui::Localization>(options.default_locale)
                               : gameui::Localization::load(options.assets_root, options.default_locale);
            for (const auto& e : localization->errors()) VICTUS_LOG_WARN("script", "{}", e);
        }
        return *localization;
    }

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
    Entity current; // entity whose script is running (events.emit sender)
    std::uint64_t events_seen{0}; // last gameplay event delivered to on_event

    // Selects the input `e`'s script reads: multiplayer sessions route remote players' inputs to
    // the entities they own (and give proxies no input at all).
    void route(Entity e) {
        current = e;
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
        const bool top = dbg.call_depth == 0;
        int errfunc = 0;
        if (options.debugger) {
            if (top) debug_begin_call(T);
            if (dbg.break_on_error && dbg.handler) { // stop with the failing frames still on the stack
                lua_pushcfunction(T, &debug_error_handler, "debug_error_handler");
                lua_insert(T, -(nargs + 2));
                errfunc = lua_gettop(T) - nargs - 1;
            }
        }
        ++dbg.call_depth;
        const int st = lua_pcall(T, nargs, nresults, errfunc);
        --dbg.call_depth;
        if (errfunc) lua_remove(T, errfunc);
        if (options.debugger && top) debug_end_call(T);
        deadline = outer;
        ++counters.calls;
        if (st == LUA_OK) return true;
        const char* msg = lua_tostring(T, -1);
        report(std::format("{}: {}", what, msg ? msg : "unknown error"));
        lua_pop(T, 1);
        return false;
    }

    // Source debugger (debugger.cpp).
    void debug_begin_call(lua_State* T);
    void debug_end_call(lua_State* T);
    // Resolves and installs the breakpoints of a freshly compiled module (`chunk` is a ref).
    void debug_module_loaded(const std::string& path, int chunk, int lines);
    static int debug_error_handler(lua_State* L);

    std::string chunk_name(std::string_view path) const { return "@" + std::string(path); }

    // Compiles `source` and leaves the loaded function on L's stack inside a fresh sandboxed
    // thread (also left on the stack below the function). Returns false with an error message.
    bool load_chunk(std::string_view source, const std::string& name, std::string& error) {
        lua_CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = options.debugger ? 2 : 1; // 2: local names for the debugger
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
        const int chunk = (lua_pushvalue(L, -1), lua_ref(L, -1));
        lua_pop(L, 1);
        if (options.debugger) debug_module_loaded(m.path, chunk, 1 + static_cast<int>(std::count(text->begin(), text->end(), '\n')));
        if (!pcall(0, 1, m.path)) {
            lua_unref(L, chunk);
            lua_settop(L, top);
            return false;
        }
        if (m.chunk != LUA_NOREF) lua_unref(L, m.chunk);
        m.chunk = chunk;
        m.lines = 1 + static_cast<int>(std::count(text->begin(), text->end(), '\n'));
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

namespace detail {


// ---- bridges: helper processes speaking JSON lines ------------------------------------------------

constexpr int kBridgeTag = 2;

// script_lua.cpp
ScriptHost::Impl& host_of(lua_State* L);

// Time spent inside a native call that loads assets on first use (a character model's morph and
// clip data) is not charged to the script's time budget: the load is a one-off engine cost, not
// script work, and charging it kills the calling script on slow machines.
struct AssetLoadPause {
    ScriptHost::Impl& h;
    const char* what;
    double start{now_seconds()};
    AssetLoadPause(lua_State* L, const char* name) : h(host_of(L)), what(name) {}
    ~AssetLoadPause() {
        const double spent = now_seconds() - start;
        if (h.deadline > 0) h.deadline += spent;
        if (spent > 0.05) VICTUS_LOG_INFO("script", "entity:{}() spent {:.0f} ms resolving assets (not charged to the script budget)", what, spent * 1000.0);
    }
    AssetLoadPause(const AssetLoadPause&) = delete;
    AssetLoadPause& operator=(const AssetLoadPause&) = delete;
};
scene::Scene& scene_of(lua_State* L);
void* lua_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
void interrupt(lua_State* L, int gc);
// Pushes a native function for scripts (wrapped with a timer when VICTUS_SCRIPT_SLOW_CALLS is set).
void push_native(lua_State* L, lua_CFunction fn, const char* name);
void push_json(lua_State* L, const Value& v, int depth = 0, bool refs = false);
Value to_json(lua_State* L, int idx, int depth = 0, bool refs = false);
void push_entity(lua_State* L, Entity e);
Entity opt_entity(lua_State* L, int idx);
Entity check_entity(lua_State* L, int idx);
Entity check_live(lua_State* L, int idx, scene::Scene& s);
math::Vec3 check_vec3(lua_State* L, int idx);
void push_vec3(lua_State* L, math::Vec3 v);
math::Quat from_euler_degrees(math::Vec3 d);
const scene::ComponentSerializer* check_serializer(lua_State* L, int idx);
int entity_get(lua_State* L);
int entity_set(lua_State* L);
int entity_has(lua_State* L);
int entity_remove(lua_State* L);
int entity_destroy(lua_State* L);
int entity_find_child(lua_State* L);
int entity_children(lua_State* L);
int entity_set_parent(lua_State* L);
int entity_translate(lua_State* L);
int entity_rotate(lua_State* L);
int entity_look_at(lua_State* L);
int entity_index(lua_State* L);
int entity_newindex(lua_State* L);
int entity_eq(lua_State* L);
int entity_tostring(lua_State* L);
void register_lib(lua_State* L, const char* name, const luaL_Reg* fns);
int json_encode(lua_State* L);
int json_decode(lua_State* L);
bool editable(lua_State* L, int idx);

// bind_scene.cpp
int scene_find(lua_State* L);
int scene_spawn(lua_State* L);
int scene_instantiate(lua_State* L);
int scene_destroy(lua_State* L);
int scene_query(lua_State* L);
int scene_count(lua_State* L);
int input_down(lua_State* L);
int input_pressed(lua_State* L);
int input_released(lua_State* L);
int input_mouse_down(lua_State* L);
int input_mouse_pressed(lua_State* L);
int input_mouse_position(lua_State* L);
int input_mouse_delta(lua_State* L);
int input_mouse_wheel(lua_State* L);
int input_touch_count(lua_State* L);
int input_touch(lua_State* L);
int input_axis(lua_State* L);
int input_action(lua_State* L);
int input_action_pressed(lua_State* L);
int input_action_released(lua_State* L);
int input_value(lua_State* L);
int input_vector(lua_State* L);
int input_actions(lua_State* L);
int input_bindings(lua_State* L);
int input_rebind(lua_State* L);
int input_gamepad_connected(lua_State* L);
int input_gamepad_button(lua_State* L);
int input_gamepad_axis(lua_State* L);
int time_delta(lua_State* L);
int time_now(lua_State* L);
int time_frame(lua_State* L);
int time_fixed_delta(lua_State* L);
int log_info(lua_State* L);
int log_warn(lua_State* L);
int log_error(lua_State* L);
int graph_trace_for(lua_State* L);

// bind_physics.cpp
int physics_raycast(lua_State* L);
int physics_raycast2d(lua_State* L);
int physics_overlap_sphere(lua_State* L);
int physics_overlap_box(lua_State* L);
int physics_overlap_circle(lua_State* L);
int physics_overlap_box2d(lua_State* L);
int physics_gravity(lua_State* L);
int physics_set_gravity(lua_State* L);
int physics_gravity2d(lua_State* L);
int physics_set_gravity2d(lua_State* L);
int physics_available(lua_State* L);
int entity_move_character(lua_State* L);
int entity_jump(lua_State* L);
int entity_drop_through(lua_State* L);
int entity_character_state(lua_State* L);
int entity_joint_state(lua_State* L);
int entity_set_ragdoll(lua_State* L);
int entity_damage(lua_State* L);
// bind_gameplay.cpp
int entity_has_tag(lua_State* L);
int entity_add_tag(lua_State* L);
int entity_remove_tag(lua_State* L);
int entity_tags(lua_State* L);
int entity_start_timer(lua_State* L);
int entity_stop_timer(lua_State* L);
int entity_pause_timer(lua_State* L);
int entity_timer_left(lua_State* L);
int entity_attribute(lua_State* L);
int entity_attribute_base(lua_State* L);
int entity_set_attribute(lua_State* L);
int entity_modify_attribute(lua_State* L);
int entity_apply_effect(lua_State* L);
int entity_remove_effect(lua_State* L);
int entity_effect(lua_State* L);
int entity_activate_ability(lua_State* L);
int entity_ability_cooldown(lua_State* L);
int entity_add_item(lua_State* L);
int entity_remove_item(lua_State* L);
int entity_item_count(lua_State* L);
int entity_items(lua_State* L);
int entity_give_item(lua_State* L);
int scene_tagged(lua_State* L);
int events_emit(lua_State* L);
int water_query(lua_State* L);
int water_height(lua_State* L);
int entity_water_state(lua_State* L);
int entity_destructible_state(lua_State* L);
int entity_ragdoll_state(lua_State* L);
int entity_ragdoll_root(lua_State* L);
int entity_set_motor(lua_State* L);

// bind_render.cpp
int entity_set_text(lua_State* L);
int entity_set_text_key(lua_State* L);
int entity_scroll_to(lua_State* L);
int entity_scroll(lua_State* L);
int entity_text(lua_State* L);
int entity_set_visible(lua_State* L);
int entity_visible(lua_State* L);
bool is_primitive(std::string_view name);
Uuid resolve_asset(lua_State* L, std::string_view path, const char* fn);
void push_asset(lua_State* L, const Uuid& id);
scene::MeshRendererComponent& check_mesh_renderer(lua_State* L, scene::Scene& s, Entity e);
int entity_set_material(lua_State* L);
int entity_material(lua_State* L);
int entity_set_tile(lua_State* L);
int entity_tile(lua_State* L);
int entity_fill_tiles(lua_State* L);
int entity_clear_tiles(lua_State* L);
int entity_paint_terrain(lua_State* L);
int entity_world_to_tile(lua_State* L);
int entity_tile_to_world(lua_State* L);
int entity_tile_bounds(lua_State* L);
int entity_set_uv_scroll(lua_State* L);
int entity_set_emissive(lua_State* L);
int entity_ui_state(lua_State* L);
int ui_screen_size(lua_State* L);
int ui_pointer_over(lua_State* L);
int ui_focus(lua_State* L);
int ui_focused(lua_State* L);
int ui_click(lua_State* L);
int loc_text(lua_State* L);
int loc_has(lua_State* L);
int loc_locale(lua_State* L);
int loc_set_locale(lua_State* L);
int loc_locales(lua_State* L);
int loc_name(lua_State* L);
int entity_play_animation(lua_State* L);
int entity_animation(lua_State* L);
int entity_animation_clips(lua_State* L);
int entity_set_anim_param(lua_State* L);
int entity_anim_param(lua_State* L);
int entity_anim_trigger(lua_State* L);
int entity_reset_anim_trigger(lua_State* L);
int entity_anim_state(lua_State* L);
int entity_anim_controller(lua_State* L);
int entity_set_anim_layer_weight(lua_State* L);
int entity_anim_layer_weight(lua_State* L);
int entity_morph_targets(lua_State* L);
int entity_set_morph_weight(lua_State* L);
int entity_morph_weight(lua_State* L);
int entity_root_motion(lua_State* L);
int entity_set_ik_target(lua_State* L);
int entity_ik_state(lua_State* L);
int entity_play_sprite(lua_State* L);
int entity_sprite_animation(lua_State* L);
int entity_sprite_clips(lua_State* L);
int entity_emit_particles(lua_State* L);
int entity_set_emitting(lua_State* L);
int entity_reset_particles(lua_State* L);
int entity_particles(lua_State* L);

// bind_mesh.cpp
int entity_set_mesh_data(lua_State* L);
int entity_clear_mesh_data(lua_State* L);
int entity_mesh_data(lua_State* L);
int entity_set_mesh(lua_State* L);
int entity_mesh(lua_State* L);
int entity_set_color(lua_State* L);
int entity_color(lua_State* L);
int entity_set_sprite(lua_State* L);
int entity_sprite(lua_State* L);
int entity_set_cast_shadows(lua_State* L);
gameplay::MaterialOverridesComponent& overrides_of(scene::Scene& s, Entity e);
int entity_set_uv_offset(lua_State* L);

// bind_net.cpp
int net_role(lua_State* L);
int net_is_server(lua_State* L);
int net_is_client(lua_State* L);
int net_authority(lua_State* L);
int net_id(lua_State* L);
int net_connected(lua_State* L);
int net_local_player(lua_State* L);
int net_send(lua_State* L);
int net_clients(lua_State* L);
int net_stats(lua_State* L);
int entity_set_net(lua_State* L);
int entity_get_net(lua_State* L);
int entity_has_authority(lua_State* L);
int entity_is_local(lua_State* L);
int entity_owner(lua_State* L);

// bind_world.cpp
int terrain_height(lua_State* L);
int terrain_sample(lua_State* L);
int terrain_raycast(lua_State* L);
int nav_available(lua_State* L);
int nav_find_path(lua_State* L);
int nav_nearest(lua_State* L);
int nav_raycast(lua_State* L);
int nav_random_point(lua_State* L);
int nav_rebuild(lua_State* L);
int entity_set_destination(lua_State* L);
int entity_stop(lua_State* L);
int entity_bb_get(lua_State* L);
int entity_bb_set(lua_State* L);
int entity_blackboard(lua_State* L);
int entity_perception(lua_State* L);
int ai_available(lua_State* L);
int ai_report_noise(lua_State* L);
int entity_play_sequence(lua_State* L);
int entity_stop_sequence(lua_State* L);
int entity_pause_sequence(lua_State* L);
int entity_seek_sequence(lua_State* L);
int entity_sequence_state(lua_State* L);
int sequencer_available(lua_State* L);
int entity_nav_state(lua_State* L);

// bind_audio.cpp
int audio_play(lua_State* L);
int audio_set_volume(lua_State* L);
int audio_volume(lua_State* L);
int audio_set_muted(lua_State* L);
int audio_muted(lua_State* L);
int audio_set_lowpass(lua_State* L);
int audio_lowpass(lua_State* L);
int audio_set_reverb_send(lua_State* L);
int audio_reverb_send(lua_State* L);
int audio_set_reverb_decay(lua_State* L);
int audio_reverb_decay(lua_State* L);
int audio_available(lua_State* L);
int entity_play_audio(lua_State* L);
int entity_stop_audio(lua_State* L);
int entity_pause_audio(lua_State* L);
int entity_audio_playing(lua_State* L);
int entity_audio_occlusion(lua_State* L);
int audio_acoustics(lua_State* L);

// bind_save.cpp
int save_available(lua_State* L);
int save_version(lua_State* L);
int save_write(lua_State* L);
int save_read(lua_State* L);
int save_exists(lua_State* L);
int save_delete(lua_State* L);
int save_list(lua_State* L);
int save_migrate(lua_State* L);
int save_checkpoint(lua_State* L);
Result<Value> read_checkpoint(ScriptHost::Impl& h, scene::Scene& s, std::string_view slot);
int save_restore(lua_State* L);
int scene_find_id(lua_State* L);
Result<Value> read_scene(ScriptHost::Impl& h, std::string_view path);
Status apply_scene(ScriptHost::Impl& h, scene::Scene& s, ScriptHost::Impl::SceneRequest req);
// Behaviours (behaviours.cpp): movement before the physics step, everything else each frame.
void behaviours_fixed_update(ScriptHost::Impl& h, scene::Scene& s, float dt);
void behaviours_update(ScriptHost::Impl& h, scene::Scene& s, float dt);
void behaviours_pause(ScriptHost::Impl& h, scene::Scene& s);
int scene_load(lua_State* L);
int scene_reload(lua_State* L);
int scene_current(lua_State* L);
int scene_params(lua_State* L);
Status apply_checkpoint(ScriptHost::Impl& h, scene::Scene& s, const Value& data);

// bind_bridge.cpp
int bridge_open(lua_State* L);
int bridge_request(lua_State* L);
int bridge_send(lua_State* L);
int bridge_receive(lua_State* L);
int bridge_running(lua_State* L);
int bridge_close(lua_State* L);
int bridge_stats(lua_State* L);
int script_require(lua_State* L);
int plugins_call(lua_State* L);
int plugins_has(lua_State* L);

// script.cpp
void open_victus_libs(lua_State* L);


template <void (physics::PhysicsWorld::*Fn)(Entity, math::Vec3)>
int entity_physics_apply(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* w = physics::world_of(s)) (w->*Fn)(e, check_vec3(L, 2));
    return 0;
}

} // namespace detail

} // namespace victus::script
