# VICTUS Studio UX audit (Phase 0)

Dated: **2026-11-18**. Scope: `docs/handoff/STUDIO_UX_AND_WORKSPACE_MATURITY.md`, Phase 0.
Revision audited: `main` at `9c65fd2`.

## What was inspected

Every editor source was read for this audit, not only the specification:

| Area | Files | Notes |
|---|---|---|
| App startup, automation flags | `editor/app/main.cpp` | `--frames/--screenshot/--show/--select/--graph/--host` drive the editor headlessly; the layout lives in `<data>/editor_layout.ini` |
| Editor class, public API | `editor/include/victus/editor/editor.hpp` (966 lines) | every UI action is also a method (tests drive it without input) |
| Launcher, workspace, menus, toolbar, status bar, viewport, hierarchy, inspector, assets, console, input actions, modals, shortcuts | `editor/src/panels.cpp` (1956 lines) | one file holds most panels |
| Project/scene lifecycle, editing, play mode, picking, overlays | `editor/src/editor.cpp` | |
| Command palette, autosave/recovery, selection, undo | `command_palette.cpp`, `autosave.cpp`, `selection.cpp`, `undo.cpp` | |
| Specialised tools | `material_panel.cpp`, `graph_panel.cpp`, `animation_panel.cpp`, `sequencer_panel.cpp`, `terrain_tools.cpp`, `tilemaps.cpp`, `sprite_sheets.cpp`, `ai_debugger.cpp`, `network_panel.cpp`, `profiler_panel.cpp`, `editor_clients.cpp` | |
| Gizmo, thumbnails | `gizmo.cpp`, `gizmo_tool.cpp`, `thumbnails.cpp` | |
| UI layer and theme | `engine/ui/src/ui_context.cpp`, `engine/ui/src/theme.cpp`, `engine/ui/include/victus/ui/ui.hpp` | Dear ImGui (docking branch) through the VICTUS RHI |
| Export and validation used by the CLI | `engine/player/include/victus/player/export.hpp`, `tools/cli/main.cpp` (`cmd_check`, `cmd_export`) | |
| Tests | `tests/editor/editor_tests.cpp` (30 cases), `tests/editor/gizmo_tests.cpp` | GPU-backed, headless, lavapipe |

Not inspected in depth: the internals of the specialised panels' drawing code beyond their entry points and visible controls. Their statuses below are marked **needs validation** where the claim depends on that detail.

## Existing strengths (keep)

- **One state model, API-first.** Every panel action is an `Editor` method, so UI slices can be tested headlessly. This is the right base; no rewrite is justified.
- **Undo is unified for the scene** (`UndoStack` of entity snapshots), with separate, explicit histories for graph and sequence files.
- **Autosave and crash recovery** with a recovery modal; GPU device loss autosaves first.
- **Command palette** (Ctrl+Shift+P / F1) covering menu actions, scenes, assets and entities, with fuzzy ranking.
- **Play-in-editor isolation**, including hosted multiplayer with in-editor clients, capture/replay and a profiler with trace export.
- **Docking** with a default layout and a Reset Layout item.

## Gap matrix

Status labels follow the handoff: **implemented / partial / missing / blocked / needs validation**. Severity: **S1** blocks a beginner workflow, **S2** serious friction, **S3** polish.

### Phase 1: design system and workspace shell

