// SPDX-License-Identifier: Apache-2.0
// Plugin scaffolding (`victus plugins --new`, Studio Plugins panel) and manifest edits.
#include "victus/plugin/plugin.hpp"

#include "victus/core/json.hpp"

#include <format>
#include <system_error>

namespace victus::plugin {
extern const std::string_view kSdkHeader; // generated from victus_plugin.h (CMakeLists.txt)

namespace {

constexpr std::string_view kPluginTemplateC = R"C(/* NAME: a VICTUS native plugin (docs/PLUGINS.md). */
#include "victus/plugin/victus_plugin.h"

static const VictusHostApi* g_host;
static const VictusLogV1* g_log;
static const VictusSystemsV1* g_systems;
static const VictusSceneV1* g_scene;
static const VictusFunctionsV1* g_functions;
static const VictusEditorV1* g_editor; /* NULL outside VICTUS Studio */

/* Runs every frame: spins the entity named "Spinner" around Y, if the scene has one. */
static void spin(void* user, VictusScene* scene, float dt) {
    static float angle;
    float q[4];
    VictusEntity e = g_scene->find_by_name(scene, "Spinner");
    (void)user;
    if (e == VICTUS_NULL_ENTITY) return;
    angle += dt;
    /* Quaternion for `angle` radians about +Y (small-angle-safe without libm). */
    q[0] = 0.0f;
    q[1] = angle / 2.0f - (angle * angle * angle) / 48.0f;
    q[2] = 0.0f;
    q[3] = 1.0f - (angle * angle) / 8.0f;
    if (angle > 1.0f) angle = 0.0f;
    g_scene->set_rotation(scene, e, q);
}

/* Luau: plugins.call("NAME.greet", "world") --> "hello from NAME" */
static int greet(void* user, const char* args_json, VictusCallResult* result) {
    (void)user;
    (void)args_json;
    g_functions->set_result(result, "\"hello from NAME\"");
    return VICTUS_PLUGIN_OK;
}

/* Studio: Plugins > Add Spinner creates the entity `spin` turns (one undo step). */
static int add_spinner(void* user, VictusScene* scene) {
    float at[3] = {0.0f, 1.0f, 0.0f};
    VictusEntity e;
    (void)user;
    if (g_scene->find_by_name(scene, "Spinner") != VICTUS_NULL_ENTITY) {
        g_editor->notify(g_host->host, "the scene already has a Spinner", 0);
        return VICTUS_PLUGIN_OK;
    }
    e = g_scene->create(scene, "Spinner", VICTUS_NULL_ENTITY);
    if (e == VICTUS_NULL_ENTITY) return VICTUS_PLUGIN_ERROR;
    g_scene->set_position(scene, e, at);
    g_editor->notify(g_host->host, "added Spinner: press Play to watch it turn", 0);
    return VICTUS_PLUGIN_OK;
}

static int init(void* user) {
    (void)user;
    if (g_editor && g_editor->add_command(g_host->host, "add_spinner", "Add Spinner", add_spinner, 0) != VICTUS_PLUGIN_OK)
        return VICTUS_PLUGIN_ERROR;
    if (g_systems->add_system(g_host->host, "spin", VICTUS_PHASE_UPDATE, spin, 0) != VICTUS_PLUGIN_OK) return VICTUS_PLUGIN_ERROR;
    if (g_functions->register_function(g_host->host, "greet", greet, 0) != VICTUS_PLUGIN_OK) return VICTUS_PLUGIN_ERROR;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "NAME plugin initialized");
    return VICTUS_PLUGIN_OK;
}

static void shutdown(void* user) {
    (void)user;
    g_log->write(g_host->host, VICTUS_LOG_INFO, "NAME plugin shut down");
}

VICTUS_PLUGIN_EXPORT int victus_plugin_entry(const VictusHostApi* host, VictusPluginDesc* out) {
    if (host->api_major != VICTUS_PLUGIN_API_MAJOR) return VICTUS_PLUGIN_ERROR;
    g_host = host;
    g_log = (const VictusLogV1*)host->get_interface(host->host, VICTUS_LOG_INTERFACE_1);
    g_systems = (const VictusSystemsV1*)host->get_interface(host->host, VICTUS_SYSTEMS_INTERFACE_1);
    g_scene = (const VictusSceneV1*)host->get_interface(host->host, VICTUS_SCENE_INTERFACE_1);
    g_functions = (const VictusFunctionsV1*)host->get_interface(host->host, VICTUS_FUNCTIONS_INTERFACE_1);
    if (!g_log || !g_systems || !g_scene || !g_functions) return VICTUS_PLUGIN_ERROR;
    g_editor = (const VictusEditorV1*)host->get_interface(host->host, VICTUS_EDITOR_INTERFACE_1);
    out->struct_size = sizeof *out;
    out->api_major = VICTUS_PLUGIN_API_MAJOR;
    out->api_minor = VICTUS_PLUGIN_API_MINOR;
    out->user = 0;
    out->init = init;
    out->shutdown = shutdown;
    return VICTUS_PLUGIN_OK;
}
)C";

