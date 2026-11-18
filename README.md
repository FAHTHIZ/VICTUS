# VICTUS

**An independent, cross-platform engine for complete games, real-time rendering, and large-scale simulation.**

VICTUS aims to give developers an end-to-end path from an empty project to a finished, playable, distributable game. It is intended for 2D and 3D games, high-fidelity visuals, simulation-heavy worlds, mobile experiences, multiplayer projects, and dedicated servers. It must provide real gameplay and real rendering—not just technical demos.

**Status:** early; not production-ready. The list below describes product goals, not current features; see [Development status](#development-status) for what works today. A feature counts as delivered only when implementation, integration, tests and a usable workflow prove it.

## What VICTUS is being built to provide

- **Complete gameplay framework:** worlds, scenes, entities, components, game modes/states, player controllers, cameras, input mapping, interaction, physics, navigation, gameplay AI, scripting, visual scripting, events, timers, save/load, and reusable prefabs.
- **Real 2D and 3D development:** dedicated 2D and 3D workflows, sprites, tilemaps, UI, physics, 3D meshes, skeletal animation, materials, terrain, large worlds, and spatial audio.
- **Actual rendering:** a production-oriented graphics architecture with PBR materials, lighting, shadows, reflections, post-processing, GPU profiling, particles, visual effects, atmosphere, water, scalable quality tiers, and researched advanced rendering paths.
- **Complete creator tools:** VICTUS Studio, scene/world editing, visual graphs, material/shader tools, animation tools, terrain and procedural tools, cinematic sequencing, asset inspection, debugging, profiling, and play-in-editor.
- **Production pipeline:** asset import and validation, stable asset IDs, dependency tracking, incremental cooking, caching, build automation, packaging, platform presets, crash diagnostics, and reproducible releases.
- **Multiplayer and online foundations:** authoritative server architecture, replication, prediction/reconciliation, interpolation, dedicated/headless servers, network simulation, and replaceable online-service integrations.
- **Large-scale simulation:** deterministic and controlled simulation where feasible, populations, economies, ecosystems, crowds, autonomous agents, world streaming, and validated simulation LOD.
- **Extensibility:** documented VICTUS APIs, plugin/module boundaries, safe modding architecture, versioning, migration tools, and an ecosystem that does not require another complete engine.
- **Cross-platform delivery:** Windows, Linux, macOS, Android, and iOS as researched and validated targets; web, XR, and console support only when technically and legally viable. Console development depends on platform-holder access and approval.
- **Three complete reference projects:** a 2D game, a 3D gameplay-focused game, and a simulation project, all built with VICTUS systems rather than hidden custom workarounds.

## Product principles

1. **Independent core:** VICTUS owns its architecture, runtime, editor experience, project model, and public APIs. Specialist libraries may be used behind replaceable interfaces after research and license review.
2. **Complete, not cosmetic:** no placeholder, mock, or screenshot-only feature counts as implemented.
3. **Finish vertical slices:** implement, integrate, test, profile, document, and demonstrate each subsystem before marking it complete.
4. **Measure performance:** publish reproducible results with hardware, OS, driver, build, quality settings, and test methodology.
5. **Mobile is a first-class constraint:** account for memory, bandwidth, battery, sustained thermals, startup, and device capability.
6. **Separate concerns:** gameplay, authoritative simulation, presentation, rendering, editor, and server execution have clear boundaries.
7. **Build for creators:** usability, undo/redo, recovery, import feedback, debugging, tutorials, and packaging are engine requirements.
8. **Keep main buildable:** use small verified commits, remove obsolete implementations, and never claim unverified support.
9. **Respect rights and safety:** audit code, assets, licenses, patents, trademarks, plugin boundaries, and untrusted project content.

## Development status

VICTUS is early and **not production-ready**: no game has shipped with it, rendering has only been validated on software Vulkan and CI's virtual Apple GPU, and no phone has run it. What does exist is a broad, tested engine through Slice 78: SDL3 platform layer and an RHI on SDL_GPU; a render graph with clustered forward PBR, shadows, SSAO, IBL and sky, reflection probes, irradiance volumes, TAA and temporal upscaling, occlusion culling, LODs, decals and quality tiers; 2D sprites, tilemaps and 2D lights; an archetype ECS and versioned scenes with prefabs; an asset pipeline with stable IDs and a cooked cache; Jolt (3D) and Box2D (2D) physics with character controllers; skeletal animation; particles; miniaudio audio; Recast/Detour navigation; behaviour-tree AI with perception; terrain with sculpting and foliage; a timeline sequencer; in-game UI; Luau scripting and visual scripting; encrypted server-authoritative multiplayer; the VICTUS editor; and `victus export` packaging.

- [Subsystem maturity and known limitations](docs/KNOWN_LIMITATIONS.md) — what is usable, experimental, incomplete or missing
- [Platform support matrix](docs/PLATFORM_SUPPORT.md) — what was actually executed on each platform
- [STATUS.md](docs/STATUS.md) — verified state, slice by slice, with commands and results
- [ROADMAP.md](ROADMAP.md) — prioritised plan with acceptance criteria

## Quick start

~~~bash
tools/setup/install_deps.sh          # Linux/macOS toolchain and SDL3 system headers (Windows: see the guide)
python3 tools/setup/check_env.py     # verify the toolchain
cmake --preset dev && cmake --build --preset dev && ctest --preset dev

B=build/dev/bin
$B/victus new MyGame --template 3d   # templates: 3d, 2d, simulation, multiplayer
$B/victus_editor MyGame              # open it in the editor
$B/victus run MyGame                 # or play it directly
$B/victus export MyGame --out MyGame-export && ./MyGame-export/MyGame
~~~

Dependencies are fetched by CMake from pinned, hash-verified archives; no account or sign-in is needed. Full instructions, presets, Windows notes and troubleshooting: [Getting started](docs/GETTING_STARTED.md). How to use scripting, physics, AI, multiplayer, the editor and the other systems: [User guide](docs/USER_GUIDE.md).

Template controls (a gamepad works too: left stick moves, A/South jumps, View/Back toggles the help panel): 2D platformer — A/D or arrows to run, Space/W/Up to jump, S/Down to drop through the one-way ledge; 3D — WASD/arrows walk the robot, Space jumps; simulation — the camera orbits the wave field; multiplayer — WASD/arrows walk, Space jumps, coins are scored by the server. In the editor, click the viewport after pressing Play so the game receives the keyboard.

## Sample games

Complete games made only with engine features; each is a normal project you can open in the editor or run with `victus run`, and each is played to the end headlessly by a CTest (`ctest -L samples`).

- [`samples/dungeon_crawler`](samples/dungeon_crawler/README.md) — top-down 2D maze game: procedurally generated tilemap floors, autotiled walls with colliders, top-down character controller, gems, slimes and a HUD.
- [`samples/platformer`](samples/platformer/README.md) — Ruin Runner, a side-view 2D platformer: three levels authored as ASCII maps, one-way planks, spikes, stompable beetles, checkpoints, lives and a HUD.
- [`samples/arena`](samples/arena/README.md) — Robot Arena, a third-person 3D arena fight: five waves of robots chase you over the navmesh, melee combat, health pickups and a HUD.
- `samples/render_showcase` — a C++ program that renders the 3D feature set for visual inspection.

## Repository map

- `engine/<module>/` — engine modules, each with `include/victus/<module>/` and `src/`; module ownership and the dependency graph are in [ARCHITECTURE.md](docs/ARCHITECTURE.md):
  - foundation: `core`, `platform`, `rhi`, `shadercompiler`
  - world: `ecs`, `scene`, `assets`, `project`
  - systems: `render`, `physics`, `animation`, `particles`, `audio`, `navigation`, `ai`, `terrain`, `tilemap`, `sequencer`, `gameui`, `net`, `gameplay`
  - integration: `script` (Luau bindings and visual scripting), `runtime` (scene to renderer), `ui` (Dear ImGui), `player`
- `editor/` — the VICTUS editor (library + executable)
- `apps/player/` — standalone game player; `platforms/android/` — Gradle project for APKs
- `tools/` — `victus` CLI (`cli`), shader compiler (`shaders`), dependency manifest and OSV scanner (`deps`), clang-tidy runner (`lint`), setup scripts (`setup`), APK checks (`android`)
- `samples/`, `benchmarks/`, `tests/` — sample games, benchmark harness, automated tests (CTest)
- `docs/` — guides, architecture, specifications, decisions and status
- `AGENTS.md` — engineering contract for autonomous contributors

## Documentation

Using VICTUS: [Getting started](docs/GETTING_STARTED.md) · [User guide](docs/USER_GUIDE.md) · [Known limitations](docs/KNOWN_LIMITATIONS.md) · [Platform support](docs/PLATFORM_SUPPORT.md) · [Gameplay AI](docs/AI.md) · [Sequencer](docs/SEQUENCER.md) · [Android](docs/ANDROID.md)

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md) · [Architecture](docs/ARCHITECTURE.md) · [Roadmap](ROADMAP.md) · [Engineering status](docs/STATUS.md) · [Technology decisions](docs/TECHNOLOGY_DECISIONS.md) · [Third-party and license policy](docs/THIRD_PARTY_AND_LICENSES.md) · [Autonomous engineering contract](AGENTS.md)

Specifications: [Gameplay](docs/GAMEPLAY_SPEC.md) · [Rendering](docs/RENDERING_SPEC.md) · [Networking](docs/NETWORKING_SPEC.md) · [Production pipeline](docs/PRODUCTION_PIPELINE_SPEC.md) · [Editor](docs/EDITOR_SPEC.md) · [Performance](docs/PERFORMANCE_STRATEGY.md) · [Platforms](docs/PLATFORM_STRATEGY.md) · [Reference projects](docs/REFERENCE_PROJECTS.md) · [Research register](docs/RESEARCH_REGISTER.md)

## License

Original VICTUS code: Apache-2.0. Third-party components retain their own licenses and notices.
