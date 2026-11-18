# Platform support matrix

What has actually been executed on each platform, as of 2026-11-18. "CI" means the GitHub-hosted runners in `.github/workflows/ci.yml`; none of them has a physical GPU. "Local" means the maintainer's Ubuntu 26.04 machine (GCC 15.2, Mesa lavapipe software Vulkan). No physical Android or Apple device has been used. The intended target order and requirements are in [PLATFORM_STRATEGY.md](PLATFORM_STRATEGY.md).

| | Linux x64 | Windows x64 | macOS arm64 | Android arm64 | iOS / Web / consoles |
|---|---|---|---|---|---|
| Compiler | GCC 13+ (CI: Ubuntu 24.04 GCC 13, local GCC 15.2), Clang 18+ | MSVC 2022 `/W4 /WX` | Apple Clang (CI `macos-latest`) | NDK 29.0 Clang (CI) | — |
| Engine, editor, CLI, player build | CI Debug + Release, local | CI Debug + Release | CI Debug + Release | Player only (`libvictus_player.so`), CI | Not started |
| Non-GPU test suites | CI + local, all pass | CI, all pass | CI, all pass | Not run (no emulator lane) | — |
| GPU test suites (rendered output pixel-probed) | Vulkan via lavapipe, CI + local | Skipped: no Vulkan device on CI runners | Metal on CI's Apple GPU (paravirtualised), pass | Not run | — |
| Sanitizers | ASan + UBSan, TSan on non-GPU suites (CI, local) | — | — | — | — |
| Graphics backend | Vulkan | Vulkan only (no DXIL is generated, so D3D12 cannot be used) | Metal | Vulkan (GLES fallback not implemented) | — |
| Editor | Used daily (local, Xvfb in tests) | Builds; never launched interactively | Builds; never launched interactively | Not a target | — |
| `victus export` | Verified (smoke test runs the export) | Builds; export path exercised by tests | Builds; export path exercised by tests | APK via Gradle from an export, CI build only | — |
| Audio output | miniaudio; tests mix with no device (`VICTUS_NO_AUDIO=1`) | Same | Same | AAudio/OpenSL via miniaudio, never heard on a device | — |
| Gamepad / touch | SDL3 gamepad tested via injected events | Same code path | Same code path | Touch API tested on desktop with injected events | — |
| Physical hardware validation | Desktop only, software rasteriser | None | None | None | — |

Status labels used in docs: **verified** (executed and checked on that platform), **builds** (compiles and links, possibly runs non-GPU tests), **untested** (code exists, never executed), **not supported**.

## Known platform gaps

- Windows rendering has never been executed: CI runners have no GPU driver, and no Windows machine with a GPU was available. Treat the Windows editor and player as **builds, untested**.
- macOS rendering runs in CI on a virtualised Apple GPU; results differ slightly from lavapipe, so GPU test thresholds were measured on both (see STATUS.md, "Fix: Metal storage-buffer slots").
- Android: the APK is built and packaged in CI. It has never been installed on an emulator or phone, so frame rate, thermals, memory, input feel and lifecycle on real devices are unknown. The roadmap's mobile performance targets are unmeasured.
- GPU device loss: SDL_GPU 3.4 exposes no device-lost or device-reset notification, and VICTUS does not attempt to recreate the device. The RHI counts failed command-buffer acquisitions, swapchain acquisitions and submissions; 30 consecutive failures of one kind (about half a second) mark the device lost. The player then stops with `GPU device lost: <operation> failed N times in a row (<driver message>)`, exit code 3 and an error dialog; the editor first writes unsaved scene edits to the autosave slot, which the next session offers to recover. Detection is verified with injected failures (`VICTUS_GPU_FAIL`) only; no real driver reset has been tested. On suspend/resume the player pauses the simulation and avoids a time jump; nothing is recreated.
- No GPU timestamp queries in SDL_GPU, so per-pass GPU timings are not available on any platform (CPU timings are).
- iOS, Web/WebAssembly, XR and consoles: not started.
