# Handoff: VICTUS Competitive Maturity and Production-Readiness Gap Closure

**Priority:** High — product maturity / shipping confidence  
**Status:** Ready for agent execution  
**Target:** Direct commits to main; no branch or PR.

## Mission

Move VICTUS from a broad, technically ambitious engine to a dependable engine that an independent developer can use to build, debug, test, package and ship a complete game. Do not claim that VICTUS is equal to or better than Unreal, Unity or Godot based on a feature checklist. Establish an evidence-based competitor baseline, close the most consequential gaps, and publish repeatable proof.

This is a continuing engineering program, not a one-commit feature. Work in small verified slices, update ROADMAP.md and docs/STATUS.md, and continue to the next unblocked task.

## Why VICTUS is not yet production-mature

The repository already has substantial foundations: ECS/jobs, a PBR renderer, glTF import and skinning, physics, animation controllers, scripting, AI/navigation, audio, UI, multiplayer, an editor, CLI/export, tests and sample projects. However, maturity is not the number of implemented subsystems. It is whether those systems work together reliably for external developers across real hardware and complete production workflows.

The current roadmap/status explicitly records these blockers:
- No game has shipped using VICTUS.
- No physical-device Android performance/thermal validation; Windows rendering and interactive Windows/macOS runs remain unverified; platform support is not equivalent to build-only CI.
- The editor is implemented but needs broader real-user workflow validation and further authoring depth.
- Animation controller editor preview remains open; root motion is optional/open; retargeting, IK, additive layers and morph-target animation are not currently supported.
- No plugin SDK or stable versioned extension API.
- Source-control integration, project diffs/merges and collaboration foundations remain open.
- Crash reporting, symbol handling and diagnostics export/privacy controls remain open.
- Reference games are not yet complete release-quality proof; external usability testing and reproducible clean-checkout builds remain open.
- Performance evidence must be collected on actual GPUs/phones. Software Vulkan/lavapipe test results do not prove physical GPU performance.
- Device-loss recovery and other documented platform/runtime limitations must be treated as known product risks, not hidden by feature claims.

These are not reasons to dismiss the work already completed. They are the concrete distance between a broad engine codebase and a proven production tool.

## Operating rules

1. Read AGENTS.md, ROADMAP.md, docs/STATUS.md, docs/KNOWN_LIMITATIONS.md and docs/PLATFORM_SUPPORT.md before choosing work.
2. Inspect current main and the exact implementation before changing anything. This handoff is based on roadmap/status evidence and is not a substitute for a fresh code audit.
3. Follow dependency order. Fix reliability and workflows before adding superficial parity features.
4. Do not rewrite functioning subsystems merely to resemble competitor architecture. Prefer VICTUS-owned solutions and open-source dependencies with compatible licenses.
5. Every claim must have evidence: test, CI run, benchmark, reproducible editor workflow, hardware capture or external user result.
6. Never invent benchmark values, platform support, feature completeness or shipped-game status.
7. Commit directly to main after each coherent verified slice; update ROADMAP.md and docs/STATUS.md and continue.

## Workstream A — platform and performance proof

- Establish a reproducible validation matrix for Linux physical GPU, Windows physical GPU, macOS/Metal and Android physical devices.
- For each available device record OS, device, CPU/GPU, driver, build configuration, quality tier, resolution, frame-time p50/p95/p99, memory, CPU/GPU utilization where measurable, thermal behaviour and run duration.
- Include sustained 10–20 minute mobile runs, lifecycle suspend/resume, memory pressure and battery-aware behaviour.
- Separate software-renderer test evidence from physical GPU evidence.
- Add reproducible sample scenes and automated performance budgets; do not set targets until baselines exist.
- If hardware is unavailable, keep the task explicitly blocked and continue independent work.

## Workstream B — editor as the primary product

