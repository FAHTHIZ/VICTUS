# VICTUS Roadmap

Living, evidence-driven plan. Status reflects verified repository state, not intention. Work directly on main in small, buildable commits.

**Phase:** 0 — foundation. **Maturity:** early bootstrap, not a production engine.

Legend: [x] verified in repository · [~] partial/in progress · [ ] not started · [!] blocked.

## Phase 0 — Foundation
- [x] Project identity, principles and repository layout.
- [x] Autonomous-agent contract and direct-main delivery rules.
- [x] Roadmap, architecture, research and subsystem specifications.
- [x] Third-party/license policy.
- [x] Minimal C++20 core/runtime/test slice.
- [x] CMake build and initial desktop CI.
- [ ] Verify hosted CI and resolve environment failures.
- [ ] Add formatting/static analysis and sanitizer lanes.
- [ ] Add setup scripts and troubleshooting.

## Phase 1 — Runtime and diagnostics
- [ ] Lifecycle, ownership and shutdown ordering.
- [ ] Structured logging, categories, sinks and diagnostic events.
- [ ] Error/result types and validated configuration.
- [ ] Platform abstraction: filesystem, clock, threads, window, input, paths, device capabilities.
- [ ] CLI modes: editor, game, headless, cook, test, benchmark.
- [ ] Crash diagnostics, build metadata, metrics and tracing.
**Exit:** Debug/Release builds, tests, headless launch, predictable shutdown and useful diagnostics on desktop CI.

## Phase 2 — Performance lab
- [ ] Benchmark harness with warm-up, repetitions and machine/build metadata.
- [ ] CPU profiler, scoped timings and trace export.
- [ ] GPU timestamp/query abstraction.
- [ ] Memory accounting, allocations and high-water marks.
- [ ] Frame/tick histograms and p50/p95/p99.
- [ ] Synthetic entity, transform, visibility, asset decode, streaming and scheduler workloads.
- [ ] Machine-readable results and baseline methodology.
- [ ] Baselines on desktop and Android hardware when available.
**Exit:** reproducible benchmark command and documented methodology; no unsupported claims.

## Phase 3 — Simulation kernel
- [ ] Compare Flecs, EnTT and custom archetype/SoA using VICTUS workloads.
- [ ] Compare Taskflow, enkiTS and custom scheduler; test affinity/cancellation.
- [ ] VICTUS-owned ECS/system API or documented reason to expose dependency.
- [ ] Transform hierarchy and spatial queries.
- [ ] Controlled simulation clock, bounded catch-up, pause/step.
- [ ] Event model, deterministic random streams and ordering rules.
- [ ] Versioned snapshots, serialization and migrations.
- [ ] Headless simulation host.
- [ ] LOD tiers: full entities, simplified entities, regional aggregates, event-driven dormant regions.
- [ ] Invariant tests for LOD transitions/population/resource totals.
- [ ] Large-population benchmarks.
**Exit:** headless simulation runs, saves, reloads and replays documented workloads; LOD correctness tests pass.

## Phase 4 — Graphics/RHI
- [ ] ADR comparing bgfx, Diligent, direct Vulkan/Metal/D3D12 behind VICTUS RHI and alternatives.
- [ ] Verify Windows/Linux/Android/Apple backend matrix.
- [ ] Prototype surface/device/swapchain/resource lifetime/shader compilation.
- [ ] VICTUS RHI without third-party types in public interfaces.
- [ ] Render graph, frame resources and GPU synchronization.
- [ ] Validated triangle/clear demo and debug output.
- [ ] Shader build/cache/reflection and device capability profiles.
- [ ] Compatibility fallback.
**Exit:** minimal scene on desktop and mobile-capable backend, documented limitations.

## Phase 5 — Asset pipeline
- [ ] Stable asset IDs, metadata, dependency graph and source-to-cooked model.
- [ ] glTF 2.0 import first; assess FBX/SDK licensing separately.
- [ ] Mesh/material/skeleton/animation/image/HDR import.
- [ ] Texture mips, color management, GPU compression and platform variants.
- [ ] Geometry validation, mesh optimization, LOD and collision proxies.
- [ ] Content-addressed cache, incremental cook and invalidation.
- [ ] Background import, cancellation, progress and recoverable errors.
- [ ] Asset registry/dependency inspection and Blender export presets.
- [ ] Evaluate OpenUSD for complex scene interchange.
**Exit:** import glTF, cook variants, load at runtime, detect source changes and report diagnostics.

