# VICTUS Architecture

## Product boundary
VICTUS is a general-purpose engine for complete 2D/3D games, real-time rendering, simulation, visualization and dedicated server workloads. It owns its engine architecture and creator workflow. Specialist libraries may be integrated behind VICTUS interfaces after technical, maintenance, platform and license review. Another complete engine must never provide the core runtime.

## System layers
~~~text
Creator experience: Studio | CLI | Graph editors | Debuggers | Profilers
Project/build services: Project model | Asset database | Import/cook | Package | Test
VICTUS APIs: Gameplay | World/ECS | Tasks | Simulation | Physics | Navigation | AI
             Rendering/RHI | 2D | Animation | Audio | Input | UI | Networking | Scripting
Platform adapters + audited specialist dependencies
Windows | Linux | macOS | Android | iOS | optional XR/Web/console targets
~~~

Dependencies point inward toward VICTUS-owned contracts. Public APIs should not expose third-party types without a documented architectural decision.

## Module map

Every engine module is a static library `victus_<name>` (alias `VICTUS::<Name>`) under `engine/<name>/`, with public headers in `include/victus/<name>/`. Layers are enforced by `cmake/VictusTargets.cmake` (`VICTUS_MODULE_LAYERS`): a module may link only modules on a strictly lower layer, so the configure fails on an upward dependency or a cycle. Third-party libraries are linked privately unless a public header needs them (SDL3, Luau, Jolt, Box2D, miniaudio, Recast, ENet and cgltf are all private).

| Layer | Modules | Owns |
|---|---|---|
| 0 | `core` | Result/errors, logging, JSON, filesystem and packs, jobs, memory, profiler, math, hashing |
| 1 | `platform` | SDL3 windows, input, lifecycle, clipboard |
| 1 | `ecs` | Archetype world, queries, command buffers |
| 1 | `shadercompiler` | GLSL to SPIR-V/MSL at run time (glslang, SPIRV-Cross) |
| 1 | `graph` | Typed node-graph documents, validation, serialization, compilers to Luau source and GLSL surface functions (no Luau or GPU dependency) |
| 2 | `rhi` | Device, resources, pipelines, swapchain on SDL_GPU |
| 2 | `scene` | Entities with UUIDs, hierarchy, transforms, reflection and serialization |
| 3 | `render` | Render graph, SceneRenderer (3D), SpriteBatch (2D), built-in shaders |
| 3 | `gameplay` | Fixed-step Simulation, systems, input sources, gameplay components |
| 4 | `ui` | Dear ImGui on the RHI (editor and debug UI) |
| 4 | `physics`, `animation`, `particles`, `audio`, `gameui` | Jolt/Box2D bodies and characters; clips and skinning; CPU emitters; miniaudio; in-game UI components |
| 5 | `navigation`, `tilemap`, `terrain`, `sequencer`, `net` | Navmesh and agents; tile sets and colliders; heightfields and foliage; timeline playback; transport, replication, sessions |
| 6 | `ai` | Behaviour trees, blackboard, perception |
| 6 | `assets` | Asset database, importers, cooker, GPU-ready model/texture data |
| 7 | `script` | Luau ScriptHost and the scripting API over every gameplay system; loads `.vgraph` through `graph` |
| 8 | `runtime` | Scene to renderer extraction, SceneView, GPU asset cache, surface shaders (material graphs through `graph`), hot reload |
| 8 | `project` | Project manifest, templates, component registration for every module |
| 9 | `player` | Game loop for `victus run`, the exported player and Android |

Above the engine: `editor/` (VICTUS Studio, links `player`-level modules and `ui`), `tools/cli` (`victus`), `apps/player`. Rule of thumb: put code in the lowest module that has everything it needs; integration code that wires several systems together belongs in `script`, `runtime`, `project` or `player`, not in a system module.

## Runtime modes and lifetimes
Separate editor, game runtime, headless simulation, dedicated server, asset cooker, test runner and benchmark targets. Headless/server builds must not initialize graphics, windowing or audio devices. Editor dependencies and authoring metadata must not leak into shipped game binaries. Define startup/shutdown order and ownership explicitly.

