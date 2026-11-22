# Getting started with VICTUS

This guide takes you from a clean machine to a running game, an open editor and an exported build. Every command here is exercised by CI or by the CLI smoke test (`victus_cli_smoke`) on Linux; Windows and macOS build and run the non-GPU tests in CI (see [Platform support](PLATFORM_SUPPORT.md)).

## 1. Install the tools

| Platform | What you need |
|---|---|
| Linux (Ubuntu 24.04+/Debian, Fedora) | `tools/setup/install_deps.sh` installs CMake, Ninja, a compiler and the X11/Wayland/audio headers SDL3 needs, plus Mesa's software Vulkan driver for GPU tests. The Ubuntu list is the one CI uses; the Fedora list is maintained by hand. |
| Windows 10/11 x64 | Visual Studio 2022 (17.8 or newer) with "Desktop development with C++", CMake 3.24+ (bundled with VS), Python 3.10+, Git. A Vulkan-capable GPU driver to render (D3D12 is not supported yet). |
| macOS 14+ (Apple Silicon) | Xcode command line tools (`xcode-select --install`), then `tools/setup/install_deps.sh` (Homebrew: CMake, Ninja, Python). Rendering uses Metal. |

Then check the machine:

~~~bash
python3 tools/setup/check_env.py
~~~

It reports each required and optional tool and exits non-zero if something required is missing. All third-party libraries (SDL3, Luau, Jolt, Box2D, miniaudio, Recast, ENet, glslang, ...) are downloaded by CMake on the first configure from pinned, SHA-256-verified release archives; no account or SDK sign-in is needed. Pins live in `cmake/VictusDependencies.cmake`; `python3 tools/deps/victus_deps.py list` prints them.

## 2. Build and test

With Ninja (Linux, macOS, or a Visual Studio Developer prompt):

~~~bash
cmake --preset dev                 # Debug, tests on, warnings as errors -> build/dev
cmake --build --preset dev
ctest --preset dev                 # all suites; GPU suites skip when no Vulkan/Metal device exists
~~~

Other presets: `release` (optimised, used for benchmarks), `asan` (AddressSanitizer + UBSan) and `tsan` (ThreadSanitizer). Without presets (for example with the Visual Studio generator):

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
~~~

Binaries land in `build/<preset>/bin/` (`build/bin/<Config>/` with multi-config generators). A Debug build with tests needs about 5.5 GB of disk and takes roughly 40 minutes on 2 cores the first time; later builds are incremental.

To build only the headless core (ECS, scene, scripting, physics, audio mixing, navigation, networking, gameplay; no renderer, editor, player or `victus` CLI), for example on a server or a machine without graphics headers:

~~~bash
cmake -S . -B build/headless -G Ninja -DVICTUS_WITH_GRAPHICS=OFF -DVICTUS_BUILD_TESTS=ON
cmake --build build/headless
ctest --test-dir build/headless   # 19 suites
~~~

Useful environment variables for tests:

| Variable | Effect |
|---|---|
| `VICTUS_REQUIRE_GPU=1` | GPU suites fail instead of skipping when no device is found (CI on Linux sets it). |
| `VICTUS_NO_GPU=1` | Skip GPU suites (sanitizer lanes, machines without a driver). |
| `VICTUS_NO_AUDIO=1` | Mix audio without opening an output device. |
| `SDL_VIDEO_DRIVER=offscreen` | Run graphical tests without a display. |

Run one suite or one case: `./build/dev/bin/victus_physics_tests -tc="*character*"` (doctest filters), or by label: `ctest --preset dev -L editor`.

## 3. Create and run a project

~~~bash
B=build/dev/bin
$B/victus templates                          # 3d, 2d, nocode, nocode3d, simulation, multiplayer
$B/victus new MyGame --template 3d           # MyGame/victus.project, scenes/, assets/
$B/victus run MyGame                         # play it in a window
$B/victus check MyGame                       # validate scenes, scripts, shaders, trees, sequences
$B/victus simulate MyGame --frames 600 --hold D   # headless: gameplay + scripts, no GPU
~~~

## 4. Open the editor

~~~bash
$B/victus_editor MyGame       # or with no argument for the project launcher
~~~

Press Play to run the scene on an isolated copy (Stop restores the authored scene), click the viewport so the game receives the keyboard, and use Ctrl+S to save. The [User guide](USER_GUIDE.md#editor) describes gizmos, prefabs, the material and graph editors, terrain tools, the sequencer and the debuggers.

## 5. Export a game

~~~bash
$B/victus export MyGame --out MyGame-export
./MyGame-export/MyGame
~~~

`victus export` re-imports stale assets, validates every scene and script, and writes the player executable, `game/game.vpak` (cooked assets only, byte-reproducible), `THIRD_PARTY_NOTICES.txt` and `export.json`. It exports for the platform you are on. Android APKs are built with Gradle from an export; see [Android](ANDROID.md).

## 6. Before you contribute

~~~bash
python3 tools/shaders/compile_shaders.py --check     # generated shader library up to date
python3 tools/deps/victus_deps.py check      # dependency pins match the manifest
python3 tools/lint/run_clang_tidy.py -p build/dev    # static analysis (needs clang-tidy)
~~~

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the workflow and commit conventions.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Could NOT find X11` / Wayland errors during configure | Install the headers: `tools/setup/install_deps.sh`. |
| GPU tests are skipped | No Vulkan (Linux/Windows) or Metal (macOS) device was found. On Linux install `mesa-vulkan-drivers`; check with `vulkaninfo --summary`. Windows CI runners have no GPU, so GPU suites skip there. |
| `victus run` opens no window on a headless server | Use `victus simulate` (no GPU) or `xvfb-run -a victus run ...`. |
| First configure is slow or fails on download | CMake downloads the pinned archives (about 300 MB unpacked) into `build/<preset>/_deps`. Re-run configure after a network failure; hashes are verified, so a partial download is rejected rather than used. |
| `fatal error: generated shader library is stale` in CI | You edited `engine/render/shaders/*`; install glslang + SPIRV-Cross and run `python3 tools/shaders/compile_shaders.py`. |
| Editor tests take minutes | They drive the real editor on a software rasteriser (lavapipe); `ctest -j2` and `-LE editor` help during iteration. |
| Out of disk | A Debug build with tests is ~5.5 GB; `release` is smaller. Delete `build/<preset>` to start over. |
