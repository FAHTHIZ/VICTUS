# VICTUS Studio — Workspace, UX and Creator-Experience Maturity Handoff

**Priority:** Critical — the editor is the product most creators experience  
**Status:** Owner-approved for implementation  
**Target:** Direct commits to `main`; no branch or PR  
**Relationship:** Complements `COMPETITIVE_MATURITY_GAP_CLOSURE.md`; does not replace it.

## Mission

Make VICTUS Studio feel like a polished, coherent, learnable game-creation environment. A beginner must be able to create a project, understand the workspace, build a scene, import and organize assets, add gameplay, test it, diagnose problems and package a playable build without being expected to write C++ or manually edit project files for ordinary tasks.

C++ remains an optional advanced extension path. The normal creator workflow should be visual, discoverable, safe and reversible. Luau and visual scripting remain available for gameplay logic, with clear guidance on when each is appropriate.

The goal is not to copy the appearance of Unity, Unreal or Godot. Study their proven workflows and independent alternatives, then build a distinctive VICTUS-owned design system and interaction model.

**Important:** This brief is a product-level audit and implementation contract, not evidence that every listed gap is absent. The repository already contains meaningful editor features. Audit current `main`, classify every requirement as implemented/partial/missing/blocked, and improve or extend the existing implementation. Do not duplicate panels, systems, commands or state models.

## Product principles

1. **Creator-first:** ordinary game creation must not require C++, a terminal, JSON editing or knowledge of engine internals.
2. **Progressive disclosure:** beginners see clear, guided defaults; advanced controls remain available without overwhelming the first session.
3. **Discoverability:** visible labels, tooltips, contextual help, searchable commands, sensible empty states and clear feedback.
4. **Consistency:** one design system, navigation model, selection model, property-editing model, undo model and shortcut vocabulary across the editor.
5. **Visual quality with purpose:** refined typography, spacing, hierarchy, iconography, contrast, restrained colour, responsive layouts and meaningful motion. Avoid decorative clutter and oversized panels.
6. **Fast and dependable:** asynchronous expensive work, cancellable operations, responsive UI, predictable state, recovery and actionable errors.
7. **Safe editing:** undo/redo, confirmation for destructive operations, non-destructive import/reimport, backups and explicit migration.
8. **Real capabilities only:** every visible control must work end-to-end or be clearly marked unavailable; no fake panels or dead buttons.
9. **2D and 3D are first-class:** the workspace adapts to the task without treating 2D as a reduced 3D mode.
10. **Evidence before claims:** use task-based tests, real projects, clean checkouts and external beginner feedback.

## Phase 0 — audit the actual editor before changing it

Read `AGENTS.md`, `ROADMAP.md`, `docs/STATUS.md`, `docs/KNOWN_LIMITATIONS.md`, `docs/EDITOR_SPEC.md`, `docs/USER_GUIDE.md`, architecture/ADRs and all current editor sources/tests.

Inspect the complete editor implementation, not only its specification:
- editor app startup, project launcher and window/workspace construction;
- menus, toolbars, command palette, panels, dock/layout state and shortcuts;
- scene hierarchy, selection, viewport, gizmos, inspector and component registry;
- asset table/browser, thumbnail generation/cache, import/reimport, metadata and dependency tracking;
- project settings, input actions, build/export and platform configuration;
- visual graph, material, animation, sequencer, terrain, tilemap, AI and debug tools;
- undo/redo command model, autosave, crash recovery, serialization and migration;
- logs, diagnostics, profiler, test/capture and play-in-editor lifecycle;
- theme/style resources, fonts, icons, scaling, accessibility and localization;
- tests, screenshots/golden captures, sample projects and CI coverage.

Produce `docs/audits/STUDIO_UX_AUDIT.md` with:
- actual files/classes and existing capabilities;
- status for each requirement: **implemented / partial / missing / blocked / needs validation**;
- exact workflow friction and reproduction steps;
- duplicated or conflicting interaction/state systems;
- severity, user impact, dependencies and proposed small vertical slices;
- baseline screenshots/captures where the current environment supports them.

Do not call the whole codebase audited unless it has actually been inspected. Do not overwrite owner work or remove functioning features without tracing all consumers.

