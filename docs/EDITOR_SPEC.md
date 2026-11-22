# VICTUS Studio — Editor Specification

VICTUS Studio is a first-class creation environment. A developer should be able to create, author, debug, test and package ordinary game content without editing engine source.

## Workspace and project management
- Project launcher, templates, samples, recent projects, project settings and version validation.
- Dockable workspace, saved layouts, command palette, keyboard shortcuts and scalable UI.
- Asset/project status, background tasks, diagnostics, autosave and crash recovery.
- Clear editor/runtime/server separation; editor plugins never silently ship in game builds.

## Core authoring panels
1. Scene/world hierarchy with search, grouping, visibility, lock and multi-selection.
2. Inspector with typed component editing, validation, reset, multi-object editing and custom property views.
3. Viewports for 2D and 3D, cameras, selection, transform gizmos, snapping and overlays.
4. Asset browser with thumbnails, metadata, tags, filters, dependency graph and source/cooked status.
5. Console, structured logs, warnings, errors and source navigation.
6. Profiler with CPU/GPU timelines, frame/tick breakdown, memory, allocations, streaming and network statistics.
7. Project/platform settings, input maps, build profiles and package validation.
8. Undo/redo history, command feedback and recovery tools.

## Specialized editors
- 2D: sprite/atlas, tilemap, tileset, 2D physics, 2D animation and camera tools.
- 3D: mesh/scene, terrain, foliage, world partition/streaming and lighting tools.
- Materials/shaders: visual graph, parameter inspector, compile diagnostics and preview.
- Animation: timeline, state machine, blend spaces, retargeting and skeletal preview.
- Gameplay: visual scripting, prefab/variant editor, input mapping, AI behavior/state tools and gameplay debugger.
- Effects: particle/VFX graph, curves, emitters, trails and preview.
- UI: visual layout, anchors, themes, input/accessibility and localization preview.
- Audio: mixer, buses, spatial audio and event preview.
- Cinematics: sequencer, camera rigs, animation/audio/event tracks and render output.
- Procedural: terrain/biome, spline/road, scatter, building and graph-based generation.
- Production: import queue, asset validation, cook/package manager, test runner, crash report viewer and source-control status.
- Multiplayer: server/client launch profiles, replication inspector, latency/loss simulation and network timeline.

## Play-in-editor
- Isolated runtime world with play, pause, single-step, restart and stop.
- Safe transfer of selected authored settings without sharing mutable state.
- Inspect entities, components, physics, AI decisions, events, network ownership and runtime variables.
- Record/replay test sessions and capture regression evidence.
- Multiplayer: Play as Host (Slice 30) runs Play mode as an encrypted listen server with the editor as player 0; Window > Network shows the server key, join command, bandwidth graphs, clients and a per-entity replication table, and the inspector shows the selected entity's live network state. In-editor clients (Slice 31, up to 3) are full client worlds joined over loopback UDP; View or Ctrl+1..4 chooses which one the viewport, panels and keyboard follow.

## Reliability and UX
Keyboard-first workflows; undo/redo for destructive operations; autosave and recovery; asynchronous previews/import; clear cancellation and error recovery; accessibility and localization; project schema migration; no silent destructive conversion.

## Acceptance levels
**Editor alpha:** create/open project, 2D/3D viewport, hierarchy, inspector, gizmos, save/reopen, asset import and play/pause/stop.

**Editor beta:** undo/redo, asset browser, materials, logs, profiler, import queue, specialized editors and platform build profiles.

**Editor 1.0:** stable end-to-end workflows, recovery, extension points, tutorials, sample projects, accessibility/localization checks and usability testing.

## Current state (2026-11-22)

Checked against `editor/src` (Dear ImGui). The detailed UX review is [audits/STUDIO_UX_AUDIT.md](audits/STUDIO_UX_AUDIT.md), and limits are in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

| Spec area | In the Studio | Missing or partial |
|---|---|---|
| Workspace | Launcher with templates and recent projects, docking with saved layouts, command palette, UI size (Bigger/Smaller), autosave and crash recovery, Safe Mode, diagnostics bundles | Rebindable shortcuts |
| Hierarchy and inspector | Multi-selection, prefabs (create, apply, revert, unpack), undo history, add-component search, roles | Multi-object editing is partial (see the UX audit) |
| Viewports | 2D and 3D, gizmos with snapping, overlays for colliders, the navmesh and overdraw | — |
| Asset browser | Thumbnails, reference-safe rename, move and delete (ADR-0175), Trash, import settings, import queue window (ADR-0189), Asset Store | Dependency-graph view, tags |
| Console and Problems | Structured log, Problems panel with jump-to-source | — |
| Profiler | CPU and GPU timings (scripts, render passes, GPU waits), memory and GPU resource counters, pause and trace recording | Per-allocation tracking, a streaming view and a network timeline in the profiler (network stats live in the Network window) |
| 2D | Sprite sheets, tile sets and tilemaps | — |
| 3D | Terrain sculpt, paint and scatter, lighting bake | Foliage painting as its own tool, spline and road tools |
| Materials | Material editor, material graph, surface shaders | — |
| Animation | Clips, state machine and blend-space panel, IK and foot placement, ragdoll tools | Retargeting UI |
| Gameplay | Visual scripts, behaviour-tree editor, AI debugger, script debugger, input actions, vehicle tools | — |
| Effects | Particle components edited in the inspector | Dedicated VFX graph and preview |
| UI | UI editor | Localization preview (reloads at run time only) |
| Audio | Audio components in the inspector | Mixer and bus panel |
| Cinematics | Sequencer panel | Render-to-file output |
| Production | Export dialog with validation, import queue, Report a Problem | Test runner, crash-report viewer, source-control status |
| Multiplayer | Play as Host, up to three in-editor clients, Network window (Slices 30 and 31) | — |
| Play-in-editor | Play, pause, stop, record and replay a capture | Single-step |
| Extensions | Editor plugins with undoable commands (ADR-0192) | — |

Acceptance: alpha is met. Beta is mostly met; the platform build profiles are partial (exports are host-only). 1.0 needs usability testing with real users, which has not happened.
