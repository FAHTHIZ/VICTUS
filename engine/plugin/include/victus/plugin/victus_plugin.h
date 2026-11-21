/* SPDX-License-Identifier: Apache-2.0 */
/*
 * VICTUS native plugin SDK: the stable C ABI between the engine and a plugin library.
 *
 * A plugin is a shared library (.so / .dll / .dylib) plus a JSON manifest (`<name>.vplugin`) in
 * the project's `plugins/<name>/` directory. The library exports one function,
 * `victus_plugin_entry` (or the manifest's "entry"), which the host calls once after loading:
 *
 *     VICTUS_PLUGIN_EXPORT int victus_plugin_entry(const VictusHostApi* host, VictusPluginDesc* out);
 *
 * The entry fills `out` (callbacks and the API version the plugin was compiled against) and
 * returns VICTUS_PLUGIN_OK. Engine services are requested by name through
 * host->get_interface("victus.log.1"); each name identifies a frozen struct layout. Compatibility
 * rules (docs/PLUGINS.md, ADR-0078):
 *   - VICTUS_PLUGIN_API_MAJOR changes only for breaking changes; the host loads a plugin only
 *     when the majors match and the plugin's minor is <= the host's minor.
 *   - A minor bump only adds interfaces. A published interface struct never changes: new or
 *     changed functions ship as a new name ("victus.scene.2") and the old one stays served.
 *   - Every struct starts with `struct_size` so the host can tell which fields a plugin knows.
 *
 * Threading: every host function and plugin callback runs on the simulation thread. Strings
 * passed to the plugin are valid only for the duration of the call. Plugins must not throw C++
 * exceptions or longjmp across this boundary.
 */
#ifndef VICTUS_PLUGIN_H
#define VICTUS_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VICTUS_PLUGIN_API_MAJOR 1
#define VICTUS_PLUGIN_API_MINOR 1

#if defined(_WIN32)
#define VICTUS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define VICTUS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define VICTUS_PLUGIN_OK 0
#define VICTUS_PLUGIN_ERROR 1

/* Opaque handles owned by the host. */
typedef struct VictusHost VictusHost;   /* one per loaded plugin; identifies the caller */
typedef struct VictusScene VictusScene; /* the running scene, valid during a system callback */

/* Entity handles are generation-checked 64-bit values; 0 is never a live entity. */
typedef uint64_t VictusEntity;
#define VICTUS_NULL_ENTITY ((VictusEntity)0)

typedef struct VictusHostApi {
    uint32_t struct_size;
    uint32_t api_major;
    uint32_t api_minor;
    VictusHost* host;
    /* Returns the interface struct for `name` (e.g. "victus.log.1") or NULL when unknown. */
    const void* (*get_interface)(VictusHost* host, const char* name);
    /* Records the reason for a failing init; shown in diagnostics and `victus plugins`. */
    void (*set_error)(VictusHost* host, const char* message);
} VictusHostApi;

typedef struct VictusPluginDesc {
    uint32_t struct_size;  /* sizeof(VictusPluginDesc) as compiled by the plugin */
    uint32_t api_major;    /* VICTUS_PLUGIN_API_MAJOR as compiled by the plugin */
    uint32_t api_minor;    /* VICTUS_PLUGIN_API_MINOR as compiled by the plugin */
    void* user;            /* passed back to every callback */
    /* Called once, in dependency order, after every dependency initialized. Register systems
     * and functions here. Return VICTUS_PLUGIN_OK; anything else fails the plugin (and skips
     * the plugins that depend on it). May be NULL. */
    int (*init)(void* user);
    /* Called once, in reverse init order, only when init succeeded. May be NULL. */
    void (*shutdown)(void* user);
} VictusPluginDesc;

typedef int (*VictusPluginEntryFn)(const VictusHostApi* host, VictusPluginDesc* out);

/* ---- "victus.log.1" -------------------------------------------------------------------- */
#define VICTUS_LOG_INTERFACE_1 "victus.log.1"
enum { VICTUS_LOG_DEBUG = 1, VICTUS_LOG_INFO = 2, VICTUS_LOG_WARN = 3, VICTUS_LOG_ERROR = 4 };
typedef struct VictusLogV1 {
    uint32_t struct_size;
    /* Writes to the engine log under the category "plugin.<name>". */
    void (*write)(VictusHost* host, int level, const char* message);
} VictusLogV1;

/* ---- "victus.systems.1" ---------------------------------------------------------------- */
#define VICTUS_SYSTEMS_INTERFACE_1 "victus.systems.1"
/* Simulation phases, in run order each frame (fixed update runs 0..n times at the fixed step). */
enum {
    VICTUS_PHASE_PRE_UPDATE = 0,
    VICTUS_PHASE_FIXED_UPDATE = 1,
    VICTUS_PHASE_UPDATE = 2,
    VICTUS_PHASE_POST_UPDATE = 3
};
typedef void (*VictusSystemFn)(void* user, VictusScene* scene, float dt);
typedef struct VictusSystemsV1 {
    uint32_t struct_size;
    /* Registers a per-frame system named "plugin.<plugin>.<name>". Only valid during init.
     * Returns VICTUS_PLUGIN_OK, or VICTUS_PLUGIN_ERROR for a bad phase/name or a call after init. */
    int (*add_system)(VictusHost* host, const char* name, int phase, VictusSystemFn fn, void* user);
} VictusSystemsV1;

