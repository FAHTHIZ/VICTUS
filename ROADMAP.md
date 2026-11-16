# VICTUS Roadmap

Living, evidence-driven plan. Work directly on `main` in small, buildable commits. Status reflects verified repository state, never ambition.

**Current maturity:** core runtime, platform, RHI, real 2D/3D renderer (Vulkan verified), ECS/scenes, asset pipeline, an editor with project templates, a standalone player, a `victus` CLI, Luau gameplay scripting with hot reload and 3D/2D rigid-body physics (Jolt/Box2D) and audio (miniaudio), and `victus export` packaging into a pack-based runnable game folder. No networking yet. Not a production engine.

Legend: [x] verified · [~] in progress/partial · [ ] not started · [!] blocked.

## Phase 0 — Foundation and project quality
- [x] Project identity, principles, repository layout, agent contract, roadmap, architecture and license policy.
- [x] Minimal C++20 core/runtime/test slice and initial CMake/desktop CI.
- [x] Verify hosted CI; fix any failures.
- [~] Formatting, static analysis, sanitizers, warning policy and dependency scanning. (Sanitizer CI lanes, -Werror policy and .clang-format done; clang-tidy and dependency scanning pending.)
- [ ] Reproducible developer setup, presets, troubleshooting and contribution workflow.
**Exit:** clean Debug/Release build and tests on supported desktop CI.

## Phase 1 — Runtime, platform layer and diagnostics
- [~] Lifecycle, ownership, shutdown ordering, error/result types and validated configuration. (Result/Error, JSON config parsing done.)
- [x] Structured logging, diagnostic events, crash context, build metadata and tracing.
- [~] Platform services: filesystem, paths, clock, threads, window, input, device capabilities and lifecycle. (SDL3 windows, keyboard/mouse/gamepad/touch input, lifecycle events, clipboard, GPU capabilities implemented; mobile lifecycle untested on devices.)
- [~] CLI modes: editor, game, headless, cook, test, benchmark and package. (`victus new/templates/info/import/run/check/simulate/export`, `victus_editor`, `victus_player` done; `simulate` is the headless mode and `export` packages; a separate cook-only command is pending.)
- [~] Stable project configuration and runtime/editor/server target separation. (Versioned `victus.project`; separate editor and player targets; server target pending.)
**Exit:** predictable lifecycle and useful diagnostics in graphical and headless modes.

## Phase 2 — Performance laboratory
- [x] Reproducible benchmark harness with warm-up, repetitions, machine/build metadata and machine-readable output.
- [ ] CPU/GPU profiling, frame/tick histograms, p50/p95/p99, memory accounting and allocation tracking.
- [~] Workloads for ECS, jobs, transforms (done: `victus_bench_scene`, `victus_bench_script`, `victus_bench_physics`, `victus_bench_audio`), animation, visibility, rendering, asset import, streaming, networking and saves.
- [ ] Desktop baselines; Android physical-device baselines when hardware is available.
- [ ] Performance budgets and regression gates based on measured reference hardware.
**Exit:** another developer can reproduce the published results and compare equivalent workloads.

## Phase 3 — Core simulation, ECS and jobs
- [ ] Benchmark custom SoA/archetype approaches against Flecs and EnTT on VICTUS workloads.
- [~] Compare task schedulers and define dependency, affinity, cancellation and priority semantics. (VICTUS work-stealing JobSystem + TaskGraph with priorities implemented and benchmarked; external comparison pending.)
- [x] VICTUS-owned ECS/system API or documented reason to expose a dependency. (`engine/ecs`, ADR-0004.)
- [~] Transform hierarchy (done: `engine/scene`), spatial indexes, events, deterministic random streams and controlled simulation clock.
- [~] Versioned snapshots, serialization (done: `victus.scene` v1 JSON), migrations and headless simulation host.
- [ ] Simulation LOD: full entities, simplified entities, regional aggregates and event-driven dormant regions.
- [ ] Invariant tests for populations, resources, ownership, time, relationships and event ordering.
**Exit:** headless simulation saves, reloads, replays and passes correctness/performance workloads.

