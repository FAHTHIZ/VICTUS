// SPDX-License-Identifier: Apache-2.0
// Script bindings: audio.* and entity audio methods.
#include "script_internal.hpp"

namespace victus::script::detail {


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


// audio.play(clip, { volume=, pitch=, bus=, position=, priority=, max_instances= }) -> boolean
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
        lua_getfield(L, 2, "priority");
        if (!lua_isnil(L, -1)) o.priority = static_cast<std::int32_t>(std::clamp(luaL_checknumber(L, -1), 0.0, 255.0));
        lua_pop(L, 1);
        lua_getfield(L, 2, "max_instances");
        if (!lua_isnil(L, -1)) o.max_instances = static_cast<std::uint32_t>(std::clamp(luaL_checknumber(L, -1), 0.0, 1024.0));
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

// audio.set_lowpass(bus, hz): 0 turns the bus's low-pass filter off.
int audio_set_lowpass(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    const float hz = static_cast<float>(luaL_checknumber(L, 2));
    if (auto* a = audio_of(L)) a->set_bus_lowpass(b, hz);
    return 0;
}

int audio_lowpass(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Master);
    auto* a = audio_of(L);
    lua_pushnumber(L, a ? a->bus_lowpass(b) : 0.0);
    return 1;
}

// audio.set_reverb_send(bus, amount 0..1); Master has no send.
int audio_set_reverb_send(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Sfx);
    const float v = static_cast<float>(luaL_checknumber(L, 2));
    if (auto* a = audio_of(L)) a->set_bus_reverb_send(b, v);
    return 0;
}

int audio_reverb_send(lua_State* L) {
    const audio::Bus b = check_bus(L, 1, audio::Bus::Sfx);
    auto* a = audio_of(L);
    lua_pushnumber(L, a ? a->bus_reverb_send(b) : 0.0);
    return 1;
}

int audio_set_reverb_decay(lua_State* L) {
    const float v = static_cast<float>(luaL_checknumber(L, 1));
    if (auto* a = audio_of(L)) a->set_reverb_decay(v);
    return 0;
}

int audio_reverb_decay(lua_State* L) {
    auto* a = audio_of(L);
    lua_pushnumber(L, a ? a->reverb_decay() : 0.0);
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

// entity:audio_occlusion() -> { blocked, gain, cutoff } or nil (no voice / occlusion off)
int entity_audio_occlusion(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* a = audio::engine_of(s);
    const auto o = a ? a->occlusion(e) : std::nullopt;
    if (!o) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, o->blocked);
    lua_setfield(L, -2, "blocked");
    lua_pushnumber(L, o->gain);
    lua_setfield(L, -2, "gain");
    lua_pushnumber(L, o->cutoff);
    lua_setfield(L, -2, "cutoff");
    return 1;
}

// audio.acoustics() -> { zone = entity|nil, zone_weight, underwater, decay, rays, occluded } or nil
int audio_acoustics(lua_State* L) {
    auto* a = audio_of(L);
    if (!a) {
        lua_pushnil(L);
        return 1;
    }
    const audio::AcousticState st = a->acoustics();
    lua_createtable(L, 0, 6);
    if (st.zone.is_null()) lua_pushnil(L);
    else push_entity(L, st.zone);
    lua_setfield(L, -2, "zone");
    lua_pushnumber(L, st.zone_weight);
    lua_setfield(L, -2, "zone_weight");
    lua_pushnumber(L, st.underwater);
    lua_setfield(L, -2, "underwater");
    lua_pushnumber(L, st.decay);
    lua_setfield(L, -2, "decay");
    lua_pushnumber(L, st.rays);
    lua_setfield(L, -2, "rays");
    lua_pushnumber(L, st.occluded);
    lua_setfield(L, -2, "occluded");
    return 1;
}

} // namespace victus::script::detail
