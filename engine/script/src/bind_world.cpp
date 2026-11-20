// SPDX-License-Identifier: Apache-2.0
// Script bindings: terrain.*, nav.*, ai.*, sequencer.* and the matching entity methods.
#include "script_internal.hpp"

namespace victus::script::detail {


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
    if (n && (!n->valid() || n->building())) {
        // large levels bake in the background; a smaller build is not charged to the script budget
        const AssetLoadPause pause{L, "nav"};
        (void)n->ensure_built_async(s);
    }
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

} // namespace victus::script::detail
