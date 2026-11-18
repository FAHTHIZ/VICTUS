// SPDX-License-Identifier: Apache-2.0
// Script bindings: bridge.* helper processes speaking JSON lines, and require().
#include "script_internal.hpp"

namespace victus::script::detail {


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

} // namespace victus::script::detail
