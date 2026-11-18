// SPDX-License-Identifier: Apache-2.0
// Script bindings: net.* and entity networking methods.
#include "script_internal.hpp"

namespace victus::script::detail {


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

} // namespace victus::script::detail
