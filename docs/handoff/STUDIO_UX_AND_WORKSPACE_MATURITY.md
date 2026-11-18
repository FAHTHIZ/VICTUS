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


## Additional gaps surfaced by the 2026-11-18 cross-document review

These are explicit audit targets because the current `KNOWN_LIMITATIONS.md`, `ROADMAP.md` and `STATUS.md` identify them as incomplete, experimental or not yet validated. Recheck each against current code before changing status.

### Creator-facing editors that must not be forgotten

- **In-game UI authoring:** current limitations say there is no visual UI canvas editor, rich text, slider or text input. A complete UI workflow needs a canvas/layout editor, anchors/constraints, responsive preview sizes, hierarchy, style/theme editing, fonts, navigation/focus preview, localization preview and working widgets. Implement missing runtime widgets as prerequisites; do not expose nonfunctional controls.
- **Visual AI authoring:** behaviour trees currently have runtime/debugger support but no visual tree editor. Add a real authoring canvas with node palette, typed pins/conditions, blackboard inspection, validation, breakpoints/step controls if supported, and live play-mode debug. Reuse visual graph infrastructure where sensible without forcing unrelated graph semantics into one generic editor.
- **Particles/VFX:** current system is experimental and CPU-based. Add creator workflows for emitter preview, curves/gradients, bursts, shape controls, deterministic seed, performance budgets and diagnostics. GPU particles, trails, flipbooks, collision and lit particles are engine/runtime capabilities to evaluate separately; don't claim they exist because a panel exists.
- **Audio authoring:** a mixer/bus editor, asset audition, routing/volume/mute/effects preview and clear device/output diagnostics. Advanced spatial/HRTF/ducking features remain runtime gaps, not UI polish tasks.
- **Animation authoring versus inspection:** the new Animation window is a controller inspector/preview/debugger. It is not yet a complete animation authoring suite. Evaluate clip timeline editing, keyframe/curve editing, blend-space visual authoring, transition graph editing, skeleton/rig inspection, retargeting and IK only with explicit runtime capability and acceptance tests.
- **Terrain/foliage authoring:** improve brush discoverability, brush presets, layer painting feedback, foliage preview/instancing diagnostics and undo/recovery for large edits.
- **2D workflow:** dedicated 2D scene ergonomics, camera/zoom/pixel snapping, tile palette, sprite atlas workflow, collision editing, animation and 2D UI should be tested as an end-to-end first-class path.

### Product workflow gaps outside the visible editor

- **Asset dependency graph and import queue:** explicitly listed as pending in ROADMAP Phase 5/6 and KNOWN_LIMITATIONS. Treat background import, progress, cancellation, retry, validation reports, dependency impact and reference-safe rename/move/delete as a single dependable pipeline.
- **Packaging/distribution:** current limitations list no cross-export, code signing, installers or patching. Design clear platform export targets, prerequisite detection, reproducible outputs, installer/signing integration where legally and technically supported, patch/update strategy and release validation. Do not imply every platform can be exported from every host.
- **Plugin/mod ecosystem:** currently missing a plugin loader, versioned public API and mod sandbox. Design extension discovery, compatibility/versioning, permissions, trust boundaries, crash isolation, documentation and sample extensions before advertising a marketplace or binary compatibility.
- **Source control/collaboration:** evaluate project diffs, asset metadata stability, scene/graph mergeability, lock/conflict handling, collaboration and team workflows. Do not bolt on a shallow Git status panel and call collaboration complete.
- **Networking workflow:** networking is currently experimental and tested on loopback according to known limitations. Add multiplayer project setup, local server/client launch, session diagnostics, latency/loss simulation, replication inspection and deployable dedicated-server workflow; separately validate real network conditions, NAT traversal and host migration if those are product goals.
- **Save-game authoring:** a beginner-facing save/checkpoint browser, schema migration diagnostics and reproducible test/reset tools.
- **Project health:** a single project validation/health dashboard for missing assets, invalid references, unsupported platform features, oversized assets, script errors, import failures and export blockers.
- **Learning ecosystem:** searchable API reference generated from the actual public scripting surface, contextual examples, template walkthroughs, migration guides and sample projects kept buildable in CI.

### Audit findings: documentation drift to resolve