## Phase 6 — VICTUS Studio editor
- [ ] Prototype Dear ImGui docking vs Qt/Avalonia/native shell.
- [ ] Project manager/templates; dockable workspace and command palette.
- [ ] Scene viewport, camera, selection and gizmos.
- [ ] Hierarchy, inspector and component editing.
- [ ] Snapping, multi-select, undo/redo.
- [ ] Asset browser, import queue and previews.
- [ ] Material/shader editing and diagnostics.
- [ ] Isolated play/pause/step/stop.
- [ ] Save/reopen, corrupt-asset recovery, logs and profiler.
- [ ] Layouts, preferences and accessibility.
**Exit:** create project, import model, assemble scene, save/reopen and run without source edits.

## Phase 7 — Renderer and world
- [ ] PBR and color management.
- [ ] Forward/clustered baseline; deferred only if justified.
- [ ] Shadows, cascades, contact shadows and scalable quality.
- [ ] Image-based/environment lighting and probes.
- [ ] Screen-space effects with mobile fallbacks.
- [ ] GPU culling/indirect draws where beneficial.
- [ ] Mesh LOD, instancing, occlusion and streaming.
- [ ] Virtual texturing/virtual geometry research prototypes.
- [ ] Terrain, foliage, water, atmosphere and large-world precision.
- [ ] World cells, async IO and memory budgets.
- [ ] High-end/standard/mobile/compatibility tiers; dynamic resolution.
**Exit:** representative outdoor scene with measured quality tiers on desktop and mobile.

## Phase 8 — Essential systems
- [ ] Jolt candidate physics behind VICTUS API; character controllers/collision queries.
- [ ] Recast/Detour candidate navigation and crowd movement.
- [ ] Skeletal animation, blending, state machines and retargeting.
- [ ] Audio, spatialization, buses and streaming.
- [ ] Input mapping, gamepad and touch.
- [ ] Scripting evaluation, sandboxing, hot reload and API stability.
- [ ] UI runtime and visual UI authoring.
- [ ] Save/load, checkpoints and migrations.
- [ ] Networking evaluation: transport, replication, prediction and authority.
- [ ] Localization, accessibility and platform services.
**Exit:** interactive sample uses physics, animation, navigation, audio, input, UI and save/load.

## Phase 9 — Procedural creation
- [ ] Typed procedural graph, validation and deterministic seeds.
- [ ] Terrain/biome/vegetation generation.
- [ ] Building/settlement generation and rule-based placement.
- [ ] Scatter, spline and road tools.
- [ ] Material/shader graph; animation graph/state machine.
- [ ] Cinematic sequencer and asset optimization reports.
- [ ] Optional AI-assisted authoring with human review and reproducibility.
**Exit:** reproducible procedural environment generated, edited, saved, cooked and loaded at multiple quality tiers.

## Phase 10 — Cross-platform/mobile hardening
- [ ] Windows/Linux packages; macOS/Metal build and signing.
- [ ] Android arm64 runtime, touch, thermal/memory profiling and device matrix.
- [ ] iOS build/signing feasibility and policy review.
- [ ] Backend fallback matrix, low-memory eviction and context recreation.
- [ ] Suspend/resume, battery-aware pacing and quality adaptation.
- [ ] Package size/startup budgets and device smoke tests.
**Exit:** tested device matrix, reproducible builds and sustained performance evidence.

## Phase 11 — Production/ecosystem
- [ ] Plugin/module SDK and versioning.
- [ ] Public API stability and migration guides.
- [ ] Docs site/tutorials; 3D, simulation, mobile and headless examples.
- [ ] Modding/sandbox security model.
- [ ] Localization/accessibility.
- [ ] Release automation/changelog/semantic versioning.
- [ ] Security response, dependency updates and performance regression dashboard.
- [ ] Governance and long-term support.
**Exit:** external developer installs VICTUS, follows tutorial, builds a supported sample and diagnoses performance.

## Definition of done
A checkbox is complete only when implementation is on main, tests pass, build commands work, errors/fallbacks are handled, platform/dependency limits are documented, performance claims have evidence, diff is audited and SHA plus verification are recorded in docs/STATUS.md.

## Immediate next actions
1. Verify GitHub Actions.
2. Add formatting/static analysis and sanitizer lanes.
3. Implement structured diagnostics/result types.
4. Build benchmark harness before selecting performance libraries.
5. Run ECS/scheduler bake-offs.
6. Prototype graphics backend candidates.

Update status after every verified milestone. Never mark work complete based only on generated code.
