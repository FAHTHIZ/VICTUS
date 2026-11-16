# VICTUS

**An independent, cross-platform engine for real games, high-fidelity rendering, and large-scale simulation.**

VICTUS is a general-purpose engine project pursuing the breadth of a modern game engine: developers should be able to build and ship complete, playable games with it—not just run simulations or view technical demos. It also aims to support demanding worlds and complex simulations, from capable desktop systems to constrained mobile devices. Auroria may become one demanding showcase, but VICTUS must never depend on a single game.

**Status:** early foundation, not a production-ready engine. The goals below describe the intended product, not capabilities already delivered. A feature is complete only when implemented, integrated, tested, and demonstrated in a real workflow.

## Product goals

- **Complete gameplay:** a real game runtime and gameplay framework with input, cameras, scenes, entities, physics, collision, animation, audio, UI, scripting/logic, save systems, and the foundations needed to build complete interactive games.
- **Actual rendering:** a real graphics pipeline—not mock screens or placeholder visuals—with geometry submission, materials, lighting, shadows, post-processing, asset streaming, and scalable quality tiers. Pursue high-end visual fidelity while supporting standard and mobile hardware.
- **Deep simulation:** scalable systems for populations, economies, ecosystems, physics, and autonomous agents, with simulation correctness independent of camera visibility and render frame rate.
- **Professional creation tools:** an integrated editor, world and scene tools, asset import and processing, animation and material workflows, debugging, profiling, and packaging.
- **Cross-platform delivery:** a modular runtime and practical build, deployment, and optimization workflows for desktop and mobile, with additional platforms evaluated through research and testing.
- **Independent engineering:** VICTUS owns its core architecture and developer experience. Specialized open-source libraries may be integrated behind replaceable interfaces when justified; another complete engine must not provide VICTUS's core.

## Engineering principles

1. Research major architectural choices; record evidence, trade-offs, and rejected alternatives.
2. Measure before optimizing. Publish reproducible benchmarks rather than unsupported performance claims.
3. Own the engine architecture while reusing proven specialist libraries where sensible.
4. Keep gameplay, simulation, and rendering properly decoupled.
5. Treat mobile CPU, GPU, memory, bandwidth, startup time, and thermals as first-class constraints.
6. Provide scalable quality settings and graceful degradation without silently breaking gameplay or simulation correctness.
7. Treat the editor, importer, debugger, profiler, and packaging pipeline as essential engine systems.
8. Finish each subsystem as an integrated, tested, documented capability before marking it complete.
9. Keep the main branch buildable and remove obsolete or duplicate implementations.
10. Respect source-code, asset, license, patent, and trademark rights.

## Repository

`include/victus/` public headers · `src/` engine/runtime · `tests/` tests · `docs/` specifications · `tools/` tooling · `.github/workflows/` CI · `AGENTS.md` agent contract · `ROADMAP.md` implementation plan.

## Build

Requires CMake 3.24+ and a C++20 compiler:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
./build/victus_runtime
```

For Visual Studio/Xcode, run the executable from the matching configuration output directory. See [CMake presets](CMakePresets.json).

## Start here

Read [AGENTS.md](AGENTS.md), [ROADMAP.md](ROADMAP.md), [architecture](docs/ARCHITECTURE.md), [technology decisions](docs/TECHNOLOGY_DECISIONS.md), [research register](docs/RESEARCH_REGISTER.md), [editor spec](docs/EDITOR_SPEC.md), [performance strategy](docs/PERFORMANCE_STRATEGY.md), [platform strategy](docs/PLATFORM_STRATEGY.md), [license policy](docs/THIRD_PARTY_AND_LICENSES.md), and [status](docs/STATUS.md).

## License

Original VICTUS code: Apache-2.0. Third-party components retain their own licenses and notices.
