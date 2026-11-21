// SPDX-License-Identifier: Apache-2.0
#include "victus/plugin/plugin.hpp"

#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <set>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace victus;

// Host-side definitions of the opaque C handles.
struct VictusScene {
    scene::Scene* scene;
};
struct VictusCallResult {
    std::string json{"null"};
    std::string error;
    bool failed{false};
};
struct VictusHost {
    plugin::PluginManager::Impl* impl{nullptr};
    std::size_t status{0}; // index into Impl::statuses
    std::string name;
    bool initializing{false};
    std::string error; // set_error during entry/init
    // The table handed to the entry point; plugins keep the pointer, so it lives here (heap,
    // address-stable) for as long as the plugin is loaded.
    VictusHostApi api{};
};

namespace victus::plugin {
namespace {

constexpr std::string_view kCategory = "plugin";

// ---- dynamic libraries ------------------------------------------------------------------

class Library {
public:
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library() { close(); }

    Status open(const fs::Path& path) {
#if defined(_WIN32)
        handle_ = static_cast<void*>(LoadLibraryW(path.wstring().c_str()));
        if (!handle_)
            return make_error(ErrorCode::IoError, std::format("cannot load '{}' (error {})", path.string(),
                                                              static_cast<unsigned long>(GetLastError())));
#else
        // RTLD_LOCAL keeps plugin symbols from interposing on each other or the engine.
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            const char* why = dlerror();
            return make_error(ErrorCode::IoError, std::format("cannot load '{}': {}", path.string(), why ? why : "?"));
        }
#endif
        return {};
    }
    [[nodiscard]] void* symbol(const std::string& name) const {
        if (!handle_) return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name.c_str()));
#else
        return dlsym(handle_, name.c_str());
#endif
    }
    void close() {
        if (!handle_) return;
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

private:
    void* handle_{nullptr};
};

// ---- small helpers ----------------------------------------------------------------------