## Phase 1 — visual design system and workspace shell

Audit the existing ImGui-based UI and preserve the current architecture unless a measured limitation justifies change.

Deliver a coherent VICTUS Studio design system:
- typography scale, spacing scale, layout grid, icon style, colour tokens, surfaces, borders, elevation, focus/hover/pressed/disabled states;
- consistent controls: buttons, segmented controls, tabs, menus, search, fields, sliders, colour pickers, dropdowns, lists, tables, tree rows, tooltips, dialogs and notifications;
- readable hierarchy and density at common desktop resolutions and high-DPI scaling;
- light/dark themes only if both can be implemented and tested consistently; do not add a half-finished theme switch;
- keyboard focus visibility, contrast checks, scalable UI and reduced-motion behaviour where supported;
- polished startup, loading, empty, error, success and background-task states.

Workspace:
- dockable/resizable panels with saveable named layouts and reliable reset-to-default;
- sensible default layout for first-time users; workspace presets for General, 2D, 3D, Animation, UI, Scripting/Visual Scripting, Audio and Debug/Profiling only where their panels actually exist;
- predictable tabs, panel search, close/reopen, panel focus and layout persistence;
- command palette covers all discoverable editor actions and displays shortcuts/categories;
- menus and toolbar use consistent naming and do not hide essential operations in obscure locations;
- status bar communicates project, active scene, save state, background work, errors and target platform without noise;
- layout survives resolution changes and panel resizing without clipping or inaccessible controls.

Acceptance: restart the editor and verify layouts, shortcuts, focus navigation, high-DPI scaling and panel recovery. No decorative-only controls.

## Phase 2 — beginner onboarding and guided creation

Project launcher / first-run:
- clear Create, Open, Recent, Samples and Learn entry points;
- guided project creation with template descriptions, preview, target platform and optional starter character/content;
- explain 2D, 3D, simulation and multiplayer templates in plain language;
- safe project naming/location validation, duplicate detection, version compatibility and recoverable creation failures;
- recent projects show path, last opened, engine compatibility and missing/moved project recovery;
- no account/sign-in requirement for local project creation.

First-session onboarding:
- short optional interactive tour of viewport, hierarchy, inspector, assets, play/stop and save;
- contextual hints that disappear when completed and can be restarted;
- guided first tasks: create a scene, add an object, move it, add a component, assign an asset, press Play, save and export;
- searchable help and links to the relevant user-guide section;
- useful empty states with one clear next action;
- beginner-friendly terminology first, advanced/internal terminology in tooltips or advanced sections;
- never block experienced users behind a mandatory tutorial.

Provide at least one beginner path that creates a small playable 2D scene and one that creates a small playable 3D scene entirely through supported editor interactions. Record steps and automate what can be automated.

## Phase 3 — scene authoring and inspector coherence

Hierarchy:
- clear scene/entity tree with search, filtering, grouping, visibility, lock, rename, duplicate, delete, parent/reparent and multi-selection;
- drag/drop reparenting with clear insertion feedback and cycle prevention;
- create common objects from categorized, searchable menus with meaningful names and defaults;
- clear unsaved/modified state and missing-reference indicators;
- context menu actions consistent with toolbar and command palette.

Viewport:
- intuitive select/move/rotate/scale tools, local/world modes, snapping, frame selection, focus, camera presets and 2D/3D navigation;
- discoverable controls and an optional viewport navigation overlay;
- safe selection/picking, grid/axis/gizmo clarity, useful overlays and non-blocking notifications;
- viewport performance and input focus must remain predictable when switching panels or playing.

Inspector:
- categories and searchable components/properties;
- Add Component menu with search, descriptions, categories and compatibility filtering;
- typed editors with units, ranges, validation, reset-to-default, tooltips and inline errors;
- multi-object editing with clear mixed-value states;
- asset fields support browse, drag/drop, preview, clear and reveal;
- advanced fields collapsed by default where appropriate;
- component dependencies and conflicts explained before changes;
- changes participate in undo/redo and save/reload.

Common workflows must not require users to manually type C++ or hand-edit JSON.

