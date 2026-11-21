# Native plugins

VICTUS loads native plugins: shared libraries (`.so`, `.dll`, `.dylib`) that add per-frame
systems and functions to a game without rebuilding the engine. Luau scripts call plugin
functions with `plugins.call`. The design and its reasons are in ADR-0078
(docs/TECHNOLOGY_DECISIONS.md).

Plugins are trusted native code. They run in the game process with full access to the machine,
like the engine itself. Only install plugins you would run as a program. Native plugins are not
a modding system for untrusted content; see "Limitations" below.

## Quick start

```sh
victus plugins MyGame --new hello                   # plugins/hello with a C template
cmake -S MyGame/plugins/hello -B MyGame/plugins/hello/build
cmake --build MyGame/plugins/hello/build --config Release
victus plugins MyGame                               # hello 0.1.0 ready
victus plugins MyGame --load                        # loads it: system plugin.hello.spin, function hello.greet
victus check MyGame                                 # manifest, dependency and library checks
```

In any script:

```lua
if plugins.has("hello.greet") then
    log.info(plugins.call("hello.greet", { who = "world" }))   -- "hello from hello"
end
```

`victus run`, `victus simulate`, `victus serve` and the exported player all load
`<project>/plugins` before the first frame. `victus export` copies each plugin's manifest and the
library for the export platform to `game/plugins/`, next to `game.vpak` (shared libraries cannot
be loaded from inside an archive), and lists them under `"plugins"` in `export.json`.

## Layout

```
MyGame/
  victus.project
  plugins/
    hello/
      hello.vplugin          manifest (required)
      bin/hello.so           built library (path set in the manifest)
      src/hello.c            sources (only the manifest and library are needed at run time)
      include/victus/plugin/victus_plugin.h   the SDK header, copied by --new
      CMakeLists.txt
```

Each plugin lives in `plugins/<name>/`, and its manifest is `<name>.vplugin`. The scaffold puts
`build/` and `bin/` in a `.gitignore`.

## Manifest (`.vplugin`)

```json
{
  "name": "terrain_tools",
  "version": "1.2.0",
  "api": "1.0",
  "description": "Erosion and scatter helpers.",
  "entry": "victus_plugin_entry",
  "libraries": {
    "linux": "bin/terrain_tools.so",
    "windows": "bin/terrain_tools.dll",
    "macos": "bin/terrain_tools.dylib",
    "android": "bin/libterrain_tools.so"
  },
  "dependencies": { "core_utils": ">=1.1" },
  "targets": ["player", "editor"],
  "enabled": true
}
```

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | 1-64 characters of `[a-z0-9_]`, unique in the project; also the directory name and the prefix of the plugin's systems and functions |
| `version` | yes | `major.minor.patch` (missing parts are 0; pre-release suffixes are rejected) |
| `api` | yes | The plugin API version the plugin needs. The engine loads it only when the major matches and the minor is not newer than the engine's |
| `description` | no | Shown by tools |
| `entry` | no | Exported entry function, default `victus_plugin_entry` |
| `libraries` | no | Library path per platform key, relative to the plugin directory; absolute paths and `..` are rejected |
| `dependencies` | no | Plugin name to minimum version (`"1.1"` or `">=1.1"`); dependencies initialize first |
| `targets` | no | Which hosts load it: `"player"` (run, simulate, serve, exported games) and/or `"editor"`; default both |
| `enabled` | no | `false` keeps the plugin installed but unloaded |

## Load order and failures

1. Every `plugins/*/*.vplugin` is read in path order. An invalid manifest is reported and skipped.
2. Disabled plugins and plugins for another target are skipped.
3. Plugins with a duplicate name, an incompatible `api`, a missing or too-old dependency, or a
   dependency cycle are rejected, and so is every plugin that depends on them.
4. The rest load in dependency order (ties by name). For each one the engine loads the library,
   finds the entry, checks the API version the plugin was built against, and calls `init`.