The 2026-11-18 documents are not perfectly synchronized:
- `ROADMAP.md` says terrain collision edits update Jolt heightfields in place via dirty rectangles and `HeightFieldShape::SetHeights`; `KNOWN_LIMITATIONS.md` still says the whole heightfield is rebuilt on every edit. Verify implementation/tests, then correct the stale statement.
- `ROADMAP.md` describes the clang-tidy CI gate as pending; `KNOWN_LIMITATIONS.md` says clang-tidy runs on engine/editor/tools/apps sources. Clarify the difference between a configured/running analysis job and a required merge gate, and state whether test sources are included.
- The latest animation editor commit adds controller preview/inspection and parameter controls. Keep the status precise: it does not establish a full clip/curve/rig authoring suite, IK, retargeting, additive layers or root motion.
- Platform docs distinguish CI builds, software Vulkan, virtualized Apple GPU and physical-device validation. Preserve those distinctions in every UI/export claim.

The audit must scan all status/spec/roadmap/user-guide/ADR documents for stale claims, contradictory acceptance criteria, orphaned roadmap items and features marked complete without end-to-end user workflows. Add a consistency check or a lightweight structured capability registry if it materially reduces future drift; do not create a second manually maintained source of truth without a clear owner.

### Expanded acceptance matrix

Add explicit usability tests for:
1. A non-programmer creates and edits a UI canvas and uses it in a packaged game.
2. A creator builds and debugs a behaviour tree visually.
3. A creator authors, previews and tunes a particle effect.
4. A creator previews and routes audio assets through buses.
5. A creator edits an animation controller and can distinguish preview/debug from authoring.
6. A creator imports a large asset set, cancels/retries work and resolves dependency/missing-reference problems.
7. A creator exports for a supported target, receives accurate prerequisite warnings and launches the package.
8. A team member can understand source-control status and conflict limitations without risking project data.
9. A developer installs/enables a sample extension only if the versioned plugin boundary and safety model have actually shipped.
10. A multiplayer creator launches and inspects a local session, then follows a documented path to test a remote session.

Record which tasks are currently impossible because a runtime/editor subsystem is missing. Those become explicit engine roadmap dependencies, not reasons to fake the UI.


## Additional creator workflow — View Fidelity Lab

Coordinate with [VICTUS Camera-Responsive Visual Fidelity and Per-View Budgets](VICTUS_CAMERA_RESPONSIVE_VISUAL_FIDELITY.md). Vectors is not complete as a creator-facing system if the editor cannot show why an object changed detail or reproduce a zoom-related defect.

### Required view and asset inspection

Add a View Fidelity Lab to the real renderer/editor workflow, reusing the existing viewport and profiler rather than creating a disconnected demo:

- Display the active view's projection/FOV, target pixel dimensions, render scale and quality tier.
- Preview a repeatable wide-to-narrow-to-wide camera sweep at a fixed position, plus a camera-dolly sweep.
- Show per-object selected LOD, triangle count, source geometric error and projected pixel error where those values are actually available.
- Visualize selected LODs with colour/wireframe overlays and allow a debug force-LOD override that never changes cooked/gameplay data.
- Show per-view results for supported secondary views and make the cost of each view visible.
- Show texture format, mip count and current mip/residency only when the renderer can provide truthful values. Until mip streaming exists, label the asset as fully resident or report that residency granularity is unavailable; do not invent a streaming UI.
- Show shadow/probe contributions independently from main-view visibility.
- Expose the active geometry/view budget, quality floor and any unmet budget as actionable diagnostics.

### Creator acceptance

1. An artist can select a wall or character and scrub camera FOV while watching LOD/error change.
2. The same asset can be previewed at several target resolutions and aspect ratios.
3. The artist can compare an asset's full-detail source with its selected runtime LOD and inspect silhouette/alpha coverage.
4. A character can be previewed through several animation poses while the camera zooms.
5. A developer can distinguish geometry error from texture/mip softness and from render-scale/upscaler softness.
6. A creator can save a reproducible camera sweep or regression capture with the asset/scene.
7. All displayed values come from the live renderer or are clearly marked unavailable. No inert toggles, pretend texture-streaming controls or false GPU-utilization numbers.

The preview must use the production renderer path and actual view dimensions. A special editor-only LOD formula that differs from play mode is not acceptable.