Audit complete workflows, not isolated panels:
- Create/open project, import assets, create scene, author materials, build terrain, set up animation, script gameplay, configure physics/AI/audio/UI, debug, profile, test, package and export.
- Identify workflows that require editing JSON or hand-authoring files when a visual editor workflow should exist.
- Prioritize animation state-machine preview/control, asset diagnostics, import settings, prefab workflows, scene validation, debugger usability, profiling and reliable undo/redo.
- Run task-based usability sessions with developers who did not build VICTUS. Record where they get stuck and fix the highest-frequency blockers.
- Preserve fast startup and avoid making the editor a heavyweight mandatory runtime dependency.

## Workstream C — runtime and authoring gaps

Use ROADMAP.md and KNOWN_LIMITATIONS.md as the source of truth. Prioritize by user impact and dependencies:
- Finish animation editor preview and verify state/transition/blend debugging. Evaluate root motion only with concrete use cases.
- Improve animation authoring where justified (IK, retargeting, additive layers, morph targets) only after documenting requirements and testing importer/runtime architecture.
- Improve asset pipeline diagnostics, reimport stability, missing-reference recovery and packaging validation.
- Add missing gameplay/editor workflows only when a real sample project needs them.
- Keep 2D and 3D as first-class workflows.
- Strengthen physics, navigation, AI, audio and UI authoring where the editor currently forces workarounds.

## Workstream D — production and ecosystem foundations

- Design a stable plugin/module SDK with versioned public APIs, extension lifecycle, compatibility policy and migration tests before promising plugin compatibility.
- Add crash reports, symbol handling, diagnostics bundles and explicit privacy controls.
- Investigate source-control integration, safe project diffs/merges and collaboration conflict handling.
- Improve localization, accessibility, tutorials and generated API documentation.
- Establish release automation, changelog, security response and dependency update procedures.
- Do not add a large ecosystem surface before the core APIs and versioning policy are stable.

## Workstream E — proof through complete games

- Bring the 2D and 3D reference games to coherent, externally playable states: title/menu flow, settings, audio mix, save/load, accessibility basics, packaging and clear completion.
- Build a simulation reference with population scale, simulation LOD, persistence and headless mode.
- Run each sample from a clean checkout and through the packaged player.
- Recruit external testers who have not worked on VICTUS; record setup time, task completion, crashes, workarounds and qualitative feedback.
- Do not claim "production-ready" until an external developer can independently create, debug, build and package a complete game with documented limitations.

## Workstream F — honest competitor benchmark

Create a dated comparison against current stable releases of Godot, Unity and Unreal, plus other relevant open-source engines where useful. Use primary documentation and reproducible tests. Compare by workflow and evidence, not marketing bullet points:
- Project onboarding and editor usability.
- 2D and 3D authoring completeness.
- Rendering quality and real-device performance at equivalent quality settings.
- Animation, rigging and character workflow.
- Physics, navigation, AI, audio and UI authoring.
- Asset import, reimport, LODs, packaging and platform export.
- Debugging, profiling, tests, crash recovery and diagnostics.
- Extensibility/plugin SDK, docs, community/ecosystem and release stability.
- Platform/device coverage and external production proof.

For each row classify VICTUS as **verified competitive**, **competitive in a narrow area**, **behind**, **not yet implemented**, or **unknown/unmeasured**. Cite sources, versions and dates. Do not use a single synthetic score to hide weaknesses. Identify the 3–5 highest-impact gaps and update roadmap ordering based on evidence.

## Workstream G — whole-engine capability and subsystem robustness audit

This workstream applies the same engineering standard to **every major VICTUS system**, not only character animation, the editor or whichever feature is currently visible. The goal is a coherent engine whose systems are dependable together under real game workloads, across supported hardware and failure conditions.

### G1. Audit before adding more systems

Create `docs/audits/ENGINE_CAPABILITY_AND_ROBUSTNESS_AUDIT.md` from the actual current `main` source, tests, CI configuration, sample projects and documentation. Do not infer implementation from a roadmap checkbox, panel name, public API declaration or demo.

