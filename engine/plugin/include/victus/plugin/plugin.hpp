// SPDX-License-Identifier: Apache-2.0
#pragma once
// Native plugin host: manifest parsing, dependency resolution, versioned C ABI loading and the
// plugin lifecycle (ADR-0078, docs/PLUGINS.md). Plugin authors include only victus_plugin.h.
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/plugin/victus_plugin.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace victus::plugin {

inline constexpr std::string_view kManifestExtension = ".vplugin";
inline constexpr std::string_view kDefaultEntry = "victus_plugin_entry";

struct Version {
    std::uint32_t major{0}, minor{0}, patch{0};
    friend constexpr auto operator<=>(const Version&, const Version&) = default;
};
// "1", "1.2" or "1.2.3" (missing parts are 0; pre-release/build suffixes are rejected).
[[nodiscard]] Result<Version> parse_version(std::string_view text);
[[nodiscard]] std::string to_string(const Version& v);

// True when a plugin requiring `required` (manifest "api") runs on a host offering `host`.
[[nodiscard]] constexpr bool api_compatible(Version required, Version host) noexcept {
    return required.major == host.major && required.minor <= host.minor;
}
[[nodiscard]] constexpr Version host_api_version() noexcept {
    return {VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR, 0};
}

struct Dependency {
    std::string name;
    Version min_version; // the dependency's own "version" must be >= this
};

struct Manifest {
    std::string name;    // [a-z0-9_]+, unique per project; also the directory name
    Version version;
    Version api;         // minimum host plugin API (major must match)
    std::string description;
    std::string entry{kDefaultEntry};
    // Library per platform key ("linux", "windows", "macos", "android"), relative to `dir`.
    std::map<std::string, std::string> libraries;
    std::vector<Dependency> dependencies;
    std::vector<std::string> targets{"player", "editor"}; // hosts that load it
    bool enabled{true};
    fs::Path dir;           // directory holding the manifest
    fs::Path manifest_path; // the .vplugin file
};

// Platform key of the running host ("linux", "windows", "macos", "android").
[[nodiscard]] std::string_view platform_key() noexcept;

// Parses and validates a manifest. `dir` anchors library paths, which must stay inside it.
[[nodiscard]] Result<Manifest> parse_manifest(std::string_view json_text, const fs::Path& dir);
[[nodiscard]] Result<Manifest> read_manifest(const fs::Path& manifest_path);

struct DiscoveryIssue {
    fs::Path path;
    std::string message;
};
// Reads every `<plugins_dir>/<dir>/*.vplugin` (sorted). Invalid manifests become issues.
[[nodiscard]] std::vector<Manifest> discover(const fs::Path& plugins_dir, std::vector<DiscoveryIssue>* issues = nullptr);

struct ResolveResult {
    std::vector<std::size_t> order; // indices into the input, dependencies first
    // name -> reason for each plugin that cannot load (missing/too-old dependency, cycle,
    // duplicate name, incompatible API, dependent of an unloadable plugin).
    std::map<std::string, std::string> rejected;
};
// Topological order (ties broken by name) over enabled manifests compatible with `host_api`.
// Disabled manifests are ignored and do not satisfy dependencies.
[[nodiscard]] ResolveResult resolve(const std::vector<Manifest>& manifests, Version host_api = host_api_version());

enum class State : std::uint8_t {
    Disabled, // manifest disabled or not for this host target
    Rejected, // failed resolution (dependencies, API, duplicate)
    Failed,   // library, entry or init failed
    Skipped,  // a dependency failed to load
    Loaded,   // init succeeded; systems and functions are live
    Unloaded, // shut down
};
[[nodiscard]] std::string_view to_string(State s) noexcept;

struct PluginStatus {
    std::string name;
    Version version;
    State state{State::Disabled};
    std::string error;
    fs::Path library; // resolved library path (empty when none for this platform)
    std::vector<std::string> systems;   // registered system names
    std::vector<std::string> functions; // registered function names
};

struct HostOptions {
    std::string target{"player"}; // matched against Manifest::targets
};

class PluginManager {
public:
    explicit PluginManager(HostOptions options = {});
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Discovers, resolves, loads and initializes every plugin under `plugins_dir`. Per-plugin
    // failures are recorded in statuses() and logged; they never fail the call. A missing
    // directory is fine (no plugins). Can be called once per manager.
    void load_directory(const fs::Path& plugins_dir);
    // Same, for manifests already in memory (tests, editor previews).
    void load(std::vector<Manifest> manifests);

    // Adds every registered plugin system to `sim` as "plugin.<plugin>.<name>".
    void attach(gameplay::Simulation& sim);

    // Calls a registered function ("<plugin>.<name>") with JSON arguments; returns its JSON result.
    [[nodiscard]] Result<std::string> call(std::string_view full_name, std::string_view args_json);
    [[nodiscard]] bool has_function(std::string_view full_name) const;
    [[nodiscard]] std::vector<std::string> function_names() const;

    // Shuts loaded plugins down in reverse init order and unloads their libraries. Idempotent;
    // also run by the destructor.
    void shutdown();

    [[nodiscard]] const std::vector<PluginStatus>& statuses() const noexcept;
    [[nodiscard]] const PluginStatus* status(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t loaded_count() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace victus::plugin
