# VICTUS

**An independent, cross-platform engine for complete games, real-time rendering, and large-scale simulation.**

VICTUS aims to give developers an end-to-end path from an empty project to a finished, playable, distributable game. It is intended for 2D and 3D games, high-fidelity visuals, simulation-heavy worlds, mobile experiences, multiplayer projects, and dedicated servers. It must provide real gameplay and real rendering—not just technical demos.

**Status:** early foundation; not production-ready. This README describes product goals, not delivered capabilities. Every feature remains planned until implementation, integration, tests, and a usable workflow prove it.

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

VICTUS is currently at its bootstrap stage. The roadmap is aspirational and must not be confused with implemented functionality. See [ROADMAP.md](ROADMAP.md) and [docs/STATUS.md](docs/STATUS.md) for verified progress.

## Repository map

- `include/victus/` — public engine interfaces
- `src/` — engine and runtime implementation
- `tests/` — automated tests
- `tools/` — developer and content tools
- `docs/` — architecture, subsystem specifications, research, and status
- `.github/workflows/` — CI
- `AGENTS.md` — autonomous engineering contract
- `ROADMAP.md` — complete implementation roadmap

## Build

Requires CMake 3.24+ and a C++20 compiler:

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
./build/bin/victus_bench_core   # core benchmark baseline
~~~

For Visual Studio/Xcode, run the executable from the matching configuration output directory. See [CMake presets](CMakePresets.json).

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
