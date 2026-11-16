# VICTUS Reference Projects and Proof of Capability

These projects are product acceptance tests, not marketing demos. They must be built with public VICTUS workflows and engine systems. Do not hide missing capabilities in project-specific engine forks or hardcoded workarounds.

## 1. VICTUS 2D Game
A complete small 2D game with a title/menu flow, player controls, tilemap or level composition, collision/physics, animation, UI, audio, save/checkpoint behavior, effects and a packaged build.

**Proves:** 2D renderer, sprite/atlas pipeline, tilemap editor, 2D physics, animation, input, UI, audio, save/load, test runner and packaging.

**Acceptance:** clean checkout builds and runs; gameplay smoke tests cover start, movement, collision, level transition, save/load and completion; no editor-only dependency in shipped runtime.

## 2. VICTUS 3D Gameplay Game
A complete third-person or first-person playable game slice with camera, movement, interaction, AI opponents/allies, physics, animation, materials, lighting, audio, UI, checkpoints, scripted events and at least one cinematic sequence.

**Proves:** 3D renderer, PBR, asset pipeline, gameplay framework, prefabs, scripting/visual logic, physics, navigation, AI debugging, VFX, cinematics, UI, audio and build pipeline.

**Acceptance:** playable from clean project; automated tests cover key gameplay loop and save migration; quality profiles run on validated desktop and mobile devices where supported.

## 3. VICTUS Simulation Project
A large world with populations, resources, autonomous agents, regional simulation LOD, time progression, save/reload, deterministic workloads where feasible and headless execution.

**Proves:** ECS/jobs, authoritative simulation, LOD invariants, serialization, streaming, diagnostics, benchmark and server/headless runtime.

**Acceptance:** scripted workload validates population/resource/time invariants before and after LOD transitions, save/reload and long-duration runs; benchmark outputs are reproducible.

## 4. Optional Multiplayer Project
A small networked game with at least two clients and a dedicated authoritative server, replicated state, prediction/reconciliation and simulated latency/loss.

**Proves:** transport/session lifecycle, authority, replication, network diagnostics, headless packaging and network test harness.

**Acceptance:** repeatable local server/client test plus network impairment tests; invalid client state is rejected.

## Project governance
- Keep examples separate from engine implementation and use only public VICTUS APIs.
- Add automated smoke tests and build all reference projects from clean CI checkouts.
- Track missing engine features as roadmap work rather than bypassing them in examples.
- Publish source, controls, build instructions, supported platforms, test results and limitations.
- Use original or properly licensed assets with clear attribution and license records.
- A project is complete only when it can be built, played/tested and packaged without undocumented manual steps.