/* ---- "victus.scene.1" ------------------------------------------------------------------ */
#define VICTUS_SCENE_INTERFACE_1 "victus.scene.1"
typedef struct VictusSceneV1 {
    uint32_t struct_size;
    /* First entity with this exact name, or VICTUS_NULL_ENTITY. */
    VictusEntity (*find_by_name)(VictusScene* scene, const char* name);
    /* Creates an entity (with Transform) under `parent` (VICTUS_NULL_ENTITY for the root). */
    VictusEntity (*create)(VictusScene* scene, const char* name, VictusEntity parent);
    /* Destroys the entity and its subtree; ignores dead handles. */
    void (*destroy)(VictusScene* scene, VictusEntity entity);
    int (*alive)(VictusScene* scene, VictusEntity entity);
    size_t (*entity_count)(VictusScene* scene);
    /* Copies the name (NUL-terminated, truncated to cap) and returns its full length. */
    size_t (*get_name)(VictusScene* scene, VictusEntity entity, char* buffer, size_t cap);
    /* Local transform. Getters return VICTUS_PLUGIN_ERROR for a dead entity. */
    int (*get_position)(VictusScene* scene, VictusEntity entity, float out_xyz[3]);
    int (*set_position)(VictusScene* scene, VictusEntity entity, const float xyz[3]);
    int (*get_rotation)(VictusScene* scene, VictusEntity entity, float out_xyzw[4]);
    int (*set_rotation)(VictusScene* scene, VictusEntity entity, const float xyzw[4]);
    int (*get_scale)(VictusScene* scene, VictusEntity entity, float out_xyz[3]);
    int (*set_scale)(VictusScene* scene, VictusEntity entity, const float xyz[3]);
    VictusEntity (*parent)(VictusScene* scene, VictusEntity entity);
} VictusSceneV1;

/* ---- "victus.functions.1" -------------------------------------------------------------- */
/* Named functions callable from Luau (`plugins.call("name", args)`) and from other plugins.
 * Arguments and results are JSON text so they cross the ABI without shared types. */
#define VICTUS_FUNCTIONS_INTERFACE_1 "victus.functions.1"
typedef struct VictusCallResult VictusCallResult; /* opaque, valid during the call */
typedef int (*VictusFunctionFn)(void* user, const char* args_json, VictusCallResult* result);
typedef struct VictusFunctionsV1 {
    uint32_t struct_size;
    /* Registers "<plugin>.<name>". Only valid during init; names are unique per host. */
    int (*register_function)(VictusHost* host, const char* name, VictusFunctionFn fn, void* user);
    /* Sets the call's JSON result (must parse; defaults to null). */
    void (*set_result)(VictusCallResult* result, const char* json);
    /* Fails the call with a message (the function should then return VICTUS_PLUGIN_ERROR). */
    void (*set_error)(VictusCallResult* result, const char* message);
    /* Calls a registered function by full name; the result JSON is copied into `buffer`
     * (truncated to cap) and its full length returned through `out_len`. */
    int (*call)(VictusHost* host, const char* full_name, const char* args_json, char* buffer, size_t cap,
                size_t* out_len);
} VictusFunctionsV1;

/* ---- "victus.editor.1" (API 1.1) -------------------------------------------------------- */
/* Studio commands. Served only when the plugin is loaded by VICTUS Studio (manifest target
 * "editor"); players return NULL, so check the pointer and skip editor features there. */
#define VICTUS_EDITOR_INTERFACE_1 "victus.editor.1"
/* Runs on the scene open in the Studio (never on a Play session). Everything the command changes
 * is recorded as one undo step named after the command. Return VICTUS_PLUGIN_OK, or
 * VICTUS_PLUGIN_ERROR after host->set_error / notify to report why it did nothing. */
typedef int (*VictusCommandFn)(void* user, VictusScene* scene);
typedef struct VictusEditorV1 {
    uint32_t struct_size;
    /* Adds "<plugin>.<name>" to the Studio Plugins menu and command search, shown as `label`
     * (any text, e.g. "Scatter Rocks"). Only valid during init; names are [a-z0-9_]. */
    int (*add_command)(VictusHost* host, const char* name, const char* label, VictusCommandFn fn, void* user);
    /* Shows a Studio status message (is_error: 1 also writes it to the Console as an error). */
    void (*notify)(VictusHost* host, const char* message, int is_error);
} VictusEditorV1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VICTUS_PLUGIN_H */
