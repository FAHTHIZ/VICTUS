# Script API versioning

VICTUS game scripts (Luau, see `engine/script/include/victus/script/script.hpp` for the full API
reference) are written against a **script API version** `MAJOR.MINOR`, separate from the engine
version. Decision record: ADR-0155 in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

## What counts as the API

- Globals VICTUS adds to the sandbox and the functions in them (`scene.find`, `input.action`,
  `save.checkpoint`, `require`, ...). Standard Luau libraries (`math`, `string`, `vector`, ...)
  follow the Luau version and are not included.
- Methods of `Entity` and `Bridge` (`entity:get`, `bridge:request`, ...).
- Members of `Entity` and whether they are writable (`entity.position` read/write,
  `entity.id` read-only).
- Callbacks the engine calls on script instances (`start`, `update`, `on_trigger_enter`, ...).

Not included: argument lists and types, and the fields of returned tables.

## Compatibility rules

| Change | Version change |
|---|---|
| Add a function, library, method, member or callback; make a read-only member writable | MINOR + 1 |
| Remove or rename any of the above; make a writable member read-only | MAJOR + 1, MINOR = 0 |
| Behaviour or bug fix with the same names | none |

A project written for `1.2` runs on engines providing `1.2`, `1.3`, ... but not `1.1` (missing
symbols) or `2.x` (the scripts must be ported).

## In a project

`victus.project` holds `"script_api": "1.0"`. New projects get the engine's current version.
Projects saved before versioning existed have no key and are read as `1.0`. When the engine
cannot run the project's scripts:

- `victus check` reports a project error naming both versions;
- the player, `victus run`/`simulate` and Studio's play mode refuse to start scripts
  (`ScriptHost::create` returns `VersionMismatch`).

`victus info <project>` and `victus version` print the versions.

## Versions

| Version | Added | Snapshot |
|---|---|---|
| 1.0 | First frozen API (ADR-0155) | `victus_script_api_1.0.json` |
| 1.1 | `entity:ragdoll_get_up` (ADR-0158); `entity:set_vehicle_input`, `entity:vehicle_state` (ADR-0166); `entity:ui_value`, `entity:set_ui_value` and the `on_value_changed` and `on_submit` callbacks (ADR-0169); `entity:find_descendant`, `entity:attach` (ADR-0181); `entity:cloth_state`, `entity:cloth_impulse`, `entity:set_wind` (ADR-0205) | `victus_script_api_1.1.json` |

The repository has no release tag yet, so 1.1 was extended in place by four commits after it was first frozen (2c44a8b, 4d0b22b, f3194ce and the ADR-0205 cloth commit). That is allowed while no engine with 1.1 has been released. After the first release, every addition needs a new minor version.

## For engine developers

Each released API version is frozen as `engine/script/api/victus_script_api_<version>.json`.
`victus_script_tests` ("script api: the bindings match the frozen snapshots") fails when the
bindings differ from the newest snapshot, and lists the added (`+`) and removed (`-`) symbols and
the version the change needs. To change the API:

1. Add or change the binding.
2. Raise `kScriptApiVersion` in `engine/script/include/victus/script/api.hpp` as the table above
   says.
3. Run `victus script-api --write engine/script/api` to freeze the new version (older snapshots
   stay; they prove each step followed the rules).
4. `victus script-api --check engine/script/api` (or the test) must pass.

Removing a symbol is a major version: also document the port in the CHANGELOG.
