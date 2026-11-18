// SPDX-License-Identifier: Apache-2.0
// ScriptHost: Luau VM lifetime, script instances, per-frame dispatch and the registration of the
// whole Lua API (open_victus_libs). The bindings themselves live in bind_*.cpp and script_lua.cpp.
#include "script_internal.hpp"

namespace victus::script {

namespace detail {


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
                                {"set_anim_param", entity_set_anim_param},
                                {"anim_param", entity_anim_param},
                                {"anim_trigger", entity_anim_trigger},
                                {"reset_anim_trigger", entity_reset_anim_trigger},
                                {"anim_state", entity_anim_state},
                                {"anim_controller", entity_anim_controller},
                                {"set_anim_layer_weight", entity_set_anim_layer_weight},
                                {"anim_layer_weight", entity_anim_layer_weight},
                                {"morph_targets", entity_morph_targets},
                                {"set_morph_weight", entity_set_morph_weight},
                                {"morph_weight", entity_morph_weight},
                                {"root_motion", entity_root_motion},
                                {"set_ik_target", entity_set_ik_target},
                                {"ik_state", entity_ik_state},
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
                                {"set_text_key", entity_set_text_key},
                                {"scroll_to", entity_scroll_to},
                                {"scroll", entity_scroll},
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
        const luaL_Reg plugin_fns[] = {{"call", plugins_call}, {"has", plugins_has}, {nullptr, nullptr}};
        register_lib(L, "plugins", plugin_fns);
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
                                  {"set_lowpass", audio_set_lowpass}, {"lowpass", audio_lowpass},
                                  {"set_reverb_send", audio_set_reverb_send}, {"reverb_send", audio_reverb_send},
                                  {"set_reverb_decay", audio_set_reverb_decay}, {"reverb_decay", audio_reverb_decay},
                                  {nullptr, nullptr}};
    register_lib(L, "audio", audio_fns);
    const luaL_Reg ui_fns[] = {{"screen_size", ui_screen_size}, {"pointer_over_ui", ui_pointer_over},
                               {"focus", ui_focus},             {"focused", ui_focused},
                               {"click", ui_click},             {nullptr, nullptr}};
    register_lib(L, "ui", ui_fns);
    const luaL_Reg loc_fns[] = {{"text", loc_text},           {"has", loc_has},         {"locale", loc_locale},
                                {"set_locale", loc_set_locale}, {"locales", loc_locales}, {"name", loc_name},
                                {nullptr, nullptr}};
    register_lib(L, "loc", loc_fns);
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

} // namespace detail

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
    if (b->host == &h && !b->alive.expired() && gameui::localization_of(s) == h.localization.get()) return;
    b->host = &h;
    b->alive = h.alive;
    (void)h.loc();
    gameui::bind_localization(s, h.localization);
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

std::size_t ScriptHost::reload_localization() {
    Impl& h = *impl_;
    const std::string keep = h.localization ? h.localization->locale() : std::string();
    h.localization.reset();
    gameui::Localization& l = h.loc();
    if (!keep.empty()) (void)l.set_locale(keep);
    if (h.scene) gameui::bind_localization(*h.scene, h.localization);
    return l.locales().size();
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
