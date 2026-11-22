# VICTUS Reference Projects and Proof of Capability

These projects are product acceptance tests, not marketing demos. They must be built with public VICTUS workflows and engine systems. Do not hide missing capabilities in project-specific engine forks or hardcoded workarounds.

## 1. VICTUS 2D Game
A complete small 2D game with a title/menu flow, player controls, tilemap or level composition, collision/physics, animation, UI, audio, save/checkpoint behavior, effects and a packaged build.

**Proves:** 2D renderer, sprite/atlas pipeline, tilemap editor, 2D physics, animation, input, UI, audio, save/load, test runner and packaging.

**Acceptance:** clean checkout builds and runs; gameplay smoke tests cover start, movement, collision, level transition, save/load and completion; no editor-only dependency in shipped runtime.

**Current state (2026-11-22):** two 2D samples, `samples/dungeon_crawler` (a top-down maze with floors generated at run time, gems, slimes and an exit portal) and `samples/platformer` (Ruin Runner: three side-view levels with spikes, beetles, one-way planks and checkpoints). Their CTests (`victus_sample_dungeon_crawler`, `victus_sample_platformer`) run `victus check` and an autopilot play-through with a fixed seed, which must print the completion text. The `2d` and `nocode` templates are further 2D starting points. Missing against this definition: a title or menu flow in the samples, a tilemap built in the editor as the level (the crawler generates its floors), and a packaged-build test per sample.

## 2. VICTUS 3D Gameplay Game
A complete third-person or first-person playable game slice with camera, movement, interaction, AI opponents/allies, physics, animation, materials, lighting, audio, UI, checkpoints, scripted events and at least one cinematic sequence.

**Proves:** 3D renderer, PBR, asset pipeline, gameplay framework, prefabs, scripting/visual logic, physics, navigation, AI debugging, VFX, cinematics, UI, audio and build pipeline.

**Acceptance:** playable from clean project; automated tests cover key gameplay loop and save migration; quality profiles run on validated desktop and mobile devices where supported.

**Current state (Slice 53):** `samples/arena` (Robot Arena) is a third-person arena fight: camera-relative character movement, an orbit camera with occlusion raycasts, five waves of skinned, animated enemy robots instantiated from a prefab and steered over the navmesh, melee swings, health pickups, particles, emissive hit flashes, positional audio, HUD and quick-save checkpoints. The `victus_sample_arena` CTest runs `victus check` and an autopilot play-through that must clear all five waves. Missing against this definition: title/menu flow, a cinematic sequence, visual logic, save migration tests and validated mobile runs.

## 3. VICTUS Simulation Project
A large world with populations, resources, autonomous agents, regional simulation LOD, time progression, save/reload, deterministic workloads where feasible and headless execution.

**Proves:** ECS/jobs, authoritative simulation, LOD invariants, serialization, streaming, diagnostics, benchmark and server/headless runtime.

**Acceptance:** scripted workload validates population/resource/time invariants before and after LOD transitions, save/reload and long-duration runs; benchmark outputs are reproducible.

**Current state (2026-11-22):** no dedicated simulation sample. The `simulation` template has 576 entities driven by native systems and a scripted camera. Simulation LOD (ADR-0165) thins out updates for far AI and traffic agents (`engine/ai`, `engine/traffic`). World streaming keeps the state of unloaded cells and saves it (`tests/world`). `victus simulate` runs headless, and the headless build (`-DVICTUS_WITH_GRAPHICS=OFF`) builds and is tested in CI. Missing: a large-world project with populations, resources and time progression, invariant checks across LOD transitions and long runs, and a reproducible benchmark of that workload.

## 4. Optional Multiplayer Project
A small networked game with at least two clients and a dedicated authoritative server, replicated state, prediction/reconciliation and simulated latency/loss.

**Proves:** transport/session lifecycle, authority, replication, network diagnostics, headless packaging and network test harness.

**Acceptance:** repeatable local server/client test plus network impairment tests; invalid client state is rejected.

**Current state (2026-11-22):** the `multiplayer` template is a coin-collecting arena: `victus serve` hosts it and `victus run --connect` joins, with predicted characters, replicated coins and scores, and a session HUD. `tests/net` covers two clients over a lossy, delayed link, a listen-server host, reconnecting, dropping and kicking malformed or abusive traffic, delta snapshots under loss, and an encrypted session over a lossy link. All of this runs on loopback in one process; nothing has been tested across real networks. See [NETWORKING_SPEC.md](NETWORKING_SPEC.md).

## Project governance
- Keep examples separate from engine implementation and use only public VICTUS APIs.
- Add automated smoke tests and build all reference projects from clean CI checkouts.
- Track missing engine features as roadmap work rather than bypassing them in examples.
- Publish source, controls, build instructions, supported platforms, test results and limitations.
- Use original or properly licensed assets with clear attribution and license records.
- A project is complete only when it can be built, played/tested and packaged without undocumented manual steps.