bool valid_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
bool valid_identifier(std::string_view s) {
    if (s.empty() || s.size() > 128 || (s[0] >= '0' && s[0] <= '9')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
// System and function names: identifiers plus '.', so "spawner.tick" is fine.
bool valid_member_name(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

// Entity <-> C handle. Null (index ~0, generation 0) maps to 0; live handles are never 0.
VictusEntity to_handle(scene::Entity e) noexcept { return e.bits() ^ 0xFFFFFFFFull; }
scene::Entity from_handle(VictusEntity h) noexcept {
    const std::uint64_t bits = h ^ 0xFFFFFFFFull;
    return scene::Entity{static_cast<std::uint32_t>(bits & 0xFFFFFFFFull), static_cast<std::uint32_t>(bits >> 32)};
}

scene::Transform* live_transform(VictusScene* s, VictusEntity h) {
    if (!s || !s->scene) return nullptr;
    const scene::Entity e = from_handle(h);
    if (!s->scene->world().alive(e)) return nullptr;
    return s->scene->world().get<scene::Transform>(e);
}

log::Level to_level(int level) {
    switch (level) {
    case VICTUS_LOG_DEBUG: return log::Level::Debug;
    case VICTUS_LOG_WARN: return log::Level::Warn;
    case VICTUS_LOG_ERROR: return log::Level::Error;
    default: return log::Level::Info;
    }
}

} // namespace

// ---- versions ---------------------------------------------------------------------------

Result<Version> parse_version(std::string_view text) {
    Version v;
    std::uint32_t* parts[3] = {&v.major, &v.minor, &v.patch};
    std::size_t part = 0;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    if (text.empty()) return make_error(ErrorCode::ParseError, "empty version");
    while (true) {
        if (part >= 3) return make_error(ErrorCode::ParseError, std::format("version '{}' has too many parts", text));
        auto [next, ec] = std::from_chars(p, end, *parts[part]);
        if (ec != std::errc{} || next == p)
            return make_error(ErrorCode::ParseError, std::format("invalid version '{}'", text));
        p = next;
        ++part;
        if (p == end) break;
        if (*p != '.') return make_error(ErrorCode::ParseError, std::format("invalid version '{}'", text));
        ++p;
    }
    return v;
}

std::string to_string(const Version& v) { return std::format("{}.{}.{}", v.major, v.minor, v.patch); }

std::string_view platform_key() noexcept {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string_view to_string(State s) noexcept {
    switch (s) {
    case State::Disabled: return "disabled";
    case State::Rejected: return "rejected";
    case State::Failed: return "failed";
    case State::Skipped: return "skipped";
    case State::Loaded: return "loaded";
    case State::Unloaded: return "unloaded";
    }
    return "?";
}

// ---- manifests --------------------------------------------------------------------------

Result<Manifest> parse_manifest(std::string_view json_text, const fs::Path& dir) {
    auto doc = json::parse(json_text, {.max_depth = 16, .max_bytes = 1u << 20});
    if (!doc) return make_error(ErrorCode::ParseError, "manifest is not valid JSON: " + doc.error().message);
    if (!doc->is_object()) return make_error(ErrorCode::ParseError, "manifest must be a JSON object");
    const json::Value& d = *doc;
    Manifest m;
    m.dir = dir;

    const json::Value* name = d.find("name");
    if (!name || !name->is_string() || !valid_name(name->as_string()))
        return make_error(ErrorCode::InvalidArgument, "\"name\" must be 1-64 characters of [a-z0-9_]");
    m.name = name->as_string();

    const json::Value* version = d.find("version");
    if (!version || !version->is_string())
        return make_error(ErrorCode::InvalidArgument, "\"version\" must be a string like \"1.0.0\"");
    auto v = parse_version(version->as_string());
    if (!v) return make_error(ErrorCode::InvalidArgument, "\"version\": " + v.error().message);
    m.version = *v;

    const json::Value* api = d.find("api");
    if (!api || !api->is_string())
        return make_error(ErrorCode::InvalidArgument, "\"api\" must name the plugin API version, e.g. \"1.0\"");
    auto a = parse_version(api->as_string());
    if (!a) return make_error(ErrorCode::InvalidArgument, "\"api\": " + a.error().message);
    m.api = *a;

    m.description = d["description"].as_string_or("");
    if (const json::Value* entry = d.find("entry")) {
        if (!entry->is_string() || !valid_identifier(entry->as_string()))
            return make_error(ErrorCode::InvalidArgument, "\"entry\" must be a C identifier");
        m.entry = entry->as_string();
    }
    if (const json::Value* libs = d.find("libraries")) {
        if (!libs->is_object()) return make_error(ErrorCode::InvalidArgument, "\"libraries\" must be an object");
        for (const auto& [platform, path] : libs->as_object()) {
            if (!path.is_string())
                return make_error(ErrorCode::InvalidArgument, std::format("library for '{}' must be a path", platform));
            auto rel = fs::sanitize_relative(path.as_string());
            if (!rel)
                return make_error(ErrorCode::InvalidArgument,
                                  std::format("library for '{}' must stay inside the plugin directory: {}", platform,
                                              rel.error().message));
            m.libraries[platform] = *rel;
        }
    }
    if (const json::Value* deps = d.find("dependencies")) {
        if (!deps->is_object())
            return make_error(ErrorCode::InvalidArgument,
                              "\"dependencies\" must map plugin names to minimum versions");
        for (const auto& [dep, min] : deps->as_object()) {
            if (!valid_name(dep))
                return make_error(ErrorCode::InvalidArgument, std::format("invalid dependency name '{}'", dep));
            if (dep == m.name) return make_error(ErrorCode::InvalidArgument, "a plugin cannot depend on itself");
            if (!min.is_string())
                return make_error(ErrorCode::InvalidArgument,
                                  std::format("dependency '{}' needs a minimum version string", dep));
            std::string_view text = min.as_string();
            if (text.starts_with(">=")) text.remove_prefix(2);
            auto mv = parse_version(text);
            if (!mv) return make_error(ErrorCode::InvalidArgument, std::format("dependency '{}': {}", dep, mv.error().message));
            m.dependencies.push_back({dep, *mv});
        }
    }
    if (const json::Value* targets = d.find("targets")) {
        if (!targets->is_array()) return make_error(ErrorCode::InvalidArgument, "\"targets\" must be an array");
        m.targets.clear();
        for (const auto& t : targets->as_array()) {
            if (!t.is_string() || (t.as_string() != "player" && t.as_string() != "editor"))
                return make_error(ErrorCode::InvalidArgument, "\"targets\" entries must be \"player\" or \"editor\"");
            m.targets.push_back(t.as_string());
        }
    }
    if (const json::Value* enabled = d.find("enabled")) {
        if (!enabled->is_bool()) return make_error(ErrorCode::InvalidArgument, "\"enabled\" must be true or false");
        m.enabled = enabled->as_bool();
    }
    return m;
}

Result<Manifest> read_manifest(const fs::Path& manifest_path) {
    auto text = fs::read_text(manifest_path, 1u << 20);
    if (!text) return std::move(text).error();
    auto m = parse_manifest(*text, manifest_path.parent_path());
    if (!m) return std::move(m).error();
    m->manifest_path = manifest_path;
    return m;
}

std::vector<Manifest> discover(const fs::Path& plugins_dir, std::vector<DiscoveryIssue>* issues) {
    std::vector<Manifest> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(plugins_dir, ec)) return out;
    std::vector<fs::Path> files;
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir, ec)) {
        if (!entry.is_directory(ec) || entry.path().filename().string().starts_with(".")) continue;
        for (const auto& f : std::filesystem::directory_iterator(entry.path(), ec))
            if (f.is_regular_file(ec) && f.path().extension() == kManifestExtension) files.push_back(f.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        auto m = read_manifest(f);
        if (!m) {
            if (issues) issues->push_back({f, m.error().message});
            continue;
        }
        if (m->name != f.parent_path().filename().string() && issues)
            issues->push_back({f, std::format("plugin '{}' should live in plugins/{}/", m->name, m->name)});
        out.push_back(std::move(*m));
    }
    return out;
}

// ---- dependency resolution --------------------------------------------------------------

ResolveResult resolve(const std::vector<Manifest>& manifests, Version host_api) {
    ResolveResult r;
    std::map<std::string, std::size_t> by_name;
    std::map<std::string, int> name_count;
    for (const auto& m : manifests)
        if (m.enabled) ++name_count[m.name];
    for (std::size_t i = 0; i < manifests.size(); ++i) {
        const Manifest& m = manifests[i];
        if (!m.enabled) continue;
        if (name_count[m.name] > 1) {
            r.rejected[m.name] = "more than one enabled plugin has this name";
            continue;
        }
        if (!api_compatible(m.api, host_api)) {
            r.rejected[m.name] = std::format("needs plugin API {}.{}, host provides {}.{}", m.api.major, m.api.minor,
                                             host_api.major, host_api.minor);
            continue;
        }
        by_name[m.name] = i;
    }
    // Reject plugins with missing, too-old or rejected dependencies until nothing changes.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = by_name.begin(); it != by_name.end();) {
            const Manifest& m = manifests[it->second];
            std::string why;
            for (const auto& dep : m.dependencies) {
                auto found = by_name.find(dep.name);
                if (found == by_name.end()) {
                    why = r.rejected.contains(dep.name)
                              ? std::format("dependency '{}' cannot load", dep.name)
                              : std::format("missing dependency '{}' (>= {})", dep.name, to_string(dep.min_version));
                    break;
                }
                if (manifests[found->second].version < dep.min_version) {
                    why = std::format("dependency '{}' is {}, needs >= {}", dep.name,
                                      to_string(manifests[found->second].version), to_string(dep.min_version));
                    break;
                }
            }
            if (!why.empty()) {
                r.rejected[m.name] = std::move(why);
                it = by_name.erase(it);
                changed = true;
            } else ++it;
        }
    }
    // Kahn's algorithm; std::set keeps ready plugins in name order so the result is stable.
    std::map<std::string, int> pending;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& [name, index] : by_name) {
        pending[name] = static_cast<int>(manifests[index].dependencies.size());
        for (const auto& dep : manifests[index].dependencies) dependents[dep.name].push_back(name);
    }
    std::set<std::string> ready;
    for (const auto& [name, count] : pending)
        if (count == 0) ready.insert(name);
    while (!ready.empty()) {
        const std::string name = *ready.begin();
        ready.erase(ready.begin());
        r.order.push_back(by_name[name]);
        for (const auto& d : dependents[name])
            if (--pending[d] == 0) ready.insert(d);
    }
    std::string cycle;
    for (const auto& [name, count] : pending)
        if (count > 0) cycle += (cycle.empty() ? "" : ", ") + name;
    for (const auto& [name, count] : pending)
        if (count > 0) r.rejected[name] = "dependency cycle among: " + cycle;
    return r;
}