## Phase 4 — Graphics API and rendering foundation
- [x] Research and record backend choices: Vulkan, D3D12, Metal, OpenGL/ES or other justified fallback; compare specialist abstractions. (ADR-0003: SDL_GPU behind VICTUS RHI.)
- [~] VICTUS-owned RHI, capability model, device/swapchain/resource lifetime, synchronization and diagnostics. (Implemented and tested on Vulkan; Metal/D3D12 unexecuted.)
- [~] Render graph, frame resources, shader compilation/cache/reflection and GPU profiling. (Render graph with culling/aliasing/transient pool, offline shader pipeline with reflection done; GPU timing pending.)
- [x] Real 2D and 3D render paths; validated geometry, textures, materials and camera. (SpriteBatch; forward PBR with shadow map, point lights, sky, tonemap; pixel-verified tests.)
- [ ] Backend/device-loss recovery and compatibility fallbacks.
**Exit:** actual 2D and 3D scenes render on the validated desktop backend; mobile path has a verified capability plan.

## Phase 5 — Asset pipeline and content cooking
- [~] Stable asset IDs, metadata, dependency graph and versioned source-to-cooked format. (UUID `.meta` sidecars + versioned cooked cache done; cross-asset dependency graph pending.)
- [~] glTF 2.0/GLB import, validation, mesh/material/skin/animation/image/HDR handling. (Meshes, PBR materials, images, node hierarchy done; skins/animation pending.)
- [ ] Evaluate authoring interchange such as OpenUSD separately from runtime delivery formats.
- [~] Texture mips, color management (done), platform compression and asset variants.
- [ ] Mesh optimization, LODs, collision proxies and import validation.
- [~] Content-addressed cache, incremental cook, dependency invalidation and deterministic outputs. (Hash-keyed cooked cache with up-to-date checks done.)
- [ ] Background import, cancellation, progress, recoverable errors and asset diagnostics.
- [ ] External DCC workflow and Blender interchange/export presets.
**Exit:** import, validate, cook, cache, hot-reload and load real project assets on a clean machine.

## Phase 6 — VICTUS Studio editor core
- [~] Project launcher/templates, dockable workspace, command palette and saved layouts. (Launcher, 3 templates, docking workspace with persisted ImGui layout done; command palette pending.)
- [~] Scene/world viewport, camera, hierarchy, inspector, selection, gizmos and snapping. (Viewport, 2D/3D editor camera, hierarchy, inspector, click selection done; gizmos/snapping pending.)
- [~] Undo/redo command model, multi-selection, autosave and crash recovery. (Snapshot-based undo/redo with dirty tracking done; multi-selection/autosave pending.)
- [~] Asset browser, metadata/dependency inspector, import queue and previews. (Asset table with drag/drop instantiate and reimport done; previews pending.)
- [~] Logs, entity diagnostics, profiler, project settings and build configuration. (Console over the log ring sink done.)
- [x] Isolated play-in-editor with safe play/pause/step/stop and authored-world protection.
**Exit:** create project, import assets, author a scene, save/reopen and play without editing engine source.

## Phase 7 — Complete 2D development
- [ ] 2D camera, sprites, atlases, batching, sorting/layers and pixel-art controls.
- [ ] Tilemaps, tile sets, autotiling, grid and 2D world editing.
- [x] 2D physics, collision shapes, queries and debug visualization: Box2D 3.2.0 bodies, box/circle/capsule colliders, triggers, raycast/overlap queries, editor collider overlay (Slice 6).
- [ ] 2D animation, skeletal/rigged workflows where justified, particles and parallax.
- [ ] 2D UI, input, audio, scene transitions and 2D lighting/effects.
- [ ] 2D templates, profiling, export and playable reference game.
**Exit:** build and ship a complete 2D game using only supported VICTUS workflows.

## Phase 8 — Complete 3D renderer and world systems
- [ ] PBR materials, physically meaningful units/color management, forward/clustered baseline and justified alternatives.
- [~] Direct/indirect lighting, shadows, reflection probes, environment lighting and scalable global illumination. Done (Slice 9): cascaded shadow maps, SSAO, specular occlusion, analytic sky/environment lighting. Remaining: reflection probes, GI, clustered lights.
- [~] Post-processing, temporal techniques, anti-aliasing, depth of field, motion blur and tone mapping. Done (Slice 9): dual-filter bloom, GPU auto exposure, ACES/PBR Neutral/Reinhard tonemapping with grading/vignette, FXAA 3.11. Remaining: TAA/upscaling, DOF, motion blur.
- [~] GPU culling/indirect draws where beneficial; instancing, occlusion, mesh LOD and streaming. Done (Slice 10): automatic GPU instancing in all mesh passes (~12× on 4 096 cubes, llvmpipe). Remaining: GPU culling/indirect, occlusion, LOD, streaming.
- [ ] Terrain, foliage, water, atmosphere, weather, sky/clouds and large-world precision.
- [ ] Decals, dynamic environment effects and destruction interfaces.
- [~] Scalable high/standard/mobile/compatibility tiers, dynamic resolution and user overrides. Done (Slice 9): Low/Medium/High/Ultra presets, render scale, per-project setting and `--quality` override. Remaining: dynamic resolution, device-based defaults, mobile GPU validation.
- [ ] Research-only advanced paths: ray tracing, virtualized geometry/textures and other high-cost features; implement only after evidence.
**Exit:** representative outdoor and indoor scenes with measured quality tiers on desktop and mobile.

