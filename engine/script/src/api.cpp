// SPDX-License-Identifier: Apache-2.0
// Versioned script API (ADR-0155): version parsing, the live API description and snapshot diffs.
#include "victus/script/api.hpp"

#include "script_internal.hpp"

#include <charconv>
#include <iterator>

namespace victus::script {

namespace {

constexpr std::string_view kSnapshotFormat = "victus.script_api";

Error bad(std::string message) { return make_error(ErrorCode::InvalidArgument, std::move(message)); }

bool parse_part(std::string_view s, std::uint32_t& out) {
    if (s.empty() || s.size() > 6) return false;
    if (s.size() > 1 && s.front() == '0') return false; // no leading zeros
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

const char* lua_kind(int type) {
    switch (type) {
    case LUA_TFUNCTION: return "function";
    case LUA_TTABLE: return "table";
    case LUA_TNUMBER: return "number";
    case LUA_TSTRING: return "string";
    case LUA_TBOOLEAN: return "boolean";
    case LUA_TVECTOR: return "vector";
    case LUA_TUSERDATA: return "userdata";
    default: return "value";
    }
}

// Sorted names of the string keys of the table at `idx`, with the Luau type of each value.
std::vector<std::pair<std::string, std::string>> table_entries(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    std::vector<std::pair<std::string, std::string>> out;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -2), lua_kind(lua_type(L, -1)));
        lua_pop(L, 1);
    }
    std::ranges::sort(out);
    return out;
}

json::Value method_list(lua_State* L, int idx) {
    json::Value a = json::Value::array();
    for (const auto& [name, kind] : table_entries(L, idx))
        if (kind == "function") a.push(name);
    return a;
}

} // namespace

Result<ApiVersion> parse_api_version(std::string_view text) {
    ApiVersion v;
    const auto dot = text.find('.');
    const std::string_view major = text.substr(0, dot);
    if (!parse_part(major, v.major)) return bad(std::format("script API version '{}' is not MAJOR or MAJOR.MINOR", text));
    if (dot != std::string_view::npos && !parse_part(text.substr(dot + 1), v.minor))
        return bad(std::format("script API version '{}' is not MAJOR or MAJOR.MINOR", text));
    if (v.major == 0) return bad("script API major version starts at 1");
    return v;
}

std::string to_string(ApiVersion v) { return std::format("{}.{}", v.major, v.minor); }

Status check_api_requirement(ApiVersion required, ApiVersion host) {
    if (api_compatible(required, host)) return {};
    if (required.major != host.major)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("the project's scripts use script API {} but this engine provides {}; major versions "
                                      "differ, so the scripts must be ported (see docs/SCRIPT_API.md)",
                                      to_string(required), to_string(host)));
    return make_error(ErrorCode::VersionMismatch,
                      std::format("the project's scripts need script API {} but this engine provides {}; use a newer "
                                  "VICTUS build",
                                  to_string(required), to_string(host)));
}

std::string snapshot_file_name(ApiVersion v) { return std::format("victus_script_api_{}.json", to_string(v)); }