// ---- manager ----------------------------------------------------------------------------

struct PluginManager::Impl {
    struct SystemEntry {
        std::string name; // full "plugin.<plugin>.<name>"
        gameplay::Phase phase{gameplay::Phase::Update};
        VictusSystemFn fn{nullptr};
        void* user{nullptr};
        std::size_t owner{0}; // status index
        bool active{true};
    };
    struct FunctionEntry {
        VictusFunctionFn fn{nullptr};
        void* user{nullptr};
        std::size_t owner{0};
    };
    struct CommandEntry {
        Command info;
        VictusCommandFn fn{nullptr};
        void* user{nullptr};
        std::size_t owner{0};
    };
    struct Live {
        std::unique_ptr<VictusHost> host;
        std::unique_ptr<Library> library;
        VictusPluginDesc desc{};
        bool initialized{false};
    };

    HostOptions options;
    std::vector<PluginStatus> statuses;
    std::vector<Live> live; // init order
    std::vector<std::shared_ptr<SystemEntry>> systems;
    std::map<std::string, FunctionEntry, std::less<>> functions;
    std::vector<CommandEntry> commands; // registration order
    gameplay::Simulation* attached{nullptr};
    std::size_t attached_count{0};
    bool loaded{false};

    // Interface tables (static: they hold only function pointers).
    static const VictusLogV1 kLog;
    static const VictusSystemsV1 kSystems;
    static const VictusSceneV1 kScene;
    static const VictusFunctionsV1 kFunctions;
    static const VictusEditorV1 kEditor;