For every subsystem:
- Identify the real implementation files, public interfaces, owners/dependencies, runtime consumers, editor surfaces, sample-project usage and existing tests.
- Trace at least one complete creator-to-runtime workflow, including import/configuration, authoring, execution, debugging, save/reload and export where relevant.
- Classify every capability **implemented / partial / experimental / missing / blocked / needs validation**. Separate code existence from usable authoring, integration, platform validation and production maturity.
- Record concrete limitations, failure modes, duplicate/overlapping implementations, architectural coupling, undocumented contracts and workarounds.
- Link each finding to source paths, tests, reproducible steps and the relevant status/limitation entry. Never claim a whole subsystem has been audited unless its implementation and consumers have actually been traced.
- Rank findings by developer impact, severity, frequency, dependency leverage, performance cost and evidence confidence. Do not turn every conceivable feature into a priority.

### G2. Required subsystem coverage

The audit must explicitly cover all of the following, including systems that appear mature:

1. **Core runtime and architecture:** frame lifecycle, fixed/variable timestep boundaries, ECS queries and structural changes, job scheduling, thread safety, synchronization, memory ownership/allocation, handles and resource lifetime, module boundaries, shutdown/restart and failure propagation.
2. **Platform layer and RHI:** window/input/audio lifecycle, graphics backend contracts, GPU resource lifetime, swapchain recreation, device-loss detection/recovery strategy, shader compilation/cache, render graph correctness, validation layers, backend fallbacks and platform-specific behaviour.
3. **Rendering and visual quality:** 2D and 3D pipelines, materials/shaders, lighting/shadows, GI/reflections, transparency, post-processing, temporal stability, particles integration, LOD/occlusion/instancing, texture formats/streaming, resolution scaling, quality tiers, bandwidth and memory pressure on tile-based/mobile GPUs.
4. **Physics and character simulation:** Jolt and Box2D parity where appropriate, collision filtering/layers, continuous collision detection, queries, constraints/joints, character controllers, fixed-step stability, sleeping, scale extremes, tunnelling, contact events, determinism limits and performance under dense scenes.
5. **Animation and physical character behaviour:** import/skeleton correctness, controller/state-machine authoring, blend spaces, events, root motion, retargeting, IK, additive layers, morph targets, procedural animation, animation/physics blending and debugging. Explicitly research a VICTUS-owned **physics-driven character animation** path: passive ragdolls, active ragdolls, joint motors, balance/posture control, impact response, bracing, recovery/get-up behaviour, environmental interaction and transitions between authored animation and physical simulation. Treat NaturalMotion Euphoria as a reference for capability and integration lessons, not code to copy or a promise of equivalence. Evaluate licensing and open alternatives before adopting dependencies.
6. **Gameplay AI and navigation:** behaviour-tree runtime and authoring/debugging, perception, pathfinding, steering, crowd avoidance, dynamic obstacles, navigation rebuilds, agent scale, deterministic/replay implications and graceful failure when paths become invalid.
7. **Audio:** device lifecycle, mixing and buses, spatialisation, attenuation, doppler, occlusion/obstruction, effects/reverb, streaming, voice limits, latency, mobile resource use, authoring and audible validation on physical devices.
8. **In-game UI and input:** layout/anchors/scaling, resolution and aspect-ratio changes, focus/navigation, controller and touch flows, text shaping, accessibility, localization/RTL, animation, data binding, UI performance and visual authoring.
9. **Assets and content pipeline:** import validation, stable IDs, metadata, dependency graphs, reimport safety, hot reload, cooking/cache invalidation, compression/variants, streaming, missing-reference recovery, large-file handling, cancellation and corrupted-input behaviour.
10. **Scenes, prefabs, persistence and capture:** serialization/version migrations, references and overrides, transactional save, crash recovery, checkpoints/save games, deterministic capture coverage, compatibility and recovery from partial/corrupt data.
11. **Scripting and visual scripting:** Luau API coverage and consistency, lifetime safety, error isolation, sandbox/permissions, hot reload, garbage-collection pressure, execution budgets, debugging, graph validation/compiler diagnostics and API compatibility.
12. **Networking and multiplayer:** authority boundaries, authentication/encryption, validation of untrusted packets, replication cost, prediction/interpolation, bandwidth/latency/loss/jitter, reconnects, session lifecycle, server scale, abuse resistance, soak tests and real-network validation beyond loopback.
13. **2D, terrain, particles, sequencer and world systems:** each must be evaluated as a complete authoring/runtime workflow, not merely a renderer or runtime data structure. Include collision, culling, batching, streaming, memory, editing, undo, large scenes and platform fallbacks.
14. **Editor and developer tools:** project creation, scene/asset authoring, undo/redo, autosave/recovery, diagnostics, debugging, profiling, test workflows, build/export, accessibility, discoverability, keyboard operation, responsiveness and external beginner usability.
15. **Build, packaging and distribution:** clean-checkout reproducibility, dependency/licence notices, platform-specific packaging, signing, installers, patch/update strategy, content validation, release automation and rollback.
16. **Security, extensibility and ecosystem:** third-party dependency risk, untrusted project/assets/scripts, plugin API/versioning, extension lifecycle, mod sandbox and permissions, supply-chain controls, crash reports and privacy-preserving diagnostics.
17. **Documentation, samples and supportability:** API reference, tutorials, migration guides, known limitations, error messages, sample completeness, docs-to-code consistency and whether a new developer can diagnose common failures without maintainer help.

