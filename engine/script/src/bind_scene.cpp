// SPDX-License-Identifier: Apache-2.0
// Script bindings: scene.*, input.*, time.*, log.* and graph trace bindings.
#include "script_internal.hpp"

namespace victus::script::detail {


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

} // namespace victus::script::detail

namespace victus::script {



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

} // namespace victus::script