    static const void* get_interface(VictusHost* host, const char* name) {
        if (!host || !name) return nullptr;
        const std::string_view n(name);
        if (n == VICTUS_LOG_INTERFACE_1) return &kLog;
        if (n == VICTUS_SYSTEMS_INTERFACE_1) return &kSystems;
        if (n == VICTUS_SCENE_INTERFACE_1) return &kScene;
        if (n == VICTUS_FUNCTIONS_INTERFACE_1) return &kFunctions;
        if (n == VICTUS_EDITOR_INTERFACE_1 && host->impl && host->impl->options.target == "editor") return &kEditor;
        return nullptr;
    }
    static void set_host_error(VictusHost* host, const char* message) {
        if (host && message) host->error = message;
    }

    static void log_write(VictusHost* host, int level, const char* message) {
        if (!host || !message) return;
        log::logger().write(to_level(level), "plugin." + host->name, message);
    }

    static int add_system(VictusHost* host, const char* name, int phase, VictusSystemFn fn, void* user) {
        if (!host || !host->initializing || !name || !fn || !valid_member_name(name) || phase < VICTUS_PHASE_PRE_UPDATE ||
            phase > VICTUS_PHASE_POST_UPDATE)
            return VICTUS_PLUGIN_ERROR;
        Impl& impl = *host->impl;
        auto entry = std::make_shared<SystemEntry>();
        entry->name = std::format("plugin.{}.{}", host->name, name);
        for (const auto& s : impl.systems)
            if (s->name == entry->name) return VICTUS_PLUGIN_ERROR;
        entry->phase = static_cast<gameplay::Phase>(phase);
        entry->fn = fn;
        entry->user = user;
        entry->owner = host->status;
        impl.statuses[host->status].systems.push_back(entry->name);
        impl.systems.push_back(std::move(entry));
        return VICTUS_PLUGIN_OK;
    }