### G3. A common robustness contract for every subsystem

For each subsystem, define and verify the relevant parts of this contract. Do not force irrelevant requirements onto a system; document why an item does not apply.

- **Correctness:** explicit invariants, edge cases, invalid-state handling and compatibility rules.
- **Lifecycle:** initialization, repeated use, cancellation, hot reload, suspend/resume, shutdown and recovery.
- **Failure behaviour:** actionable errors, no silent corruption, safe fallback or clearly bounded failure, and recoverability where technically possible.
- **Integration:** at least one real consumer in a sample/game/editor workflow; verify boundaries with adjacent systems.
- **Performance and scale:** representative workloads, profiling, memory/CPU/GPU budgets, stress tests and measured low-end fallback paths. Establish baselines before setting numeric targets.
- **Concurrency:** thread ownership, race/deadlock review, stress/sanitizer coverage and deterministic hand-off where relevant.
- **Security and data safety:** validate untrusted inputs, enforce trust boundaries, avoid unsafe deserialization and protect project/user data.
- **Platform behaviour:** explicit support matrix, platform-specific tests and physical-device evidence where required. A successful compile is not a runtime validation.
- **Diagnostics:** useful logs, actionable error context, counters/traces and a reproducible way to investigate failures.
- **Tests:** unit tests for invariants; integration tests across boundaries; end-to-end workflow tests; regression tests for fixed defects; fuzz/property/mutation tests for parsers and untrusted inputs; soak/stress tests for long-running systems; performance tests where relevant.
- **Creator experience:** discoverable authoring and configuration, validation before runtime, clear feedback, undo/recovery for destructive edits and documentation for ordinary workflows.
- **Documentation:** supported/unsupported behaviour, limits, examples, version/compatibility notes and tested commands must match the code.

### G4. Evidence and completion gates

Maintain a subsystem matrix in the audit with columns for capability, implementation, authoring, integration, tests, performance evidence, platform evidence, known limits, severity, owner/next slice and acceptance criteria.

A subsystem must not be labelled **robust** merely because it compiles or has unit tests. Its completion gate is:
1. The implementation and all relevant consumers have been inspected.
2. Its public contract and failure modes are documented.
3. A representative workflow works end-to-end in a real sample or game.
4. Relevant unit, integration, regression and stress/fuzz tests pass.
5. Performance and memory are measured against representative workloads; budgets are based on evidence.
6. Supported platform claims match actual test evidence.
7. Diagnostics and recovery/fallback behaviour are verified.
8. Documentation and editor workflows are usable by someone other than the subsystem author.
9. The diff contains no duplicate replacement system, dead scaffolding or disconnected UI.
10. Status and roadmap entries link to the commit, test commands/results and remaining limitations.

Use a dependency-ordered sequence of small vertical slices. Fix correctness and architectural foundations before adding advanced parity features. Keep the engine usable between slices. If hardware or an external service is unavailable, mark only the affected validation blocked and continue independent work.