5. If any step fails, the plugin is marked failed, the systems and functions it registered are
   removed, and the plugins that depend on it are skipped. Other plugins still load.
6. At exit, plugins shut down in reverse init order and their libraries are unloaded. Systems a
   plugin added stop running first.

`victus plugins <project> --load` prints each plugin's state (`loaded`, `failed`, `rejected`,
`skipped`, `disabled`) with the reason, for example:

```
crashy                   1.0.0      failed: init failed: missing config file
needs_crashy             1.0.0      skipped: dependency 'crashy' did not load
future                   1.0.0      rejected: needs plugin API 1.4, host provides 1.0
```

## Writing a plugin (C ABI)

Plugins include only `victus/plugin/victus_plugin.h`, a C header with no engine code to link.
They can be written in C, C++ (with `extern "C"` entry points), Rust, Zig or any language that
can export a C function. The engine never passes C++ types across the boundary.

```c
#include "victus/plugin/victus_plugin.h"

static const VictusHostApi* host;
static const VictusSystemsV1* systems;
static const VictusSceneV1* scene_api;

static void bob(void* user, VictusScene* scene, float dt) {
    VictusEntity e = scene_api->find_by_name(scene, "Buoy");
    float p[3];
    if (e && scene_api->get_position(scene, e, p) == VICTUS_PLUGIN_OK) {
        p[1] += dt;
        scene_api->set_position(scene, e, p);
    }
}

static int init(void* user) {
    return systems->add_system(host->host, "bob", VICTUS_PHASE_UPDATE, bob, 0);
}

VICTUS_PLUGIN_EXPORT int victus_plugin_entry(const VictusHostApi* h, VictusPluginDesc* out) {
    if (h->api_major != VICTUS_PLUGIN_API_MAJOR) return VICTUS_PLUGIN_ERROR;
    host = h;
    systems = h->get_interface(h->host, VICTUS_SYSTEMS_INTERFACE_1);
    scene_api = h->get_interface(h->host, VICTUS_SCENE_INTERFACE_1);
    if (!systems || !scene_api) return VICTUS_PLUGIN_ERROR;
    out->struct_size = sizeof *out;
    out->api_major = VICTUS_PLUGIN_API_MAJOR;
    out->api_minor = VICTUS_PLUGIN_API_MINOR;
    out->init = init;
    return VICTUS_PLUGIN_OK;
}
```

### Interfaces (API 1.1)

| Name | Functions |
|---|---|
| `victus.log.1` | `write(host, level, message)`: engine log under `plugin.<name>` |
| `victus.systems.1` | `add_system(host, name, phase, fn, user)`: a per-frame system `plugin.<plugin>.<name>` in PreUpdate, FixedUpdate, Update or PostUpdate; only during `init` |
| `victus.scene.1` | `find_by_name`, `create`, `destroy`, `alive`, `entity_count`, `get_name`, `get/set_position`, `get/set_rotation` (quaternion xyzw), `get/set_scale`, `parent`; valid inside a system callback |
| `victus.editor.1` (API 1.1, editor host only) | `add_command(host, name, label, fn, user)` (only during `init`) adds a Studio command `<plugin>.<name>` shown in the Plugins menu, the Plugins panel and the command palette as `label`; `fn(user, scene)` edits the open scene and returns `VICTUS_PLUGIN_OK` or an error after `host->set_error`. `notify(host, message, is_error)` shows a Studio notification and logs it |
| `victus.functions.1` | `register_function(host, name, fn, user)` (only during `init`) as `<plugin>.<name>`; inside a function, `set_result(result, json)` and `set_error(result, message)`; `call(host, full_name, args_json, buffer, cap, out_len)` to call another plugin's function |

Plugin functions take and return JSON text, so Luau tables, strings, numbers, booleans and nil
cross without shared types. The engine checks that arguments and results are valid JSON.

`host->set_error(host->host, "why")` before returning an error from the entry or `init` puts the
reason in diagnostics.