    static int register_function(VictusHost* host, const char* name, VictusFunctionFn fn, void* user) {
        if (!host || !host->initializing || !name || !fn || !valid_member_name(name)) return VICTUS_PLUGIN_ERROR;
        Impl& impl = *host->impl;
        std::string full = std::format("{}.{}", host->name, name);
        if (impl.functions.contains(full)) return VICTUS_PLUGIN_ERROR;
        impl.statuses[host->status].functions.push_back(full);
        impl.functions.emplace(std::move(full), FunctionEntry{fn, user, host->status});
        return VICTUS_PLUGIN_OK;
    }
    static int add_command(VictusHost* host, const char* name, const char* label, VictusCommandFn fn, void* user) {
        if (!host || !host->initializing || !name || !fn || !valid_member_name(name)) return VICTUS_PLUGIN_ERROR;
        Impl& impl = *host->impl;
        std::string id = std::format("{}.{}", host->name, name);
        for (const auto& c : impl.commands)
            if (c.info.id == id) return VICTUS_PLUGIN_ERROR;
        std::string text = label && *label ? std::string(label) : std::string(name);
        if (text.size() > 128) text.resize(128);
        impl.statuses[host->status].commands.push_back(id);
        impl.commands.push_back({{std::move(id), std::move(text), host->name}, fn, user, host->status});
        return VICTUS_PLUGIN_OK;
    }
    static void editor_notify(VictusHost* host, const char* message, int is_error) {
        if (!host || !message) return;
        Impl& impl = *host->impl;
        log::logger().write(is_error ? log::Level::Error : log::Level::Info, "plugin." + host->name, message);
        if (impl.options.notify) impl.options.notify(host->name, message, is_error != 0);
    }
    Status run_command(std::string_view id, scene::Scene& scene) {
        const auto it = std::find_if(commands.begin(), commands.end(), [&](const CommandEntry& c) { return c.info.id == id; });
        if (it == commands.end()) return make_error(ErrorCode::NotFound, std::format("no plugin command '{}'", id));
        VictusHost* host = nullptr;
        for (auto& l : live)
            if (l.host->status == it->owner) host = l.host.get();
        if (!host) return make_error(ErrorCode::InvalidArgument, std::format("plugin '{}' is not loaded", it->info.plugin));
        host->error.clear();
        VictusScene vs{&scene};
        if (it->fn(it->user, &vs) != VICTUS_PLUGIN_OK)
            return make_error(ErrorCode::Unknown,
                              std::format("{} failed{}", it->info.label, host->error.empty() ? std::string() : ": " + host->error));
        return {};
    }
    static void set_result(VictusCallResult* result, const char* json_text) {
        if (result && json_text) result->json = json_text;
    }
    static void set_call_error(VictusCallResult* result, const char* message) {
        if (!result) return;
        result->failed = true;
        result->error = message ? message : "error";
    }
    static int call_from_plugin(VictusHost* host, const char* full_name, const char* args, char* buffer, std::size_t cap,
                                std::size_t* out_len) {
        if (!host || !full_name) return VICTUS_PLUGIN_ERROR;
        auto r = host->impl->invoke(full_name, args ? args : "null");
        if (!r) return VICTUS_PLUGIN_ERROR;
        if (out_len) *out_len = r->size();
        if (buffer && cap > 0) {
            const std::size_t n = std::min(cap - 1, r->size());
            std::memcpy(buffer, r->data(), n);
            buffer[n] = '\0';
        }
        return VICTUS_PLUGIN_OK;
    }

    Result<std::string> invoke(std::string_view full_name, std::string_view args) {
        auto it = functions.find(full_name);
        if (it == functions.end())
            return make_error(ErrorCode::NotFound, std::format("no plugin function '{}'", full_name));
        if (!json::parse(args, {.max_depth = 64})) return make_error(ErrorCode::InvalidArgument, "arguments are not valid JSON");
        VictusCallResult result;
        const std::string args_text(args);
        const int rc = it->second.fn(it->second.user, args_text.c_str(), &result);
        if (rc != VICTUS_PLUGIN_OK || result.failed)
            return make_error(ErrorCode::Unknown,
                              std::format("{} failed: {}", full_name, result.error.empty() ? "error" : result.error));
        if (!json::parse(result.json, {.max_depth = 64}))
            return make_error(ErrorCode::ParseError, std::format("{} returned invalid JSON", full_name));
        return std::move(result.json);
    }

    void drop_registrations(std::size_t owner) {
        std::erase_if(systems, [owner](const auto& s) { return s->owner == owner; });
        std::erase_if(functions, [owner](const auto& f) { return f.second.owner == owner; });
        std::erase_if(commands, [owner](const auto& c) { return c.owner == owner; });
        statuses[owner].systems.clear();
        statuses[owner].functions.clear();
        statuses[owner].commands.clear();
    }