constexpr std::string_view kPluginTemplateCMake = R"CM(# Builds the NAME plugin library into bin/ next to NAME.vplugin:
#   cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(NAME C)
add_library(NAME MODULE src/NAME.c)
target_include_directories(NAME PRIVATE include)
# "$<1:...>" keeps multi-config generators from adding a Debug/Release subfolder.
set_target_properties(NAME PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>")
if(APPLE)
    set_target_properties(NAME PROPERTIES SUFFIX ".dylib")
endif()
)CM";

constexpr std::string_view kPluginTemplateManifest = R"J({
  "name": "NAME",
  "version": "0.1.0",
  "api": "API",
  "description": "A native VICTUS plugin.",
  "libraries": {
    "linux": "bin/NAME.so",
    "windows": "bin/NAME.dll",
    "macos": "bin/NAME.dylib"
  },
  "dependencies": {},
  "targets": ["player", "editor"],
  "enabled": true
}
)J";

std::string fill_template(std::string_view text, std::string_view name) {
    std::string out(text);
    auto replace_all = [&out](std::string_view key, const std::string& value) {
        for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size()))
            out.replace(at, key.size(), value);
    };
    replace_all("NAME", std::string(name));
    replace_all("\"API\"", std::format("\"{}.{}\"", VICTUS_PLUGIN_API_MAJOR, VICTUS_PLUGIN_API_MINOR));
    return out;
}

} // namespace

Status scaffold(const fs::Path& plugins_dir, std::string_view name) {
    if (auto m = parse_manifest(std::format(R"({{"name":"{}","version":"0.1.0","api":"1.0"}})", name), {}); !m)
        return make_error(ErrorCode::InvalidArgument, "plugin names are 1-64 characters of [a-z0-9_]");
    const fs::Path dir = plugins_dir / std::string(name);
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) return make_error(ErrorCode::AlreadyExists, dir.string() + " already exists");
    std::filesystem::create_directories(dir / "src", ec);
    std::filesystem::create_directories(dir / "include" / "victus" / "plugin", ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot create " + dir.string() + ": " + ec.message());
    const std::string n(name);
    const std::pair<fs::Path, std::string> files[] = {
        {dir / (n + std::string(kManifestExtension)), fill_template(kPluginTemplateManifest, name)},
        {dir / "src" / (n + ".c"), fill_template(kPluginTemplateC, name)},
        {dir / "CMakeLists.txt", fill_template(kPluginTemplateCMake, name)},
        {dir / "include" / "victus" / "plugin" / "victus_plugin.h", std::string(kSdkHeader)},
        {dir / ".gitignore", "build/\nbin/\n"},
    };
    for (const auto& [path, text] : files)
        if (auto st = fs::write_text_atomic(path, text); !st) return st;
    return {};
}

Status set_enabled(const fs::Path& manifest_path, bool enabled) {
    auto text = fs::read_text(manifest_path);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return make_error(ErrorCode::ParseError, std::format("{}: {}", manifest_path.string(), doc.error().message));
    if (!doc->is_object()) return make_error(ErrorCode::ParseError, manifest_path.string() + ": not a JSON object");
    doc->set("enabled", json::Value(enabled));
    return fs::write_text_atomic(manifest_path, json::stringify(*doc) + "\n");
}

} // namespace victus::plugin
