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
| Static analysis | `python3 tools/lint/run_clang_tidy.py -p build/dev -j2` | Uses `.clang-tidy` (bugprone, clang-analyzer, selected performance/misc checks; warnings are errors). Pass file filters to lint a subset, e.g. `... engine/physics`. `--require-complete` (used by CI) also fails if a tracked source is missing from the compile database. |
| Parser robustness | `VICTUS_FUZZ_ITERATIONS=1000 ./build/dev/bin/victus_robustness_tests` | Deterministic mutation fuzzing of every untrusted-input loader (see below). CI's ASan+UBSan lane runs 1 000 mutations per target. |
| Warnings | build with `-Werror` / `/WX` | GCC `-Wshadow=local` and MSVC C4458 differ; shadowing a member breaks the Windows build. |

### Parser fuzz suite

`tests/robustness/parser_fuzz_tests.cpp` starts from valid documents written by the engine (template scenes, sample prefabs/materials/tilesets, the sample glTF robot, cooked caches, packs, saves, captures, images, terrain, WAV clips and truncated copies of miniaudio's Ogg Vorbis and FLAC test files) and applies byte-level and JSON-structure mutations. Every input must either load or fail with a non-empty error; crashes and sanitizer reports fail the suite. `VICTUS_FUZZ_SEED` changes the mutation stream, and `VICTUS_FUZZ_TRACE=1` writes each input to `<temp>/victus_fuzz_failures/last-<target>.bin` before running it so a crashing input can be recovered. Corrupted network packets are covered by the `session fuzz:` cases in `victus_net_tests`. When you add a loader for untrusted data, add a target with a valid seed.

### GPU failure injection

`VICTUS_GPU_FAIL=<commandbuffer|swapchain|submit>[@frame]` makes every GPU operation of that kind fail from the given frame, as a driver error would; `rhi::Device::inject_failure` does the same in tests. After 30 consecutive failures the device reports lost (`Device::lost()`, `lost_reason()`), and the player and editor exit with code 3. `victus_device_loss` (tests/cli/device_loss.cmake) runs both apps with each kind; set `VICTUS_NO_DIALOGS=1` to suppress the error dialog when running by hand.

Suppress a clang-tidy finding only with `// NOLINT(check-name): reason` on the line, and only when the code is correct; prefer fixing it. Disabling a check globally needs a comment in `.clang-tidy`.

## Updating GitHub Actions

The owner takes changes directly on `main`, so automated update pull requests (Dependabot) are not used. Review the action versions in `.github/workflows/` by hand when GitHub announces a runtime deprecation, and verify the bump in a CI run.

## Adding a dependency

Follow [THIRD_PARTY_AND_LICENSES.md](docs/THIRD_PARTY_AND_LICENSES.md): justify it against writing the code, check the licence, pin a release archive by SHA-256 in `cmake/`, record it in `tools/deps/dependencies.json` and the licence register, and run `victus_deps.py check`. Never copy proprietary engine code or assets.

## Code expectations

C++20; RAII and explicit ownership; no hidden global mutable state; return `Result`/error values with context instead of crashing on bad input; validate everything read from disk or the network; keep module dependencies pointing downward (see ARCHITECTURE.md); add tests with every behaviour change; note platform fallbacks. Do not commit build output, credentials, private data or unlicensed assets.

## Commits

Use focused Conventional Commits, e.g. `feat(physics): add hinge motors`, `fix(assets): reject overflowing accessors`, `test(net): cover reconnect`, `docs: update platform matrix`. Each commit must build and pass the tests it touches. After a verified unit of work, update [docs/STATUS.md](docs/STATUS.md) (what, how verified, commands and results, known gaps) and ROADMAP.md. The owner currently asks for direct commits to `main`.

User-visible changes also add a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md). Cite the ADR in that line; CTest `victus_docs_lint` (`python3 tools/lint/check_docs.py`) fails when a new ADR is not cited in CHANGELOG.md or a new `engine/` module is missing from the ARCHITECTURE.md module map. Releases are cut by tagging `vX.Y.Z`; see [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md).