| Requirement | Status | Evidence | Friction / impact | Sev |
|---|---|---|---|---|
| Typography scale | missing | `UiOptions::font_data` is empty in `editor/app/main.cpp`, so the editor uses ImGui's built-in vector font at one size. The engine already embeds Inter (SIL OFL 1.1) for game UI (`engine/runtime/CMakeLists.txt`), but the editor doesn't use it | no hierarchy between titles, labels and hints; the launcher fakes a title with `PushFont(nullptr, 1.8x)` | S2 |
| Colour tokens and semantic colours | partial | `theme.cpp` defines a palette, but editor sources hard-code 38 `ImVec4` colour literals (`accent()` in `panels.cpp` is a second, different orange; error red appears in at least five variants: `1,0.4,0.35`, `1,0.35,0.35`, `1,0.45,0.4`, `1,0.5,0.3`, `1,0.4,0.4`) | inconsistent states; impossible to retheme or contrast-check | S2 |
| Spacing scale, radii | partial | one set of values in `theme.cpp`; panels add ad-hoc `ImVec2(32,24)` and similar | — | S3 |
| Iconography | missing | text tags (`[tex]`, `(cam)`, `> Play`, `[] Stop`, `(o) Record`, `|| Pause`) | toolbar and asset lists are hard to scan; play controls look like menu text | S2 |
| Focus, hover, pressed and disabled states | partial | ImGui defaults plus theme; keyboard navigation is enabled (`NavEnableKeyboard`) | focus ring colour is themed; disabled text contrast not checked | S3 |
| Startup, empty, error, success and background states | partial | Inspector: "Select an entity…"; Recent: "No recent projects yet."; notifications are a transient status-bar line (`notify`) | errors disappear with the status line; empty Assets/Console/Hierarchy have no next action | S2 |
| Dockable panels, reset layout | implemented | `build_default_layout`, View > Reset Layout | — | — |
| Named or saved layouts, workspace presets | missing | one ini file; `KNOWN_LIMITATIONS.md` says "no layout presets" | animation/scripting/debug work needs manual docking each time | S2 |
| Window menu, panel close/reopen | partial | core panels (Hierarchy, Inspector, Assets, Console, Viewport) cannot be closed **or reopened**: they are always drawn, so there is no menu toggle. Tool windows are split between View and Assets menus (Graph Editor and Sequencer live under Assets) | panels are hard to find | S2 |
| Command palette shows shortcuts and categories | partial | `EditorCommand::shortcut` exists; categories are a name prefix ("View: …") | needs validation of coverage | S3 |
| Status bar: project, scene, save state, background work, errors, target | partial | scene path, `*` dirty marker, entity count and fps. There is no project name, error count, background-task indicator or quality/target | errors are easy to miss | S2 |
| Light/dark theme | not planned | one dark theme; the handoff forbids a half-finished switch | — | — |

### Phase 2: onboarding

| Requirement | Status | Evidence | Friction / impact | Sev |
|---|---|---|---|---|
| Create, Open, Recent entry points | implemented | `draw_launcher` | — | — |
| Samples and Learn entry points | missing | the launcher has no sample list or help links; samples exist in `samples/` | beginners do not find the sample games | S2 |
| Template descriptions and character choice | implemented | template list with `description`; Character combo | 2D, 3D, simulation and multiplayer are explained in one line each | S3 |
| Name/location validation and duplicate detection | partial | only an empty-name check; `create_project` errors are shown as raw text. An existing folder, invalid characters and an unwritable location are not reported before Create | recoverable but confusing | S2 |
| Recent projects: last opened, compatibility, missing-project recovery | partial | path and "(missing)"; missing entries are disabled. There is no last-opened time and no Locate… | S3 |
| No account requirement | implemented | the launcher says "offline, no account required" | — | — |
| First-session tour and guided first tasks | missing | none | a new user sees five panels and a scene with no guidance | S1 |
| Searchable help linked to the user guide | missing | none | — | S2 |
| Beginner 2D and 3D path through editor interactions only | partial | templates create playable scenes, but **exporting needs the terminal** (`victus export`) | Phase 6 dependency | S1 |

### Phase 3: scene authoring and inspector

| Requirement | Status | Evidence | Friction / impact | Sev |
|---|---|---|---|---|
| Hierarchy tree, rename, duplicate, delete, reparent, multi-select, drag-drop | implemented | `draw_hierarchy_node` | — | — |
| Hierarchy search and filter | missing | none | large scenes need scrolling to find an entity | S2 |
| Visibility and lock toggles | missing | `MeshRenderer.visible` is only in the inspector; no lock | — | S3 |
| Insertion feedback for drag-drop | partial | the upper quarter inserts before the node; nothing is drawn to show it | users cannot tell child from sibling drop | S3 |
| Categorised, searchable create menu | partial | two differently ordered create lists (Entity menu and Hierarchy "+ Add"), no search, no Terrain in "+ Add" | duplicated, conflicting lists | S2 |
| Entity icons by type | partial | `(cam) ` and `(light) ` text prefixes | — | S3 |
| Inspector: Add Component search and descriptions | partial | popup lists built-in components, then reflected categories as sub-menus; no search and no descriptions | S2 |
| Inspector: reset to default | missing | — | S3 |
| Mixed-value display for multi-edit | missing | multi-edit applies changes but shows the primary's values | S3 |
| Asset fields: browse, preview, clear, reveal | partial | drop target plus clear `x`; no browse picker | drag-and-drop only, which fails on touchpads and keyboard-only use | S2 |