## Gameplay and world model
Provide a coherent gameplay framework above the lower-level ECS/world model: game instance/session, world/level lifecycle, rules/modes, state, players, controllers, pawns/characters, cameras, spawning, components, events, timers and reusable prefabs. Avoid forcing every game to inherit one rigid object hierarchy; allow data-oriented entities and higher-level gameplay objects to coexist through explicit adapters.

Gameplay systems consume authoritative world state. Editor tools operate through undoable commands. Play-in-editor runs an isolated world/snapshot so stopping play cannot corrupt authored content.

## Simulation/presentation separation
Authoritative simulation must not depend on camera visibility, render frame rate or editor viewport. Simulation uses controlled time and explicit overload policy; rendering consumes safe snapshots. LOD may change detail/frequency only under declared invariants. Distant regions may use aggregates/event-driven progression. Save data includes schema version, random seeds, authoritative state and pending events. Headless and graphical runtime share simulation rules.

## Data-oriented execution
Benchmark archetype ECS, sparse-set ECS, SoA and hybrid layouts on VICTUS workloads. Scheduler expresses dependencies, read/write access, affinity, cancellation and priorities. Keep data ownership and synchronization visible; do not add abstractions without measurable benefit.

## Rendering architecture
A VICTUS-owned RHI abstracts devices, queues, resources, descriptors, synchronization, surfaces and capabilities. Render graph owns resource lifetimes and transient allocation. Keep backend selection explicit and testable. Renderer layers include:
- Shared scene extraction, visibility, materials, lights, cameras and render features.
- 3D pipeline: mesh rendering, PBR, lighting, shadows, reflections, post-processing, terrain and large-world streaming.
- 2D pipeline: sprites, atlases, batching, tilemaps, sorting/layers, 2D cameras, lighting and 2D effects.
- Shared VFX, particles, debug visualization, capture and profiling.
- Quality profiles with capability checks, fallbacks and user overrides.

Do not promise parity across devices. A feature is supported only on validated backend/device combinations.

## Gameplay subsystems
Physics, navigation, animation, audio, UI, input, AI, scripting, visual scripting and save/load have VICTUS-owned interfaces, explicit ownership and testable contracts. Scripting (implemented, `engine/script`) runs Luau inside a `ScriptHost` that is registered as Simulation systems; scripts reach the world only through the scene, serializer registry and `gameplay::InputSource`, so the same scripts run in the editor, the player and headless `victus simulate` (servers, CI). Inside `engine/script/src`, `script.cpp` owns `ScriptHost` (VM lifetime, instances, per-frame dispatch) and `open_victus_libs`, the one table that registers the whole Lua API; `script_lua.cpp` holds VM helpers, JSON <-> Lua conversion and the Entity userdata; each `bind_<area>.cpp` (scene/input, physics, render/UI/animation, mesh, net, world = terrain/nav/AI/sequencer, audio, save, bridge) owns one subsystem's bindings. `script_internal.hpp` (not installed) defines `ScriptHost::Impl` and declares only the helpers used across those files. Gameplay AI (decision-making/perception) is distinct from simulation agents (large-scale autonomous population behavior).

## Networking
Networking is server-authoritative by default for competitive/shared-state games. Separate transport, serialization, replication, prediction, reconciliation, interpolation, relevancy, session services and game-specific rules. Headless server uses the same authoritative gameplay/simulation code as the client. Online service providers are replaceable integrations, not engine dependencies.

## Asset and build pipeline
Source assets are immutable inputs. Importers validate and convert to versioned cooked artifacts. Stable IDs survive file moves. Dependency graphs support incremental cooking, cache invalidation, platform variants and provenance. Runtime loads cooked artifacts. Build graph records inputs, tool versions, settings and outputs for reproducibility.

## Editor and production tools
Studio is a client of project, asset and runtime services—not the owner of game state. Scene edits are undoable commands. Specialized tools share serialization, validation, graph execution, undo, diagnostics and preview infrastructure. Asset import and preview run asynchronously and report errors. Build/package tools provide actionable diagnostics.

## Extensibility and safety
Version plugin APIs; define module lifetime, capability permissions, ABI compatibility and failure isolation before promising binary compatibility. Mod content is untrusted. Validate paths, sizes, references, decompression limits, script execution and package signatures. Keep project, scene, asset, network and save schemas versioned and migratable.

## Validation
Every subsystem needs unit tests, integration tests, reference-project use, performance evidence where relevant, platform/fallback documentation, dependency/license review and migration strategy.