## Phase 9 — Gameplay framework and scripting
- [ ] Game instance/session, world/level lifecycle, game mode/rules, game state and player state.
- [~] Actors/entities, components, player controllers, pawns/characters, cameras and spawn points. Done: entities/components (Slice 3), scripted player control (Slice 5), 3D physics characters (Slice 11). Remaining: reusable controller/camera rigs and spawn points.
- [ ] Input actions/mappings for keyboard, mouse, controllers and touch. (Raw keyboard/mouse input is available to scripts via `input.*`; no action maps or gamepad/touch bindings yet.)
- [ ] Interaction, gameplay events, timers, tags, attributes, inventory/ability primitives as modular systems.
- [ ] Prefabs/packed scenes, reusable components, variants and safe instantiation.
- [x] Scripting runtime: Luau 0.747 sandboxed VM, `ScriptComponent` + `.luau` assets, entity/scene/input/time/log API, lifecycle (`start`/`update`/`fixed_update`/`reload`), editor-visible properties with per-entity overrides, hot reload in editor and `victus run`, time/memory budgets, `victus check` and headless `victus simulate` (Slice 5).
- [ ] Script debugging (breakpoints/stepping), type checking in `victus check`, API versioning, events/messages between scripts.
- [ ] Visual scripting graph with typed nodes, validation, debugging and deterministic serialization.
- [ ] Save/load, checkpoints, profile data, version migration and recovery.
**Exit:** complete playable 3D game loop authored in VICTUS with both code and visual-logic options.

## Phase 10 — Physics, animation, AI, audio and UI
- [~] 3D/2D physics, rigid bodies, character controllers, collision queries, joints and debug tools. Done (Slice 6): Jolt 5.7.0 + Box2D 3.2.0 behind `victus::physics`, rigid bodies, box/sphere/capsule/cylinder colliders, triggers, layers/masks, raycasts/overlaps, contact events to scripts, collider debug lines, reflected inspector. Done (Slice 11): 3D character controller (Jolt `CharacterVirtual`: slopes, steps, ground detection, jumping, pushing, moving platforms) and 3D joints (fixed/point/hinge/slider/distance with limits, motors, breaking). Remaining: mesh/convex colliders, 2D character controller and joints, 2D polygons, ragdolls.
- [~] Skeletal animation, blend spaces, state machines, retargeting, root motion and animation tooling. Done (Slice 12): glTF skins and clips imported/cooked, GPU skinning in all mesh passes, `Animator` playback with loops and cross-fades, parallel evaluation, Luau control. Remaining: blend spaces, state machines, layers, root motion, IK, retargeting, morph targets, animation editor tooling.
- [ ] Navigation meshes, pathfinding, avoidance, crowds and behavior/state systems.
- [ ] Gameplay AI: perception, decision systems, behavior trees/utility/state machines and debugging.
- [~] Audio playback, buses/mixing, spatialization, effects, streaming and profiling. Done (Slice 7): miniaudio 0.11.26 behind `victus::audio`, `AudioSource`/`AudioListener`, WAV/Ogg/MP3/FLAC, six buses with volume/mute, 3D attenuation following transforms, streaming music, one-shots, Luau API, editor inspector + preview, null-device mixing tests. Remaining: effects/reverb, HRTF, ducking, profiling view.
- [ ] Runtime UI, layout, input/accessibility, localization and visual UI editor.
- [ ] Particle/VFX system, GPU/CPU emitters, trails, curves, collision and VFX authoring.
**Exit:** reference game exercises the systems together without one-off engine bypasses.

## Phase 11 — Cinematics, procedural and world authoring
- [ ] Timeline/sequencer, camera rigs, animation tracks, events, audio and render/export.
- [ ] Material/shader graph, animation graph and procedural graph.
- [ ] Terrain/biome/vegetation, spline/road, scatter and settlement tools.
- [ ] Deterministic procedural seeds, validation and reproducible generation.
- [ ] Optional AI-assisted authoring with human review, provenance and reproducibility.
**Exit:** create, edit, save, cook and play a substantial procedural world and cinematic sequence.

