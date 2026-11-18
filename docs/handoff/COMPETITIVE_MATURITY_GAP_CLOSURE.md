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