Result<json::Value> describe_api() {
    HostOptions options;
    options.watch_files = false;
    auto host = ScriptHost::create(std::move(options));
    if (!host) return std::move(host).error();
    ScriptHost::Impl& h = *(*host)->impl_;
    lua_State* L = h.L;
    lua_rawcheckstack(L, 8);

    json::Value globals = json::Value::object();
    for (const auto& [name, kind] : table_entries(L, LUA_GLOBALSINDEX)) {
        if (std::ranges::find(h.builtin_globals, name) != h.builtin_globals.end()) continue;
        if (name.starts_with("__")) continue; // compiler/tooling hooks (e.g. __vg_trace_for), not for game code
        if (kind != "table") {
            globals.set(name, kind);
            continue;
        }
        lua_getglobal(L, name.c_str());
        json::Value lib = json::Value::object();
        for (const auto& [member, member_kind] : table_entries(L, -1)) lib.set(member, member_kind);
        lua_pop(L, 1);
        globals.set(name, std::move(lib));
    }

    json::Value types = json::Value::object();
    {
        // Entity: methods are upvalue 1 of the __index closure; members come from the property table.
        json::Value entity = json::Value::object();
        lua_getuserdatametatable(L, detail::kEntityTag);
        lua_getfield(L, -1, "__index");
        if (!lua_getupvalue(L, -1, 1)) return make_error(ErrorCode::Unknown, "Entity.__index has no method table");
        entity.set("methods", method_list(L, -1));
        lua_pop(L, 3);
        json::Value props = json::Value::object();
        for (const auto& p : detail::entity_properties()) props.set(p.name, p.writable ? "rw" : "r");
        entity.set("properties", std::move(props));
        types.set("Entity", std::move(entity));
    }
    {
        json::Value bridge = json::Value::object();
        lua_getuserdatametatable(L, detail::kBridgeTag);
        lua_getfield(L, -1, "__index");
        bridge.set("methods", method_list(L, -1));
        lua_pop(L, 2);
        types.set("Bridge", std::move(bridge));
    }

    std::vector<std::string> callbacks(std::begin(detail::kScriptCallbacks), std::end(detail::kScriptCallbacks));
    std::ranges::sort(callbacks);
    json::Value cb = json::Value::array();
    for (auto& c : callbacks) cb.push(std::move(c));

    json::Value doc = json::Value::object();
    doc.set("format", std::string(kSnapshotFormat));
    doc.set("version", to_string(kScriptApiVersion));
    doc.set("globals", std::move(globals));
    doc.set("types", std::move(types));
    doc.set("callbacks", std::move(cb));
    return doc;
}

Result<ApiVersion> snapshot_version(const json::Value& snapshot) {
    if (snapshot["format"].as_string_or("") != kSnapshotFormat) return bad("not a victus.script_api snapshot");
    return parse_api_version(snapshot["version"].as_string_or(""));
}

Result<std::set<std::string>> api_symbols(const json::Value& snapshot) {
    if (auto v = snapshot_version(snapshot); !v) return std::move(v).error();
    std::set<std::string> out;
    const json::Value& globals = snapshot["globals"];
    if (!globals.is_object()) return bad("snapshot has no \"globals\" object");
    for (const auto& [name, value] : globals.as_object()) {
        if (value.is_object()) {
            for (const auto& [member, kind] : value.as_object()) out.insert(name + "." + member);
            out.insert(name); // the library itself (an empty library is still a symbol)
        } else {
            out.insert(name);
        }
    }
    const json::Value& types = snapshot["types"];
    if (!types.is_object()) return bad("snapshot has no \"types\" object");
    for (const auto& [type, desc] : types.as_object()) {
        for (const auto& m : desc["methods"].as_array()) out.insert(type + ":" + m.as_string_or(""));
        if (const json::Value* props = desc.find("properties"); props && props->is_object())
            for (const auto& [name, access] : props->as_object()) {
                out.insert(type + "." + name);
                if (access.as_string_or("") == "rw") out.insert(type + "." + name + "=");
            }
    }
    for (const auto& c : snapshot["callbacks"].as_array()) out.insert("callback:" + c.as_string_or(""));
    return out;
}

Result<ApiDiff> diff_api(const json::Value& older, const json::Value& newer) {
    auto a = api_symbols(older);
    if (!a) return std::move(a).error();
    auto b = api_symbols(newer);
    if (!b) return std::move(b).error();
    ApiDiff d;
    std::ranges::set_difference(*b, *a, std::inserter(d.added, d.added.end()));
    std::ranges::set_difference(*a, *b, std::inserter(d.removed, d.removed.end()));
    return d;
}

