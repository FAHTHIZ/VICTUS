// SPDX-License-Identifier: Apache-2.0
// Script bindings: save.* (profiles, checkpoints, migrations) and scene loading (scene.load/reload/find_id).
#include "script_internal.hpp"

namespace victus::script::detail {


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
    h.behaviours.start_doc = req.doc;
    h.behaviours.has_start = true;
    h.behaviours.warned.clear();
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

} // namespace victus::script::detail