    void fail(std::size_t index, State state, std::string why) {
        PluginStatus& st = statuses[index];
        st.state = state;
        st.error = std::move(why);
        VICTUS_LOG_ERROR(kCategory, "plugin '{}' {}: {}", st.name, to_string(state), st.error);
    }

    void load_one(const Manifest& m, std::size_t index) {
        PluginStatus& st = statuses[index];
        for (const auto& dep : m.dependencies) {
            const auto* ds = find_status(dep.name);
            if (!ds || ds->state != State::Loaded) {
                fail(index, State::Skipped, std::format("dependency '{}' did not load", dep.name));
                return;
            }
        }
        auto lib_it = m.libraries.find(std::string(platform_key()));
        if (lib_it == m.libraries.end()) {
            fail(index, State::Failed, std::format("no library for platform '{}'", platform_key()));
            return;
        }
        st.library = m.dir / fs::Path(lib_it->second);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(st.library, ec)) {
            fail(index, State::Failed, std::format("library '{}' not found", st.library.string()));
            return;
        }
        Live l;
        l.library = std::make_unique<Library>();
        if (auto s = l.library->open(st.library); !s) {
            fail(index, State::Failed, s.error().message);
            return;
        }
        void* sym = l.library->symbol(m.entry);
        if (!sym) {
            fail(index, State::Failed, std::format("library does not export '{}'", m.entry));
            return;
        }
        // dlsym/GetProcAddress return object pointers; POSIX and Windows guarantee the conversion.
        static_assert(sizeof(VictusPluginEntryFn) == sizeof(sym));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto entry = reinterpret_cast<VictusPluginEntryFn>(sym);
        l.host = std::make_unique<VictusHost>();
        l.host->impl = this;
        l.host->status = index;
        l.host->name = m.name;
        l.host->api = {sizeof(VictusHostApi), VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR, l.host.get(),
                 &Impl::get_interface, &Impl::set_host_error};
        if (entry(&l.host->api, &l.desc) != VICTUS_PLUGIN_OK) {
            fail(index, State::Failed,
                 "entry point failed" + (l.host->error.empty() ? std::string() : ": " + l.host->error));
            return;
        }
        if (l.desc.struct_size < sizeof(VictusPluginDesc)) {
            fail(index, State::Failed, "plugin descriptor is too small (built against an unknown SDK)");
            return;
        }
        if (!api_compatible({l.desc.api_major, l.desc.api_minor, 0}, host_api_version())) {
            fail(index, State::Failed,
                 std::format("built against plugin API {}.{}, host provides {}.{}", l.desc.api_major, l.desc.api_minor,
                             VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR));
            return;
        }
        l.host->initializing = true;
        const int rc = l.desc.init ? l.desc.init(l.desc.user) : VICTUS_PLUGIN_OK;
        l.host->initializing = false;
        if (rc != VICTUS_PLUGIN_OK) {
            drop_registrations(index);
            fail(index, State::Failed, "init failed" + (l.host->error.empty() ? std::string() : ": " + l.host->error));
            return;
        }
        l.initialized = true;
        st.state = State::Loaded;
        st.error.clear();
        VICTUS_LOG_INFO(kCategory, "loaded plugin '{}' {} ({} systems, {} functions, {} commands)", st.name,
                        to_string(st.version), st.systems.size(), st.functions.size(), st.commands.size());
        live.push_back(std::move(l));
    }

    void load_all(std::vector<Manifest> manifests) {
        if (loaded) {
            VICTUS_LOG_WARN(kCategory, "plugins already loaded; ignoring a second load");
            return;
        }
        loaded = true;
        std::vector<Manifest> active;
        for (auto& m : manifests) {
            const bool for_target = std::find(m.targets.begin(), m.targets.end(), options.target) != m.targets.end();
            if (!m.enabled || !for_target) {
                statuses.push_back({m.name, m.version, State::Disabled,
                                    m.enabled ? std::format("not loaded by the {}", options.target) : "disabled",
                                    {}, {}, {}});
                continue;
            }
            active.push_back(std::move(m));
        }
        ResolveResult r = resolve(active);
        std::vector<std::size_t> index_of(active.size());
        for (std::size_t i = 0; i < active.size(); ++i) {
            index_of[i] = statuses.size();
            statuses.push_back({active[i].name, active[i].version, State::Rejected, {}, {}, {}, {}});
            if (auto it = r.rejected.find(active[i].name); it != r.rejected.end()) fail(index_of[i], State::Rejected, it->second);
        }
        for (std::size_t i : r.order) load_one(active[i], index_of[i]);
    }

    [[nodiscard]] const PluginStatus* find_status(std::string_view name) const noexcept {
        for (const auto& s : statuses)
            if (s.name == name) return &s;
        return nullptr;
    }

    void attach(gameplay::Simulation& sim) {
        if (attached && attached != &sim)
            VICTUS_LOG_WARN(kCategory, "plugin systems attached to a second simulation");
        attached = &sim;
        for (; attached_count < systems.size(); ++attached_count) {
            std::shared_ptr<SystemEntry> s = systems[attached_count];
            sim.add_system(s->name, s->phase, [s](scene::Scene& scene, const gameplay::FrameTime& t) {
                if (!s->active) return; // plugin shut down; its code may be unloaded
                VictusScene vs{&scene};
                s->fn(s->user, &vs, t.dt);
            });
        }
    }

    void shutdown() {
        for (auto& s : systems) s->active = false;
        for (auto it = live.rbegin(); it != live.rend(); ++it) {
            if (it->initialized && it->desc.shutdown) it->desc.shutdown(it->desc.user);
            PluginStatus& st = statuses[it->host->status];
            st.state = State::Unloaded;
            it->library->close();
        }
        live.clear();
        functions.clear();
        commands.clear();
    }
};

