# Autonomous Engineering Instructions — VICTUS

Read this entire file before editing. This is the operating contract for every coding agent.

## Mission
Build VICTUS into a credible general-purpose, simulation-first, cross-platform engine with a professional editor, asset pipeline, scalable renderer, profiling tools and mobile-aware runtime. Auroria may be its first showcase, never a hard-coded dependency.

The owner explicitly authorizes incremental commits directly to **main**. Do not create branches or pull requests unless the owner changes that instruction.

## Continuous-work mandate
Work autonomously through the roadmap for as long as the current execution environment permits.
- Do not stop after plans, documentation, scaffolding, screenshots or one subsystem.
- After each coherent verified unit, commit directly to main, update ROADMAP.md and docs/STATUS.md, then immediately select the next unblocked task.
- Continue until the next useful task is genuinely blocked by missing access, hardware, credentials, an owner decision or an external dependency.
- Do not wait for the owner for ordinary engineering decisions. Make conservative, reversible choices and document them.
- If more research is needed, do it; then implement and validate. Do not use research to avoid implementation.
- Never claim work will continue after the agent session/environment ends. Persist exact progress, next action, commands, results and blockers so the next session resumes without repeating work.
- Never claim code was built, tested, profiled, installed on a device or pushed unless that action actually succeeded.

## Pace
Move quickly without sacrificing correctness.
1. Ordinary reversible decisions: about 15–30 minutes focused research, then decide and record rationale.
2. High-risk foundations (graphics API, renderer, ECS, scripting, serialization, asset formats, mobile, licensing): 60–90 minutes or more when evidence warrants it; compare at least two credible alternatives.
3. Each work cycle produces a concrete artifact: tested code, benchmark, ADR, usable editor workflow, reproducible build or decision-changing research.
4. Prefer small vertical slices over speculative rewrites.
5. Keep main green. Fix or revert a breaking change before starting another feature.
6. Establish baselines before setting performance targets. Never invent deadlines or benchmark numbers.

## Required loop — repeat
1. Inspect branch, working tree, latest commits, roadmap, CI and existing code. Never overwrite unseen work.
2. Choose highest-priority unblocked roadmap item.
3. Research primary docs, upstream source, release notes, issues, benchmarks and license texts. Record versions, dates, URLs and uncertainty.
4. Write a short decision record before irreversible/architectural work.
5. Implement the smallest complete vertical slice; avoid duplicate systems and permanent temporary layers.
6. Format, compile, test, run integration checks, inspect warnings and supported platforms.
7. Audit diff for dead code, generated files, license issues, secrets, unsafe deserialization and regressions.
8. Commit to main only after checks pass. Use descriptive conventional commits; never force-push.
9. Update status: only verified work complete; record SHA, commands/results, next task and blockers.
10. Immediately repeat.

## Main-branch safety
Work directly on main. Before each commit inspect/fetch latest main. Never force-push, rewrite history, reset away owner work, delete unseen files or use destructive bulk commands. Reconcile new commits safely. Serialize branch writes. Split large work into buildable/testable slices.

## Research and IP
Study public documentation, public source and documented behavior from Unreal, Unity, Godot, O3DE, Bevy, Flax, Stride, Filament, Wicked Engine, bgfx, Diligent and other relevant projects.
- Never copy proprietary Unreal/Unity source, leaked code, private assets, trademarks or non-redistributable content.
- Verify exact upstream version, LICENSE, bundled notices, platform support, maintenance, build requirements, patent terms and security advisories.
- Search snippets are discovery aids, not license proof.
- Maintain docs/RESEARCH_REGISTER.md, docs/TECHNOLOGY_DECISIONS.md and docs/THIRD_PARTY_AND_LICENSES.md.
- Adopt a dependency only with a concrete reason, migration seam and verified license.

## Architecture rules
- C++20 baseline initially; consider C++23 only after compiler/platform validation.
- RAII, explicit ownership, narrow interfaces, const-correctness and clear errors.
- Platform APIs, graphics backends, physics, audio, scripting and editor UI behind VICTUS-owned interfaces.
- Avoid global mutable state and hidden dependencies.
- Separate editor-only code from runtime builds.
- Separate authoritative simulation from rendering, camera and frame-rate timing.
- Measure layouts before optimizing.
- Determinism where feasible; document nondeterminism.
- Controlled simulation steps, bounded catch-up and explicit overload policy.
- Treat serialization/assets as untrusted input.
- Stable asset IDs, not only file paths.
- Headless runtime/CI without graphics or audio initialization.
- Define plugin/module boundary before promising binary plugin compatibility.

## Quality gates
Every feature needs relevant tests, diagnostics, docs, platform/fallback notes and performance evidence when performance is claimed. Enable compiler warnings; add clang-format/clang-tidy; run CTest Debug/Release; CI desktop matrix; sanitizers where supported. No secrets, private data, unlicensed assets, placeholders described as production-ready or fabricated benchmarks.

## Performance rules
Benchmark Release builds on named hardware, OS, driver and power/thermal state. Track CPU/GPU frame time, simulation tick time, memory, allocations, asset decode, streaming, startup and mobile thermals. Report percentiles/variance where meaningful, warm/cold cache and equivalent quality settings. Optimize measured bottlenecks. Preserve simulation invariants when reducing detail/frequency.

## Dependency policy
Original VICTUS code uses Apache-2.0. Review copyleft, source-available, custom, non-commercial, patent, trademark and asset terms separately. Maintain SPDX identifiers, notices and SBOM. Pin versions and document upgrades. Keep dependency-light core build possible.

## Priority
Follow ROADMAP.md: (1) reproducible build/tests/CI/diagnostics, (2) profiler/benchmark/platform layer, (3) ECS/jobs/simulation, (4) graphics API spike/RHI, (5) asset pipeline/cooking, (6) editor slice, (7) renderer/streaming, (8) physics/animation/navigation/audio/scripting, (9) procedural tools/advanced rendering, (10) mobile/release hardening.

## Ask the owner only when
An irreversible product-direction, licensing/business, privacy/security or branding decision is needed, or credentials/physical hardware are unavailable. For ordinary implementation details, decide and proceed. When blocked, continue independent work and record exact blocker, attempts, required input and next action.

## Completion standard
Not a README or demo screenshot. VICTUS requires a working runtime, capable editor, asset pipeline, scalable renderer, simulation framework, profiler/diagnostics, cross-platform packaging, tests, examples and reproducible performance evidence.

**After every verified commit, continue to the next highest-priority unblocked task.**
