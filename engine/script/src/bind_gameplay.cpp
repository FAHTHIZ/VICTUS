// SPDX-License-Identifier: Apache-2.0
// Script bindings for the gameplay framework (ADR-0094): tags, events, timers, attributes,
// effects, abilities and inventories.
#include "script_internal.hpp"

#include "victus/gameplay/framework.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace victus::script::detail {

namespace {

std::string opt_field_string(lua_State* L, int t, const char* key, std::string_view fallback = {}) {
    lua_getfield(L, t, key);
    std::string out = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) : std::string(fallback);
    lua_pop(L, 1);
    return out;
}

float opt_field_number(lua_State* L, int t, const char* key, float fallback) {
    lua_getfield(L, t, key);
    const float out = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return out;
}

void push_entities(lua_State* L, const std::vector<Entity>& list) {
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        push_entity(L, list[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

void push_optional(lua_State* L, const std::optional<float>& v) {
    if (v) lua_pushnumber(L, *v);
    else lua_pushnil(L);
}

std::uint32_t check_count(lua_State* L, int idx) {
    const double n = luaL_optnumber(L, idx, 1);
    if (!(n >= 0) || n > 1e9) luaL_error(L, "count must be between 0 and 1e9");
    return static_cast<std::uint32_t>(n);
}

} // namespace

// ---- tags ----

int entity_has_tag(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, gameplay::has_tag(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

int entity_add_tag(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string_view tag = luaL_checkstring(L, 2);
    if (!gameplay::valid_tag(tag)) luaL_error(L, "add_tag: '%s' is not a valid tag (letters, digits, _ - and dots)", std::string(tag).c_str());
    lua_pushboolean(L, gameplay::add_tag(s, e, tag));
    return 1;
}

int entity_remove_tag(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, gameplay::remove_tag(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

int entity_tags(lua_State* L) {
    auto& s = scene_of(L);
    const auto tags = gameplay::tags_of(s, check_live(L, 1, s));
    lua_createtable(L, static_cast<int>(tags.size()), 0);
    for (std::size_t i = 0; i < tags.size(); ++i) {
        lua_pushlstring(L, tags[i].data(), tags[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int scene_tagged(lua_State* L) {
    push_entities(L, gameplay::find_tagged(scene_of(L), luaL_checkstring(L, 1)));
    return 1;
}

// ---- events ----

// events.emit(name [, payload [, target]]) -> id; the sender is the calling script's entity.
int events_emit(lua_State* L) {
    auto& h = host_of(L);
    auto& s = scene_of(L);
    const std::string_view name = luaL_checkstring(L, 1);
    if (name.empty()) luaL_error(L, "events.emit: empty event name");
    Value payload = lua_isnoneornil(L, 2) ? Value() : to_json(L, 2);
    const Entity target = lua_isnoneornil(L, 3) ? Entity{} : check_live(L, 3, s);
    const Entity sender = s.world().alive(h.current) ? h.current : Entity{};
    lua_pushnumber(L, static_cast<double>(gameplay::emit_event(s, name, std::move(payload), sender, target)));
    return 1;
}

// ---- timers ----

int entity_start_timer(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string_view name = luaL_checkstring(L, 2);
    if (name.empty()) luaL_error(L, "start_timer: empty timer name");
    gameplay::start_timer(s, e, name, static_cast<float>(luaL_checknumber(L, 3)), lua_toboolean(L, 4) != 0);
    return 0;
}

int entity_stop_timer(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, gameplay::stop_timer(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

int entity_pause_timer(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    lua_pushboolean(L, gameplay::pause_timer(s, e, luaL_checkstring(L, 2), lua_isnoneornil(L, 3) || lua_toboolean(L, 3) != 0));
    return 1;
}

int entity_timer_left(lua_State* L) {
    auto& s = scene_of(L);
    push_optional(L, gameplay::timer_remaining(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

// ---- attributes and effects ----

int entity_attribute(lua_State* L) {
    auto& s = scene_of(L);
    push_optional(L, gameplay::attribute(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

int entity_attribute_base(lua_State* L) {
    auto& s = scene_of(L);
    push_optional(L, gameplay::attribute_base(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

int entity_set_attribute(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    gameplay::set_attribute_base(s, e, luaL_checkstring(L, 2), static_cast<float>(luaL_checknumber(L, 3)));
    return 0;
}

int entity_modify_attribute(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    push_optional(L, gameplay::modify_attribute(s, e, luaL_checkstring(L, 2), static_cast<float>(luaL_checknumber(L, 3))));
    return 1;
}

// entity:apply_effect{name, attribute, op = "add"|"multiply"|"override", magnitude, duration,
// period, tag, stacking = "refresh"|"stack"|"independent", max_stacks} [, source] -> id (0 = instant)
int entity_apply_effect(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    luaL_checktype(L, 2, LUA_TTABLE);
    gameplay::EffectSpec fx;
    fx.name = opt_field_string(L, 2, "name");
    fx.attribute = opt_field_string(L, 2, "attribute");
    const std::string op = opt_field_string(L, 2, "op", "add");
    if (op == "add") fx.op = gameplay::ModifierOp::Add;
    else if (op == "multiply") fx.op = gameplay::ModifierOp::Multiply;
    else if (op == "override") fx.op = gameplay::ModifierOp::Override;
    else luaL_error(L, "apply_effect: unknown op '%s' (add, multiply, override)", op.c_str());
    fx.magnitude = opt_field_number(L, 2, "magnitude", 0);
    fx.duration = opt_field_number(L, 2, "duration", 0);
    fx.period = opt_field_number(L, 2, "period", 0);
    fx.grant_tag = opt_field_string(L, 2, "tag");
    if (!fx.grant_tag.empty() && !gameplay::valid_tag(fx.grant_tag)) luaL_error(L, "apply_effect: invalid tag '%s'", fx.grant_tag.c_str());
    const std::string stacking = opt_field_string(L, 2, "stacking", "refresh");
    if (stacking == "refresh") fx.stacking = gameplay::Stacking::Refresh;
    else if (stacking == "stack") fx.stacking = gameplay::Stacking::Stack;
    else if (stacking == "independent") fx.stacking = gameplay::Stacking::Independent;
    else luaL_error(L, "apply_effect: unknown stacking '%s' (refresh, stack, independent)", stacking.c_str());
    fx.max_stacks = static_cast<std::uint32_t>(std::clamp(opt_field_number(L, 2, "max_stacks", 1), 1.0f, 1e6f));
    const Entity source = lua_isnoneornil(L, 3) ? Entity{} : check_live(L, 3, s);
    lua_pushnumber(L, static_cast<double>(gameplay::apply_effect(s, e, fx, source)));
    return 1;
}

// entity:remove_effect(id | name) -> number removed
int entity_remove_effect(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::size_t n = 0;
    if (lua_type(L, 2) == LUA_TNUMBER) n = gameplay::remove_effect(s, e, static_cast<std::uint64_t>(lua_tonumber(L, 2))) ? 1 : 0;
    else n = gameplay::remove_effects(s, e, luaL_checkstring(L, 2));
    lua_pushinteger(L, static_cast<int>(n));
    return 1;
}

// entity:effect(name) -> {remaining, stacks, attribute, tag} or nil
int entity_effect(lua_State* L) {
    auto& s = scene_of(L);
    const auto* fx = gameplay::find_effect(s, check_live(L, 1, s), luaL_checkstring(L, 2));
    if (!fx) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, static_cast<double>(fx->id));
    lua_setfield(L, -2, "id");
    lua_pushnumber(L, fx->remaining);
    lua_setfield(L, -2, "remaining");
    lua_pushinteger(L, static_cast<int>(fx->stacks));
    lua_setfield(L, -2, "stacks");
    lua_pushlstring(L, fx->spec.attribute.data(), fx->spec.attribute.size());
    lua_setfield(L, -2, "attribute");
    lua_pushlstring(L, fx->spec.grant_tag.data(), fx->spec.grant_tag.size());
    lua_setfield(L, -2, "tag");
    return 1;
}

// entity:activate_ability{name, cooldown, cost = {stamina = 20}, requires = "tags", blocked_by = "tags"}
// -> ok, reason ("activated" | "cooldown" | "blocked" | "missing_tags" | "cannot_afford")
int entity_activate_ability(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    luaL_checktype(L, 2, LUA_TTABLE);
    gameplay::AbilitySpec spec;
    spec.name = opt_field_string(L, 2, "name");
    if (spec.name.empty()) luaL_error(L, "activate_ability: the ability needs a name");
    spec.cooldown = opt_field_number(L, 2, "cooldown", 0);
    spec.required_tags = opt_field_string(L, 2, "requires");
    spec.blocked_tags = opt_field_string(L, 2, "blocked_by");
    lua_getfield(L, 2, "cost");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isnumber(L, -1))
                spec.costs.emplace_back(lua_tostring(L, -2), static_cast<float>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    std::sort(spec.costs.begin(), spec.costs.end()); // table order is unspecified
    const gameplay::AbilityResult r = gameplay::activate_ability(s, e, spec);
    lua_pushboolean(L, r == gameplay::AbilityResult::Activated);
    const std::string_view name = gameplay::ability_result_name(r);
    lua_pushlstring(L, name.data(), name.size());
    return 2;
}

int entity_ability_cooldown(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushnumber(L, gameplay::ability_cooldown(s, check_live(L, 1, s), luaL_checkstring(L, 2)));
    return 1;
}

// ---- inventory ----

int entity_add_item(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string_view item = luaL_checkstring(L, 2);
    const std::uint32_t count = check_count(L, 3);
    const auto stack = static_cast<std::uint32_t>(std::max(0.0, luaL_optnumber(L, 4, 0)));
    lua_pushinteger(L, static_cast<int>(gameplay::add_item(s, e, item, count, stack)));
    return 1;
}

int entity_remove_item(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string_view item = luaL_checkstring(L, 2);
    lua_pushinteger(L, static_cast<int>(gameplay::remove_item(s, e, item, check_count(L, 3))));
    return 1;
}

int entity_item_count(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushinteger(L, static_cast<int>(gameplay::item_count(s, check_live(L, 1, s), luaL_checkstring(L, 2))));
    return 1;
}

// entity:items() -> { {item = "coin", count = 12}, ... } in slot order
int entity_items(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* inv = s.world().get<gameplay::InventoryComponent>(e);
    const int n = inv ? static_cast<int>(inv->slots.size()) : 0;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        const auto& slot = inv->slots[static_cast<std::size_t>(i)];
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, slot.item.data(), slot.item.size());
        lua_setfield(L, -2, "item");
        lua_pushinteger(L, static_cast<int>(slot.count));
        lua_setfield(L, -2, "count");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// entity:give_item(other, item, count) -> moved
int entity_give_item(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const Entity to = check_live(L, 2, s);
    const std::string_view item = luaL_checkstring(L, 3);
    lua_pushinteger(L, static_cast<int>(gameplay::transfer_item(s, e, to, item, check_count(L, 4))));
    return 1;
}

} // namespace victus::script::detail