const VictusLogV1 PluginManager::Impl::kLog{sizeof(VictusLogV1), &Impl::log_write};
const VictusSystemsV1 PluginManager::Impl::kSystems{sizeof(VictusSystemsV1), &Impl::add_system};
const VictusFunctionsV1 PluginManager::Impl::kFunctions{sizeof(VictusFunctionsV1), &Impl::register_function,
                                                        &Impl::set_result, &Impl::set_call_error,
                                                        &Impl::call_from_plugin};
const VictusEditorV1 PluginManager::Impl::kEditor{sizeof(VictusEditorV1), &Impl::add_command, &Impl::editor_notify};

namespace {
VictusEntity scene_find(VictusScene* s, const char* name) {
    if (!s || !s->scene || !name) return VICTUS_NULL_ENTITY;
    return to_handle(s->scene->find_by_name(name));
}
VictusEntity scene_create(VictusScene* s, const char* name, VictusEntity parent) {
    if (!s || !s->scene) return VICTUS_NULL_ENTITY;
    scene::Entity p = parent == VICTUS_NULL_ENTITY ? scene::kNullEntity : from_handle(parent);
    if (!p.is_null() && !s->scene->world().alive(p)) return VICTUS_NULL_ENTITY;
    return to_handle(s->scene->create(name ? name : "Entity", p));
}
void scene_destroy(VictusScene* s, VictusEntity e) {
    if (!s || !s->scene || e == VICTUS_NULL_ENTITY) return;
    const scene::Entity ent = from_handle(e);
    if (s->scene->world().alive(ent)) s->scene->destroy(ent);
}
int scene_alive(VictusScene* s, VictusEntity e) {
    return s && s->scene && e != VICTUS_NULL_ENTITY && s->scene->world().alive(from_handle(e)) ? 1 : 0;
}
std::size_t scene_count(VictusScene* s) { return s && s->scene ? s->scene->world().alive_count() : 0; }
std::size_t scene_name(VictusScene* s, VictusEntity e, char* buffer, std::size_t cap) {
    if (buffer && cap > 0) buffer[0] = '\0';
    if (!scene_alive(s, e)) return 0;
    const auto* n = s->scene->world().get<scene::Name>(from_handle(e));
    if (!n) return 0;
    if (buffer && cap > 0) {
        const std::size_t len = std::min(cap - 1, n->value.size());
        std::memcpy(buffer, n->value.data(), len);
        buffer[len] = '\0';
    }
    return n->value.size();
}
int get_vec3(const math::Vec3& v, float* out) {
    if (!out) return VICTUS_PLUGIN_ERROR;
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    return VICTUS_PLUGIN_OK;
}
int get_position(VictusScene* s, VictusEntity e, float out[3]) {
    auto* t = live_transform(s, e);
    return t ? get_vec3(t->position, out) : VICTUS_PLUGIN_ERROR;
}
int set_position(VictusScene* s, VictusEntity e, const float v[3]) {
    auto* t = live_transform(s, e);
    if (!t || !v) return VICTUS_PLUGIN_ERROR;
    t->position = {v[0], v[1], v[2]};
    return VICTUS_PLUGIN_OK;
}
int get_rotation(VictusScene* s, VictusEntity e, float out[4]) {
    auto* t = live_transform(s, e);
    if (!t || !out) return VICTUS_PLUGIN_ERROR;
    out[0] = t->rotation.x;
    out[1] = t->rotation.y;
    out[2] = t->rotation.z;
    out[3] = t->rotation.w;
    return VICTUS_PLUGIN_OK;
}
int set_rotation(VictusScene* s, VictusEntity e, const float q[4]) {
    auto* t = live_transform(s, e);
    if (!t || !q) return VICTUS_PLUGIN_ERROR;
    t->rotation = math::Quat{q[0], q[1], q[2], q[3]};
    return VICTUS_PLUGIN_OK;
}
int get_scale(VictusScene* s, VictusEntity e, float out[3]) {
    auto* t = live_transform(s, e);
    return t ? get_vec3(t->scale, out) : VICTUS_PLUGIN_ERROR;
}
int set_scale(VictusScene* s, VictusEntity e, const float v[3]) {
    auto* t = live_transform(s, e);
    if (!t || !v) return VICTUS_PLUGIN_ERROR;
    t->scale = {v[0], v[1], v[2]};
    return VICTUS_PLUGIN_OK;
}
VictusEntity scene_parent(VictusScene* s, VictusEntity e) {
    if (!scene_alive(s, e)) return VICTUS_NULL_ENTITY;
    return to_handle(s->scene->parent(from_handle(e)));
}
} // namespace

