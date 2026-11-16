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
Physics, navigation, animation, audio, UI, input, AI, scripting, visual scripting and save/load have VICTUS-owned interfaces, explicit ownership and testable contracts. Scripting (implemented, `engine/script`) runs Luau inside a `ScriptHost` that is registered as Simulation systems; scripts reach the world only through the scene, serializer registry and `gameplay::InputSource`, so the same scripts run in the editor, the player and headless `victus simulate` (servers, CI). Gameplay AI (decision-making/perception) is distinct from simulation agents (large-scale autonomous population behavior).

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