**Do not interpret this audit as an instruction to implement every advanced feature immediately.** First establish what VICTUS already does, what real projects need, what is missing, and what can be delivered reliably. The objective is depth, integration and dependable behaviour—not an ever-growing checklist or superficial feature count.

## Definition of success

VICTUS is materially more mature when:
- Independent developers can complete end-to-end workflows without maintainer intervention.
- The editor supports the common workflows without requiring manual file editing for ordinary tasks.
- Supported platforms are tested on real hardware and the matrix accurately reflects reality.
- Performance claims have repeatable baselines and regression gates.
- Reference games build, run and package from clean checkouts.
- Failures are diagnosable and recovery behaviour is predictable.
- Public APIs, documentation, release process and limitations are clear.
- External users can successfully finish representative tasks and provide evidence of usability.

**Do not promise that this program will make VICTUS universally "better than" mature commercial engines.** The goal is to become demonstrably excellent in chosen areas while closing the gaps that prevent developers from trusting it with real projects.


## Additional Workstream H — camera-responsive visual fidelity and cross-system budgets

Coordinate this workstream with [VICTUS Camera-Responsive Visual Fidelity and Per-View Budgets](VICTUS_CAMERA_RESPONSIVE_VISUAL_FIDELITY.md) and Vectors. The central risk is not simply that a distant object receives too few triangles; it is that one subsystem makes a locally reasonable decision that becomes wrong when the projection, viewport, render target or secondary view changes.

### H1. Audit and close the camera-zoom edge case

The current main-view LOD calculation already includes render height and camera vertical FOV. That is a useful base, but current gpu_lod_tests do not establish a dedicated FOV/zoom regression. Require a fixed-camera FOV sweep, dolly comparison, rapid zoom cycle, dynamic-resolution sweep and different viewport dimensions.

Audit whether every render view has independent geometry selection and visibility. A main-camera LOD should not be reused blindly for an editor viewport, secondary camera, reflection/probe capture or light-space shadow view. A second view must have an explicit cost in the total frame budget.

### H2. Treat geometry, texture and shading as different cost/quality systems

- Geometry LOD controls triangles and silhouette error.
- Mip filtering selects a texture level from the rendered texture footprint; mip residency/streaming is a separate asset and memory-management feature.
- Material/shader complexity, anisotropic sampling, transparency/overdraw and shadow maps need separate counters and controls.
- Verify texture mips in every imported/cooked path. Do not claim view-prioritized mip streaming until requests, uploads, eviction, fallbacks and memory accounting exist.
- Evaluate whether the workload is CPU submission, vertex/geometry, pixel/fill, bandwidth, shadows, animation, memory or thermals before attributing a frame-time improvement to Vectors.

### H3. Robustness and validation gates

Add the following cross-system tests to the subsystem matrix:

1. Static scene at wide/default/narrow FOV with the same camera position.
2. Camera dolly and FOV zoom that result in approximately equal object screen coverage.
3. Main and secondary views with different FOVs and render-target sizes.
4. Shadow and reflection/probe captures where off-screen main-view objects still contribute.
5. Dynamic resolution plus TAA/temporal upscaling, including quality recovery when render scale increases.
6. Texture close-up after a low-detail distant view, verifying mip correctness separately from streaming claims.
7. Character face/hands/silhouette over multiple animation poses and camera distances.
8. Thin railings, foliage, alpha-cutout surfaces and terrain chunk boundaries during zoom and transitions.
9. Rapid zoom/camera cuts and memory pressure, including fallback and lifecycle behaviour.
10. Sustained physical Android runs on named low/mid/high device classes when available.

Record CPU time, actual GPU time where available, draw/triangle counts, memory, image comparisons and frame-time percentiles. The current profiler's GPU wait time must not be reported as GPU execution time. Physical-device/thermal evidence is a separate gate from desktop software-renderer tests.

### H4. Definition of success

The engine is not accepted because it draws fewer triangles. It is accepted when the same representative scene preserves the required visible detail across camera and resolution changes, uses a measured lower or better-allocated workload when geometry is limiting, has no unacceptable popping or missing shadow/probe contributions, and stays inside memory and sustained-device budgets. If another subsystem is the bottleneck, the report must say so.