## Phase 12 — Multiplayer and online runtime
- [ ] Transport abstraction, authoritative server model and session lifecycle.
- [ ] Entity/property/event replication, relevancy, ownership and bandwidth budgets.
- [ ] Client prediction, reconciliation, interpolation and lag-tolerant movement.
- [ ] Dedicated/headless server builds, server config, logs and graceful shutdown.
- [ ] Network emulation for latency, jitter, packet loss and bandwidth constraints.
- [ ] Multiplayer debugging, replay/recording, reconnect and persistence.
- [ ] Replaceable interfaces for matchmaking, lobbies, identity and platform services; no mandatory vendor lock-in.
- [ ] Security model, input validation, server authority and abuse-resistant boundaries.
**Exit:** two or more clients play a representative networked game against an authoritative dedicated server under repeatable network tests.

## Phase 13 — Production, collaboration and ecosystem
- [~] Build automation, asset cooking, incremental compilation, caching and reproducible packaging. Done (Slice 8): `victus export` (validated, byte-reproducible `.vpak` archive with checksummed entries, cooked-only assets, embedded third-party notices, manifest) and the player's packaged mode. Remaining: compression, GPU texture compression (ASTC/BC), cross-platform targets, signing, build automation.
- [ ] Hot reload for code/assets/shaders where safe; clear fallback when reload is unsupported.
- [ ] Automated unit/integration/gameplay tests, smoke tests and deterministic regression captures.
- [ ] Crash reporting, symbol handling, diagnostics export and privacy controls.
- [ ] Source-control integration, project diffs/merges, collaboration foundations and conflict handling.
- [ ] Plugin/module SDK, versioned public APIs, extension lifecycle and migration tooling.
- [ ] Modding sandbox, permissions, package signing/validation and safe content boundaries.
- [ ] Localization, accessibility, tutorials, sample templates and API documentation.
- [ ] Release automation, changelog, SBOM, security response and dependency update process.
**Exit:** an external developer can install VICTUS, collaborate on a project, build, test, package and diagnose a shipped game.

## Phase 14 — Mobile, XR and additional platform validation
- [ ] Windows/Linux desktop runtime and editor; macOS/Metal runtime/editor.
- [ ] Android arm64 runtime, touch, thermal/memory profiling, lifecycle and physical-device matrix.
- [ ] iOS runtime/build/signing feasibility and policy validation.
- [ ] Mobile asset variants, low-memory eviction, context recreation, suspend/resume and battery-aware pacing.
- [ ] OpenXR integration research and XR input/rendering only after desktop/mobile core is stable.
- [ ] Web/WebAssembly feasibility study; only pursue if required APIs, threading, memory and distribution constraints are acceptable.
- [ ] Console support only with platform-holder access, SDK agreements and compliance; never imply support without it.
**Exit:** documented, reproducible platform matrix with actual build, launch, input, rendering and package tests.

## Phase 15 — Reference projects and production readiness
- [ ] Complete 2D game demonstrating the 2D workflow.
- [ ] Complete 3D gameplay game demonstrating rendering, gameplay, AI, audio, UI, saves and packaging.
- [ ] Complete simulation project demonstrating large populations, simulation LOD, persistence and headless mode.
- [ ] Optional multiplayer sample demonstrating dedicated server and network diagnostics.
- [ ] Build all references from clean checkouts in CI; run gameplay smoke/regression tests.
- [ ] Profile on named desktop and mobile hardware; publish reproducible reports and known limitations.
- [ ] External usability testing, tutorials, migration docs and stable release criteria.
**Exit:** independent developers can build, run, test and package the reference projects using documented workflows. Production readiness is a measured release decision, not a roadmap checkbox.

## Definition of done
A task is complete only when code is integrated on main, relevant tests pass, build commands work, errors/fallbacks are handled, platform and license limits are documented, performance claims have evidence, the diff is audited, and SHA plus verification are recorded in `docs/STATUS.md`. A demo or placeholder is not completion.

## Execution order
Follow dependency order and complete each subsystem end-to-end. If a phase is blocked by unavailable hardware, credentials or platform-holder access, document the precise blocker and continue independent work. After every verified commit, update status and immediately proceed to the next unblocked task.