const VictusSceneV1 PluginManager::Impl::kScene{sizeof(VictusSceneV1), &scene_find,   &scene_create, &scene_destroy,
                                                &scene_alive,          &scene_count,  &scene_name,   &get_position,
                                                &set_position,         &get_rotation, &set_rotation, &get_scale,
                                                &set_scale,            &scene_parent};

PluginManager::PluginManager(HostOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
}
PluginManager::~PluginManager() { shutdown(); }

void PluginManager::load_directory(const fs::Path& plugins_dir) {
    std::vector<DiscoveryIssue> issues;
    auto manifests = discover(plugins_dir, &issues);
    for (const auto& i : issues) VICTUS_LOG_WARN(kCategory, "{}: {}", i.path.string(), i.message);
    impl_->load_all(std::move(manifests));
}
void PluginManager::load(std::vector<Manifest> manifests) { impl_->load_all(std::move(manifests)); }
void PluginManager::attach(gameplay::Simulation& sim) { impl_->attach(sim); }
Result<std::string> PluginManager::call(std::string_view full_name, std::string_view args_json) {
    return impl_->invoke(full_name, args_json);
}
bool PluginManager::has_function(std::string_view full_name) const { return impl_->functions.contains(full_name); }
std::vector<std::string> PluginManager::function_names() const {
    std::vector<std::string> out;
    for (const auto& [name, f] : impl_->functions) out.push_back(name);
    return out;
}
void PluginManager::shutdown() {
    if (impl_) impl_->shutdown();
}
const std::vector<PluginStatus>& PluginManager::statuses() const noexcept { return impl_->statuses; }
const PluginStatus* PluginManager::status(std::string_view name) const noexcept { return impl_->find_status(name); }
std::vector<Command> PluginManager::commands() const {
    std::vector<Command> out;
    for (const auto& c : impl_->commands) out.push_back(c.info);
    return out;
}
Status PluginManager::run_command(std::string_view id, scene::Scene& scene) { return impl_->run_command(id, scene); }
std::size_t PluginManager::loaded_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(impl_->statuses.begin(), impl_->statuses.end(),
                                                  [](const PluginStatus& s) { return s.state == State::Loaded; }));
}

} // namespace victus::plugin