## Phase 4 — asset library and content pipeline UX

Turn the current Assets table/browser into a reliable creator-facing asset workspace, building on existing thumbnails and drag/drop.

Required audit and improvements:
- tree/grid/list views, thumbnails, search, sort, filters, tags/collections/favourites and file-type grouping;
- clear asset type, source path, stable ID, import status, cooked status, size, dimensions/duration and platform variants where relevant;
- preview/inspection for supported textures, sprites, models, materials, prefabs, audio, animation, fonts, tile sets, shaders and graphs;
- import queue with progress, cancellation, retry, per-file errors and batch operations;
- import settings per asset with safe defaults, validation and reset;
- reimport must preserve stable IDs and references, show what changed, and keep the previous valid cooked asset if new import fails;
- duplicate detection and missing/moved asset recovery;
- dependency/reference view: what uses this asset, what this asset depends on, and impact before deletion/replacement;
- reveal in filesystem, copy asset path/ID, rename/move with reference-safe updates;
- platform-specific import/cook variants and clear size/memory estimates where measurable;
- asset validation reports with actionable fixes;
- offline-first local asset library; no external marketplace or remote asset downloading until license provenance, trust, security scanning and attribution workflows are designed.

Acceptance: import, inspect, edit settings, reimport, move/rename, recover missing references, use in scene, save/reopen and export representative assets. Test corrupt/unsupported files and cancellation.

## Phase 5 — visual authoring instead of code-first workflows

Make existing systems discoverable and coherent:
- visual scripting: beginner-friendly node categories, templates, searchable palette, contextual descriptions, typed connections, validation, comments/frames, reroute nodes, graph navigation, error highlighting, breakpoint/step/variable inspection where runtime supports it;
- script workflow: create/attach/open Luau scripts from the editor, starter templates, API autocomplete/documentation if feasible, error line navigation and safe hot reload; explain that C++ is advanced engine/plugin development, not ordinary gameplay activation;
- materials: live preview, clear parameter editing, texture assignment, graph diagnostics and reusable material instances;
- animation: clip/rig preview, state-machine authoring, blend-space visualization, transition inspection and clear controller debugging; do not advertise unsupported IK/retargeting/root-motion capabilities;
- 2D: sprite/atlas, tilemap/tileset, 2D physics and animation workflows;
- 3D: model/material/lighting, terrain, foliage, prefab and camera workflows;
- UI: visual layout and live preview with anchors, layout, themes, navigation/focus and localization preview; where current engine support is insufficient, record and implement the prerequisite before presenting a complete editor;
- audio: mixer/bus and event preview where supported;
- AI/navigation: inspect behavior tree, blackboard, perception and paths using the actual runtime data;
- sequencer/cinematics: clear timeline controls, track/key editing and safe preview/restore.

Do not add a separate competing graph, animation or editor architecture. Reuse shared undo, serialization, validation, diagnostics and preview infrastructure.

## Phase 6 — play, debug, profile and ship workflows

Play-in-editor:
- clear Edit/Play state, play/pause/step/restart/stop, game-vs-editor input focus and current target;
- explain that runtime changes are isolated or discarded on stop where applicable;
- inspect live entities/components/scripts/physics/AI/network state without accidentally editing authored content;
- capture/replay workflow is discoverable and reports deterministic limits.

Debugging:
- searchable structured console with severity/category filters, timestamps, source navigation, copy/export and clear;
- actionable errors with probable cause, exact asset/entity/property and next step;
- validation panel groups project, scene, script, asset and export issues; click through to fix;
- no silent failure, swallowed exceptions or unexplained red error counts.

Profiling:
- CPU/GPU/frame/tick/memory/asset/network views use measured data and explain units;
- capture, compare and export traces; identify expensive systems and entities when supported;
- distinguish GPU wait from measured GPU execution time; never label estimates as hardware measurements.

Build/export:
- guided platform target and configuration selection;
- preflight validation, missing asset/SDK/platform warnings and clear prerequisites;
- progress, cancellation where safe, logs and actionable failure reports;
- output folder reveal and reproducible build metadata;
- distinguish editor-supported target, CI-built target and physically validated target.

