# VICTUS

**A simulation-first, cross-platform real-time engine.**

VICTUS is an independent engine project pursuing high visual fidelity, large-scale simulation and predictable performance from desktop hardware to constrained mobile devices. It is general-purpose; Auroria may become its first demanding showcase, but VICTUS must not depend on one game.

**Status:** early foundation, not a production-ready engine. A feature is complete only when implemented, tested and demonstrated.

## Product goals
- General-purpose 2D/3D runtime for games, visualization and simulations.
- Simulation-first architecture for populations, economies, ecosystems, physics and autonomous agents.
- Scalable renderer with high-end, standard, mobile and compatibility tiers.
- Professional editor, asset pipeline, profiler, debugger and packaging tools.
- Modular open-source ecosystem and cross-platform runtime.

## Principles
1. Measure before optimizing; benchmark claims must be reproducible.
2. Own VICTUS architecture; reuse proven libraries behind replaceable interfaces.
3. Audit every dependency's version, license, platform support and maintenance.
4. Authoritative simulation is independent of camera visibility and render frame rate.
5. Mobile is a first-class constraint: CPU, GPU, memory, bandwidth, startup and thermals.
6. Graceful degradation preserves declared simulation invariants.
7. Editor, importer, debugger and profiler are engine essentials.
8. Build vertical slices; keep main buildable.
9. Never claim completion without acceptance tests.
10. Respect source, asset, license, patent and trademark rights.

## Repository
`include/victus/` public headers · `src/` engine/runtime · `tests/` tests · `docs/` specifications · `tools/` future tooling · `.github/workflows/` CI · `AGENTS.md` agent contract · `ROADMAP.md` implementation plan.

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
Read [AGENTS.md](AGENTS.md), [ROADMAP.md](ROADMAP.md), [architecture](docs/ARCHITECTURE.md), [technology decisions](docs/TECHNOLOGY_DECISIONS.md), [research register](docs/RESEARCH_REGISTER.md), [editor spec](docs/EDITOR_SPEC.md), [performance strategy](docs/PERFORMANCE_STRATEGY.md), [platform strategy](docs/PLATFORM_STRATEGY.md), [license policy](docs/THIRD_PARTY_AND_LICENSES.md) and [status](docs/STATUS.md).

## License
Original VICTUS code: Apache-2.0. Third-party components retain their own licenses and notices.
