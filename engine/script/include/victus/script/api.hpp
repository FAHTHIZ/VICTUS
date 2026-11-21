// SPDX-License-Identifier: Apache-2.0
// Versioned Luau scripting API (ADR-0155).
//
// The script API is everything a game's scripts can name: the globals VICTUS adds to the Luau
// sandbox and their functions, the methods and members of the Entity and Bridge types, and the
// callbacks the engine calls on script instances (`update`, `on_trigger_enter`, ...). It has a
// version MAJOR.MINOR, separate from the engine version:
//
//   - MINOR rises when a symbol is added. Scripts written for 1.2 run on any 1.x host with x >= 2.
//   - MAJOR rises when a symbol is removed or renamed, or a writable Entity member becomes
//     read-only. Scripts for major N do not run on a host of major M != N.
//
// Each released version is frozen as a snapshot under engine/script/api/ (victus_script_api_<v>.json,
// written by `victus script-api --write`). The script tests compare the live bindings with the
// newest snapshot and every pair of snapshots with check_compatibility(), so a binding change that
// would break games fails CI unless the version and snapshot are updated in the same commit.
// A project records the API it was written against in victus.project ("script_api"); hosts refuse
// to run its scripts when the two are incompatible (HostOptions::script_api).
//
// Not covered: argument lists, argument types and the shape of returned tables. Those are kept
// stable by review and the behaviour tests, not by the snapshot.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"

#include <compare>
#include <filesystem>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace victus::script {

struct ApiVersion {
    std::uint32_t major{0}, minor{0};
    friend constexpr auto operator<=>(const ApiVersion&, const ApiVersion&) = default;
};

// The script API this build of the engine provides.
inline constexpr ApiVersion kScriptApiVersion{1, 1};

// "1" or "1.2" (a missing minor is 0).
[[nodiscard]] Result<ApiVersion> parse_api_version(std::string_view text);
[[nodiscard]] std::string to_string(ApiVersion v);

// True when scripts written for `required` run on a host providing `host`.
[[nodiscard]] constexpr bool api_compatible(ApiVersion required, ApiVersion host) noexcept {
    return required.major == host.major && required.minor <= host.minor;
}
// Ok when compatible; otherwise an error telling the developer which side to upgrade.
[[nodiscard]] Status check_api_requirement(ApiVersion required, ApiVersion host = kScriptApiVersion);

// The live API of this build as a snapshot document:
//   {"format": "victus.script_api", "version": "1.0",
//    "globals": {"scene": {"find": "function", ...}, "print": "function", ...},
//    "types": {"Entity": {"methods": [...], "properties": {"position": "rw", "id": "r", ...}},
//              "Bridge": {"methods": [...]}},
//    "callbacks": ["fixed_update", "on_click", ...]}
// Standard Luau libraries are left out (they follow the Luau version, not this API).
[[nodiscard]] Result<json::Value> describe_api();

// The snapshot flattened to symbols: "scene.find", "print", "Entity:get", "Entity.position",
// "Entity.position=" (writable), "Bridge:request", "callback:update".
[[nodiscard]] Result<std::set<std::string>> api_symbols(const json::Value& snapshot);
[[nodiscard]] Result<ApiVersion> snapshot_version(const json::Value& snapshot);

struct ApiDiff {
    std::set<std::string> added, removed;
    [[nodiscard]] bool empty() const noexcept { return added.empty() && removed.empty(); }
};
[[nodiscard]] Result<ApiDiff> diff_api(const json::Value& older, const json::Value& newer);

// Ok when `newer` is a valid successor of `older` under the rules above: removals need a higher
// major, additions a higher minor (or major), and versions never go down.
[[nodiscard]] Status check_compatibility(const json::Value& older, const json::Value& newer);

// File name of the snapshot for `v` ("victus_script_api_1.0.json").
[[nodiscard]] std::string snapshot_file_name(ApiVersion v);

// Checks a folder of snapshots (engine/script/api/): every consecutive pair passes
// check_compatibility, the newest has the version of `live` (describe_api()) and exactly its
// symbols. The error names the symbols that differ and the command that fixes the snapshot.
[[nodiscard]] Status check_snapshot_folder(const std::filesystem::path& dir, const json::Value& live);

} // namespace victus::script