### Phase 4: assets

| Requirement | Status | Evidence | Sev |
|---|---|---|---|
| List/grid views, thumbnails | implemented | `draw_assets`, `ThumbnailCache` | — |
| Search, sort, type filters | missing | records are sorted by path only; no search box | S2 |
| Folder tree | missing | flat list of every record | S2 |
| Import queue, progress, cancel, retry | missing | `reimport_assets()` is synchronous; listed as pending in ROADMAP | S2 |
| Dependency and reference view | missing | — | S2 |
| Rename/move with reference-safe updates | missing | — | S2 |
| Per-asset import settings | partial | textures only (context menu) | S3 |

### Phase 5: visual authoring

Graph Editor (visual scripts, material graphs), Material Editor, Animation window (preview and debugging), Sequencer, Terrain Tools, Tile Palette and AI Debugger exist and have editor tests. Missing per `KNOWN_LIMITATIONS.md`: visual behaviour-tree editor, UI canvas editor, particle editor, audio mixer, graph comments and breakpoints. **Needs validation:** palette search and descriptions inside the Graph Editor.

### Phase 6: play, debug, profile and ship

| Requirement | Status | Evidence | Sev |
|---|---|---|---|
| Play / Pause / Stop, isolated play world, input-focus hint | implemented | toolbar, `focus_viewport_`, "Play mode: edits are temporary." | — |
| Single-step and restart | missing | — | S3 |
| Console severity filters, search, counts, copy | missing | only Clear; messages are a coloured text list | S2 |
| Project validation panel | **missing in the editor** | `victus check` validates scenes, prefabs, materials, shaders, sheets, tile sets, controllers and scripts, but only in `tools/cli/main.cpp`. The editor has no way to run it | S1 |
| Build/export from the editor | **missing** | `player::export_game` exists, but no editor menu calls it | S1 |
| Profiler with measured CPU data | implemented | `profiler_panel.cpp`; the GPU section says "GPU wait", not GPU time | — |

### Phase 7: project-wide quality

| Requirement | Status | Evidence | Sev |
|---|---|---|---|
| Project settings window | missing | only startup scene (File menu), quality (View menu) and input actions (Edit menu) are editable; the name, version and window settings need a JSON edit | S2 |
| Input action editor with listen-to-bind | implemented | `draw_input_actions`, `begin_binding_capture` | — |
| Undo history panel | missing | labels exist (`SceneCommand::label`) | S3 |
| Editor localisation | missing | — | S3 |
| UI scale setting | partial | follows the display scale only; no user zoom | S2 |

## Progress since the audit (2026-11-22)

The matrix above is the Phase 0 audit at `9c65fd2`, kept as written. Slices 1 to 12 (STATUS.md, 2026-11-18 and 2026-11-19) and later ADRs closed most of it. Checked against `editor/src`:

