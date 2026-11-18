# Contributing to VICTUS

Read [AGENTS.md](AGENTS.md), [ROADMAP.md](ROADMAP.md), [ARCHITECTURE.md](docs/ARCHITECTURE.md) and the relevant decision records in [TECHNOLOGY_DECISIONS.md](docs/TECHNOLOGY_DECISIONS.md) first. Inspect `main` and the existing code before changing anything; extend or replace what is there rather than adding a parallel implementation. Prefer focused vertical slices: runtime, serialization, editor, scripting access, tests and docs together.

## Setup

~~~bash
tools/setup/install_deps.sh        # Linux (apt/dnf) or macOS (brew); --dry-run prints the commands, --tidy adds clang-tidy
python3 tools/setup/check_env.py   # reports missing tools
~~~

Windows: Visual Studio 2022 with the C++ workload, Python 3 and Git; see [Getting started](docs/GETTING_STARTED.md).

## Build and test

~~~bash
cmake --preset dev && cmake --build --preset dev      # Debug, -Werror (MSVC: /W4 /WX)
ctest --preset dev -j2                                 # full suite
ctest --preset dev -L editor                           # one label
./build/dev/bin/victus_physics_tests -tc="*joint*"     # one doctest case
~~~

On a headless Linux machine use `SDL_VIDEO_DRIVER=offscreen` and install Mesa's Vulkan driver so GPU suites run; set `VICTUS_REQUIRE_GPU=1` to make a missing device a failure instead of a skip. Run the `asan` or `tsan` preset for memory or threading changes and the `release` preset before performance claims (record hardware, OS, driver and build as described in [PERFORMANCE_STRATEGY.md](docs/PERFORMANCE_STRATEGY.md)).

## Checks CI runs (run them before pushing)

| Check | Command | Notes |
|---|---|---|
| Shader library up to date | `python3 tools/shaders/compile_shaders.py --check` | After editing `engine/render/shaders/*`, regenerate with `python3 tools/shaders/compile_shaders.py` (needs glslang and SPIRV-Cross). |
| Dependency manifest | `python3 tools/deps/victus_deps.py check` | Every `FetchContent` pin in `cmake/VictusDependencies.cmake` is pinned by URL + SHA-256 and listed with its exact version in the licence register; `sbom` writes a CycloneDX SBOM. |
| Known vulnerabilities | `python3 tools/deps/victus_deps.py osv` | Queries [OSV](https://osv.dev) for every pinned component; an ignored advisory needs a written mitigation in the manifest. CI also runs it weekly. |
| Static analysis | `python3 tools/lint/run_clang_tidy.py -p build/dev -j2` | Uses `.clang-tidy` (bugprone, clang-analyzer, selected performance/misc checks; warnings are errors). Pass file filters to lint a subset, e.g. `... engine/physics`. |
| Warnings | build with `-Werror` / `/WX` | GCC `-Wshadow=local` and MSVC C4458 differ; shadowing a member breaks the Windows build. |

Suppress a clang-tidy finding only with `// NOLINT(check-name): reason` on the line, and only when the code is correct; prefer fixing it. Disabling a check globally needs a comment in `.clang-tidy`.

## Updating GitHub Actions

The owner takes changes directly on `main`, so automated update pull requests (Dependabot) are not used. Review the action versions in `.github/workflows/` by hand when GitHub announces a runtime deprecation, and verify the bump in a CI run.

## Adding a dependency

Follow [THIRD_PARTY_AND_LICENSES.md](docs/THIRD_PARTY_AND_LICENSES.md): justify it against writing the code, check the licence, pin a release archive by SHA-256 in `cmake/`, record it in `tools/deps/dependencies.json` and the licence register, and run `victus_deps.py check`. Never copy proprietary engine code or assets.

## Code expectations

C++20; RAII and explicit ownership; no hidden global mutable state; return `Result`/error values with context instead of crashing on bad input; validate everything read from disk or the network; keep module dependencies pointing downward (see ARCHITECTURE.md); add tests with every behaviour change; note platform fallbacks. Do not commit build output, credentials, private data or unlicensed assets.

## Commits

Use focused Conventional Commits, e.g. `feat(physics): add hinge motors`, `fix(assets): reject overflowing accessors`, `test(net): cover reconnect`, `docs: update platform matrix`. Each commit must build and pass the tests it touches. After a verified unit of work, update [docs/STATUS.md](docs/STATUS.md) (what, how verified, commands and results, known gaps) and ROADMAP.md. The owner currently asks for direct commits to `main`.