### Rules

- Everything runs on the simulation thread. Strings and the `VictusScene*` passed to a callback
  are valid only during that call.
- Do not throw C++ exceptions or `longjmp` out of a callback.
- Entity handles are 64-bit values with a generation; `0` is never a live entity. A destroyed
  entity's handle stays dead even if its slot is reused.
- Keep the `VictusHostApi*` pointer; it stays valid until `shutdown` returns.

## Plugins in the Studio

When a project opens (not in safe mode), the Studio reads every manifest under `plugins/` and
loads the ones whose `targets` include `"editor"` in an editor host. Only editor hosts serve
`victus.editor.1`, so the same library can check `get_interface(..., VICTUS_EDITOR_INTERFACE_1)`
and add commands only when it runs inside the Studio:

```c
static const VictusEditorV1* editor; /* NULL in the player */

static int add_marker(void* user, VictusScene* scene) {
    VictusEntity e = scene_api->create(scene, "Marker", 0);
    editor->notify(host->host, "added a marker", 0);
    return e ? VICTUS_PLUGIN_OK : VICTUS_PLUGIN_ERROR;
}

static int init(void* user) {
    if (editor) editor->add_command(host->host, "add_marker", "Add Marker", add_marker, 0);
    return VICTUS_PLUGIN_OK;
}
/* in victus_plugin_entry: editor = h->get_interface(h->host, VICTUS_EDITOR_INTERFACE_1); */
```

- Running a command records the whole scene before and after and adds one undo step
  ("Plugin: <label>") if anything changed; a command that fails leaves no undo step and its
  reason appears as a notification.
- The **Plugins** panel (Window > Tools > Plugins, or the Plugins menu) lists every manifest with
  its Studio state and its state in the last Play session, the reason for any failure, what it
  registered, an Enabled checkbox (rewrites `enabled` in the manifest and reloads), a Run button
  per command, the CMake build command for plugins whose library is not built yet, and a
  New Plugin field that writes the same template as `victus plugins --new`.
- **Reload** rescans `plugins/` and reloads the editor plugins, picking up rebuilt libraries.
  **Unload** releases them; on Windows a loaded `.dll` is locked, so unload before rebuilding.
- **Play** unloads the editor plugins and loads the `"player"` plugins fresh, like `victus run`:
  their systems tick, `plugins.call` reaches their functions, and failures appear as
  notifications. **Stop** unloads them and reloads the editor plugins, so each Play starts from
  a clean `init`.
- Editor plugins' systems do not tick while editing; commands are the way to act on the scene.

## Compatibility promise

- `VICTUS_PLUGIN_API_MAJOR` changes only for a breaking change. A plugin built for API 1.x loads
  on any engine offering 1.y with y >= x.
- A minor version only adds interfaces. A published interface (for example `victus.scene.1`)
  never changes layout or behaviour. New or changed functions ship as a new name
  (`victus.scene.2`) and the old name keeps working.
- Every struct starts with `struct_size`, so the engine can tell which fields a plugin knows.
- The engine checks both the manifest's `api` (before loading the library) and the version the
  library reports it was built against (after loading).

## Limitations

- No sandbox: plugins are native code with full process access. A crash in a plugin crashes the
  game (and writes a crash report, docs/DIAGNOSTICS.md).
- No hot reload: plugins load once at startup; restart to pick up a rebuilt library.
- Reload is manual (Reload in the Plugins panel or Play/Stop); the Studio does not watch the
  library for changes, and state inside a plugin does not survive a reload.
- Editor commands receive the whole scene but not the selection, and cannot add panels,
  inspectors or viewport drawing.
- No custom components, renderer hooks, asset importers or physics callbacks through the C ABI
  yet; plugins change transforms, create and destroy entities, log, and serve functions.
- `victus export` copies libraries for the host platform only and does not sign them.
- Android: the library must be packaged into the APK by the app build; this path is untested.