| Area | Closed by | Still open |
|---|---|---|
| Phase 1 shell | Slice 1: tokens in `victus/ui/style.hpp` (all 38 colour literals and the second accent removed), Inter Medium and Bold, about 60 vector icons, toolbar, and a status bar with project, save state and error counts. Slice 3: Window menu that reopens every panel, and five layout presets. Panel state is remembered (2026-11-19) | — |
| Phase 2 onboarding | Slice 2: start screen with New Project, Recent (with Locate...), Samples and Learn, plus live name and folder validation. Slice 12: Getting Started checklist with "Do it for me", searchable Help (F2) and lessons for a first 2D and 3D game. The first S1 (no guided first task) and the second (export needs the terminal) are closed | Last-opened time in Recent |
| Phase 3 scene and inspector | Slice 7: hierarchy search and one categorised create menu. Slice 8: Add Component search with descriptions. Slice 12: asset and object picker for reference fields. Inspector "Make it a..." roles | Visibility and lock toggles, a drop-insertion line, reset-to-default per component, mixed-value display for multi-edit |
| Phase 4 assets | Slice 10: search, type filter and folders. Slice 11: import by drag and drop. Background import queue with cancel and retry (ADR-0189). Find References and reference-safe rename, move and delete (ADR-0175). Trash | A dependency graph view |
| Phase 5 visual authoring | Behaviour Tree Editor (ADR-0191), UI Editor (ADR-0193), Sequencer curves (ADR-0190), Script Debugger (ADR-0106) | See [EDITOR_SPEC.md](../EDITOR_SPEC.md) current state |
| Phase 6 play and ship | Slice 4: Problems panel on `player::validate_project`, which closes an S1. Slice 5: Export Game dialog, which closes an S1. Slice 6: Report a Problem. Slice 9: console filters, search, merged repeats and copy | Single-step and restart. GPU time in the profiler (it still shows GPU wait) |
| Phase 7 project-wide | Slice 12: Project Settings window, Undo History, and Interface Size from 90 % to 175 % (Ctrl+= and Ctrl+-) | Editor localisation |

No S1 item from this audit is still open. The blocked items below are unchanged: no external usability test has been run, and the Windows and macOS editors have never been opened interactively.

## Duplicated or conflicting systems

1. **Two create-entity lists** (Entity menu and Hierarchy "+ Add") with different order and contents (Terrain is missing from "+ Add"). Fix: one shared, categorised table used by both and by the command palette.
2. **Two accent colours** (`theme.cpp` `0.95,0.45,0.16` and `panels.cpp` `accent()` `1.0,0.55,0.1`) and many ad-hoc state colours. Fix: one token header used everywhere.
3. **Validation logic lives in the CLI only** (`cmd_check`). An editor validation panel must call the same code, so the logic has to move into a library.

## Proposed vertical slices (execution order)

1. **Design system:** token header (colours, semantic states, spacing, radii, type scale), Inter as the editor font with a bold heading face, shared widgets (section header, hint, empty state, status pill), and vector icons drawn with `ImDrawList` (no new font or licence dependency). Replace hard-coded colours. Restyle the toolbar with icon play controls.
2. **Window menu and status bar:** a single Window menu that can reopen every panel, including the core ones; workspace presets built from existing panels (General, 2D, 3D, Animation, Scripting, Debug); a status bar with project, scene and save state, an error/warning counter that opens the Console, and the quality tier.
3. **Launcher:** sections for Create, Open, Recent, Samples and Learn; template cards; name and location validation before Create (existing folder, invalid characters); recent projects with last-opened time and Locate for missing ones.
4. **Hierarchy and create menu:** search and filter, a shared categorised create menu, type icons, drop-insertion line, and visibility toggles.
5. **Inspector:** Add Component search with descriptions and categories; asset-field Browse picker.
6. **Console:** severity toggles with counts, text search, copy and export.
7. **Problems panel:** move `victus check` into a library, then show the issues in the editor with click-through.
8. **Export dialog:** File > Export Game… calls `player::export_game` on a worker thread, shows progress, warnings and errors, and can reveal the output folder.
9. **Getting Started:** a checklist panel whose items complete from real editor state (entity created, moved, component added, played, saved, exported), which can be dismissed and restarted from Help.
10. **Assets:** search, type filter and a folder tree.

Each slice gets an editor test, a user-guide update and a screenshot review before its commit.

## Blocked or not measurable here

- **External usability tests (Phase 8)** need people who did not build VICTUS. They cannot be run by an agent and are **blocked on the owner**. No participant results will be invented.
- **Windows and macOS editor sessions** need those machines interactively. CI only builds them.
- **High-DPI behaviour** can be checked only by forcing a scale in tests; real HiDPI displays have not been used.
