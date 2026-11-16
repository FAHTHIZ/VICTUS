# VICTUS

**An independent, cross-platform engine for complete games, real-time rendering, and large-scale simulation.**

VICTUS aims to give developers an end-to-end path from an empty project to a finished, playable, distributable game. It is intended for 2D and 3D games, high-fidelity visuals, simulation-heavy worlds, mobile experiences, multiplayer projects, and dedicated servers. It must provide real gameplay and real rendering—not just technical demos.

**Status:** early; not production-ready. The list below describes product goals; see [Development status](#development-status) for what works today. A feature counts as delivered only when implementation, integration, tests and a usable workflow prove it.

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

VICTUS is early and not production-ready. What exists and is tested today: core runtime (jobs, memory, profiling, JSON, logging), SDL3 platform layer, an RHI on SDL_GPU (Vulkan verified), a render graph, real 2D sprite and 3D PBR/shadow rendering, an archetype ECS, a versioned scene format, a texture/glTF asset pipeline, the VICTUS editor, Luau gameplay scripting with hot reload, a standalone player and the `victus` CLI. Physics, audio, networking and export are not implemented yet. [docs/STATUS.md](docs/STATUS.md) lists exactly what is verified and how; [ROADMAP.md](ROADMAP.md) is the plan.

## Quick start

Build (CMake 3.24+, a C++20 compiler; dependencies are fetched and pinned automatically, no account or sign-in needed):

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
~~~

Create, edit and run a project:

~~~bash
./build/bin/victus templates                         # 3d, 2d, simulation
./build/bin/victus new MyGame --template 3d          # creates MyGame/victus.project, scenes/, assets/
./build/bin/victus_editor MyGame                     # open in the editor (or run with no arguments for the launcher)
./build/bin/victus run MyGame                        # run the game in its own window
./build/bin/victus import MyGame                     # import/cook assets dropped into MyGame/assets
./build/bin/victus check MyGame                      # validate scenes and script syntax
./build/bin/victus simulate MyGame --frames 600 --hold D   # run gameplay + scripts headless (no GPU)
~~~

Template controls: 2D platformer — A/D or arrows to run, Space/W/Up to jump, collect the coins; 3D — WASD/arrows roll the ball and the camera follows; simulation — the camera orbits the wave field. In the editor, click the viewport after pressing Play so the game receives the keyboard.

### Scripting

Gameplay scripts are [Luau](https://luau.org) files in `assets/`. A script returns a table; public fields become editable properties in the inspector, and `start`, `update(dt)`, `fixed_update(dt)` and `reload` are called for each entity that has a Script component pointing at the file. Saving the file reloads it while the game runs.

~~~lua
-- assets/scripts/spin_and_jump.luau
local Spin = {}
Spin.speed = 90          -- degrees per second, editable per entity

function Spin:update(dt)
    self.entity:rotate(vector.create(0, 1, 0), self.speed * dt)
    if input.pressed("Space") then
        self.entity:translate(vector.create(0, 1, 0))
        log.info(self.entity.name, "jumped")
    end
end

return Spin
~~~

Available globals: `scene.find/spawn/destroy/query/count`, `input.down/pressed/released/axis/mouse_*`, `time.delta/now/frame`, `log.info/warn/error`. Entities expose `position`, `rotation` (Euler degrees), `scale`, `name`, `parent`, `forward/right/up`, and `get/set/has/remove` for any component as JSON-like tables. Scripts run sandboxed with time and memory limits, so a bad loop reports an error instead of freezing the editor.

In the editor: Play/Stop runs the scene in place on a copy (edits made during play are discarded), Ctrl+S saves, Ctrl+Z/Ctrl+Y undo/redo, F frames the selection, right-drag orbits, middle-drag pans, the wheel zooms. Drop `.gltf`/`.glb`/`.png`/`.jpg` files into the project's `assets/` folder and press Reimport; drag a model from the asset table into the viewport to instantiate it, or a `.luau` script onto an entity's Script component (double-click attaches it to the selection).

A game can currently be shipped by copying `victus_player` next to a `game/` folder containing the project; running `victus_player` with no arguments starts it. A proper `victus export` command is planned. Your project files are plain JSON and assets you own; Apache-2.0 allows commercial use.

For Visual Studio/Xcode generators, binaries are under `build/bin/<Config>/`. See [CMake presets](CMakePresets.json).

## Repository map

- `engine/<module>/` — engine modules (`core`, `platform`, `rhi`, `render`, `ecs`, `scene`, `assets`, `runtime`, `ui`, `gameplay`, `script`, `project`, `player`), each with `include/victus/<module>/` and `src/`
- `editor/` — the VICTUS editor (library + executable)
- `apps/player/` — standalone game player
- `tools/` — `victus` CLI and shader compiler
- `samples/`, `benchmarks/`, `tests/` — samples, benchmark harness, automated tests
- `third_party/` — vendored dependencies with licence files
- `docs/` — architecture, specifications, research, decisions and status
- `AGENTS.md` — autonomous engineering contract

## Engineering documents

- [Autonomous engineering contract](AGENTS.md)
- [Roadmap](ROADMAP.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Gameplay specification](docs/GAMEPLAY_SPEC.md)
- [Rendering specification](docs/RENDERING_SPEC.md)
- [Networking specification](docs/NETWORKING_SPEC.md)
- [Production pipeline specification](docs/PRODUCTION_PIPELINE_SPEC.md)
- [Reference projects and acceptance](docs/REFERENCE_PROJECTS.md)
- [Editor specification](docs/EDITOR_SPEC.md)
- [Performance strategy](docs/PERFORMANCE_STRATEGY.md)
- [Platform strategy](docs/PLATFORM_STRATEGY.md)
- [Research register](docs/RESEARCH_REGISTER.md)
- [Technology decisions](docs/TECHNOLOGY_DECISIONS.md)
- [Third-party and license policy](docs/THIRD_PARTY_AND_LICENSES.md)
- [Engineering status](docs/STATUS.md)

## License

Original VICTUS code: Apache-2.0. Third-party components retain their own licenses and notices.
