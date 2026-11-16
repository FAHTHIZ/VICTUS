# VICTUS Studio — Editor Specification

The editor is a first-class product: creators build, inspect, debug and package projects without editing source for ordinary content tasks.

## Workspace
Project launcher/templates/recent projects; dockable panels and saved layouts; command palette/shortcuts; 3D viewport with camera, selection and gizmos; strict separation between authoring world and play-in-editor world.

## Core panels
1. Scene/world hierarchy with search, grouping, visibility and lock.
2. Inspector with component editing, validation, reset and multi-object editing.
3. Asset browser with thumbnails, metadata, tags, filters and dependency graph.
4. Console with severity filters and source navigation.
5. Profiler with CPU/GPU timeline, simulation tick, memory and streaming.
6. Project/platform settings and build configuration.
7. Entity/component diagnostics.
8. Undo/redo history and command feedback.

## Specialized editors (incremental)
Scene/world; material and shader graph; terrain/biome; procedural graph; animation timeline/state machine/retargeting; physics/collision; navigation/crowds; audio mixer/spatial audio; UI layout; cinematic sequencer; world streaming; asset import/cook settings; build/package manager.

## UX/reliability
Keyboard-first workflows; undo/redo for destructive actions; autosave/recovery; background import with progress/cancel/errors; previews never stall UI; accessible/scalable/localization-ready UI; play-in-editor isolates mutations; editor dependencies never ship in game runtime.

## Acceptance
**Alpha:** create/open project, viewport, hierarchy, inspector, gizmos, save/reopen, glTF import, play/pause/stop.
**Beta:** undo/redo, asset browser, materials, logs, profiler, import queue and platform build settings.
**1.0:** stable workflow, recovery, extension points, core editors, tutorials and usability testing.