Status check_compatibility(const json::Value& older, const json::Value& newer) {
    auto vo = snapshot_version(older);
    if (!vo) return std::move(vo).error();
    auto vn = snapshot_version(newer);
    if (!vn) return std::move(vn).error();
    if (*vn < *vo) return bad(std::format("script API version went down from {} to {}", to_string(*vo), to_string(*vn)));
    auto d = diff_api(older, newer);
    if (!d) return std::move(d).error();
    if (vn->major > vo->major) return {};
    auto list = [](const std::set<std::string>& s) {
        std::string out;
        for (const auto& x : s) out += (out.empty() ? "" : ", ") + x;
        return out;
    };
    if (!d->removed.empty())
        return make_error(ErrorCode::VersionMismatch,
                          std::format("script API {} removes or narrows {} (present in {}); that needs major version {}",
                                      to_string(*vn), list(d->removed), to_string(*vo), vo->major + 1));
    if (!d->added.empty() && vn->minor <= vo->minor)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("script API {} adds {} without raising the minor version above {}", to_string(*vn),
                                      list(d->added), to_string(*vo)));
    if (d->empty() && *vn != *vo)
        return bad(std::format("script API {} has the same symbols as {}; versions only change with the API",
                               to_string(*vn), to_string(*vo)));
    return {};
}

Status check_snapshot_folder(const std::filesystem::path& dir, const json::Value& live) {
    auto live_version = snapshot_version(live);
    if (!live_version) return std::move(live_version).error();
    std::vector<std::pair<ApiVersion, json::Value>> snaps;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("victus_script_api_") || entry.path().extension() != ".json") continue;
        auto text = fs::read_text(entry.path(), 16u << 20);
        if (!text) return std::move(text).error();
        auto doc = json::parse(*text);
        if (!doc) return make_error(ErrorCode::ParseError, name + ": " + doc.error().message);
        auto v = snapshot_version(*doc);
        if (!v) return make_error(v.error().code, name + ": " + v.error().message);
        if (snapshot_file_name(*v) != name) return bad(name + ": file name does not match its version " + to_string(*v));
        snaps.emplace_back(*v, std::move(*doc));
    }
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot list {}: {}", dir.string(), ec.message()));
    std::ranges::sort(snaps, {}, &std::pair<ApiVersion, json::Value>::first);
    for (std::size_t i = 1; i < snaps.size(); ++i)
        if (auto st = check_compatibility(snaps[i - 1].second, snaps[i].second); !st)
            return make_error(st.error().code, std::format("{} -> {}: {}", to_string(snaps[i - 1].first),
                                                           to_string(snaps[i].first), st.error().message));
    const std::string fix = std::format(" (run `victus script-api --write {}` after deciding the version, see docs/SCRIPT_API.md)",
                                        dir.generic_string());
    if (snaps.empty() || snaps.back().first != *live_version) {
        // The live API must be checkable against the last frozen one even before it is written.
        if (!snaps.empty())
            if (auto st = check_compatibility(snaps.back().second, live); !st)
                return make_error(st.error().code, "this build: " + st.error().message + fix);
        return make_error(ErrorCode::NotFound, std::format("no snapshot for script API {}{}", to_string(*live_version), fix));
    }
    auto d = diff_api(snaps.back().second, live);
    if (!d) return std::move(d).error();
    if (!d->empty()) {
        // Tell the developer which version the change needs.
        std::string what;
        for (const auto& x : d->added) what += " +" + x;
        for (const auto& x : d->removed) what += " -" + x;
        const ApiVersion next = d->removed.empty() ? ApiVersion{live_version->major, live_version->minor + 1}
                                                    : ApiVersion{live_version->major + 1, 0};
        return make_error(ErrorCode::VersionMismatch,
                          std::format("the bindings differ from the frozen script API {}:{}; raise kScriptApiVersion to {} "
                                      "in script/api.hpp{}",
                                      to_string(*live_version), what, to_string(next), fix));
    }
    return {};
}

} // namespace victus::script