## Phase 7 — project-wide quality and trust

- project settings organized by task, with descriptions, validation and safe defaults;
- input action editor with listen-to-bind workflow and clear device mapping;
- undo/redo history panel with human-readable operations and safe selective navigation only if command model supports it;
- autosave/recovery controls and visible recovery status; test crash/reopen;
- project schema migrations with backups, preview and no silent destructive conversion;
- source-control status and safe diffs/merge support only after repository state handling is reliable;
- localization for the editor itself, not only in-game strings; audit RTL/text shaping and font coverage;
- accessibility: keyboard-only completion of core tasks, focus order, screen-reader-friendly labels where supported, contrast, scaling and reduced motion;
- privacy-aware diagnostics export and crash reporting;
- plugin/extension UX only after a stable versioned plugin API, permissions and failure isolation exist.

## Phase 8 — validation with real creators

Create an editor usability test plan with at least these tasks:
1. Install/open VICTUS and create a 2D project.
2. Create a playable scene without C++ or manual project-file editing.
3. Import a sprite/model/audio asset and resolve an intentional import error.
4. Add and configure components from the inspector.
5. Create a prefab and reuse it.
6. Create gameplay using visual scripting or Luau.
7. Configure input for keyboard and gamepad.
8. Find and fix an intentional scene/script/asset error.
9. Profile a sample and understand at least one measured bottleneck.
10. Export and run a packaged build.

Run tasks with people who did not build VICTUS, including beginners. Record completion, time, help required, misclicks, confusion, failures and feedback. Do not invent participant results. Fix the highest-impact repeated friction, then retest.

Automated coverage:
- editor UI/integration tests for project creation, workspace restore, hierarchy, inspector, asset import/reimport, undo/redo, save/reopen, play/stop and export;
- keyboard navigation and DPI/layout regression tests where practical;
- screenshots/golden images only as supplemental evidence, not proof of functionality;
- clean-checkout sample workflows in CI.

## Competitor evidence

Use current stable versions and primary documentation for Godot, Unity and Unreal, plus O3DE/Bevy/Flax/Stride where relevant. Compare workflows, not screenshots or marketing claims:
- project launcher and first-session learning;
- scene/hierarchy/inspector ergonomics;
- asset library, import, reimport, dependency and package management;
- 2D/3D creation and specialized editors;
- visual scripting and scripting onboarding;
- workspace customization, keyboard workflows and accessibility;
- diagnostics, undo/recovery, profiling, build/export and collaboration.

For each capability classify VICTUS as **verified competitive / competitive in a narrow area / behind / missing / unknown** and attach dated evidence. Do not claim overall parity or superiority based on a UI refresh.

## Required deliverables and execution order

1. `docs/audits/STUDIO_UX_AUDIT.md` — source-backed audit and gap matrix.
2. Updated `docs/EDITOR_SPEC.md` — distinguish current implementation from planned 1.0 requirements; remove ambiguous claims.
3. Small vertical implementation slices, each with tests, docs and screenshots/captures when useful:
   - design-system and workspace consistency;
   - first-run/project creation onboarding;
   - hierarchy/inspector workflow;
   - asset browser/import diagnostics;
   - visual authoring discoverability;
   - play/debug/profile/export workflow;
   - accessibility/localization and external usability fixes.
4. Updated `docs/USER_GUIDE.md`, `docs/STATUS.md`, `ROADMAP.md` and relevant ADRs.
5. Direct commits to `main` after each coherent verified slice. After every commit, inspect latest main and continue with the next unblocked item. Persist exact progress and blockers if the session ends.

## Definition of success

A beginner can open VICTUS, understand where to start, create and edit a real 2D or 3D game, use the asset library, add gameplay without C++, debug problems, save/reopen and export a working build through a coherent editor. An experienced creator can customize layouts, use shortcuts and advanced tools without fighting the interface. Independent testers can complete the core tasks without maintainer intervention.

**This handoff does not declare VICTUS mature or competitive today.** It defines the work and evidence required to make the creator experience trustworthy. A beautiful screenshot is not enough; the editor must be beautiful, understandable, responsive and genuinely functional.
