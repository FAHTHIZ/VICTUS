# VICTUS Platform Strategy

Platform support is earned through builds, launch tests, input/rendering validation, packaging and documented device coverage. Do not advertise a target merely because a backend is theoretically available.

## Target order
1. Windows x64 — primary editor/runtime.
2. Linux x64 — desktop, CI, headless and dedicated server.
3. macOS Apple Silicon — editor/runtime with Metal.
4. Android arm64 — first-class mobile runtime and physical-device performance target.
5. iOS — after build/signing workflow, device testing and policy validation.
6. Web/WebAssembly — feasibility study after core runtime; pursue only where threading, graphics, memory and distribution constraints fit.
7. XR — OpenXR-based research and integration after core rendering/input are stable.
8. Consoles — only with platform-holder access, SDK agreements, developer approval and compliance validation.

## Capability matrix

| Capability | Windows | Linux | macOS | Android | iOS |
|---|---|---|---|---|---|
| Core/headless | Required | Required | Required | Practical build/test | Practical build/test |
| Editor | Primary | Primary | Planned | Not initially | Not initially |
| Game runtime | Required | Required | Required | Required | Planned |
| Dedicated server | Required | Required | Required | Optional | Not target |
| Graphics | Validated RHI backend | Validated RHI backend | Metal strategy | Vulkan/fallback strategy | Metal strategy |
| Input | Mouse/keyboard/gamepad | Same | Same | Touch/gamepad | Touch/gamepad |

## Required platform services
Compiler/toolchain validation; capability queries; filesystem/path abstraction; thread and synchronization rules; high-resolution clock; window/surface lifecycle; input abstraction; graphics device-loss/context recreation; suspend/resume; save paths; asset variants; package signing; privacy-conscious diagnostics; crash symbols; update strategy.

## Mobile requirements
Mobile runtime is first-class even if the editor is desktop-only. Validate real devices and maintain a tested OS/device matrix, memory budgets, thermal behavior, touch/controller input, background/resume, storage permissions, package size and startup. Define minimum requirements and known limitations. Without physical validation, label support experimental.

## XR requirements
Use OpenXR as the standards-based integration target where supported. Validate runtime/device compatibility, frame pacing, stereo rendering, tracking/input, interaction profiles, comfort settings, performance budgets and fallback behavior. XR is an optional target, not a prerequisite for core engine completion.

## Console requirements
Console support cannot be validated without official platform-holder access and required SDKs. Keep platform-specific integrations isolated; do not redistribute restricted SDK materials or claim compatibility without approved testing.

## Current state (2026-11-22)

This is what main shows, checked against `.github/workflows/ci.yml`, `release.yml`, `apps/` and [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md). Under the rule at the top of this document, nothing below counts as validated support until a person has run it on real hardware.

| Target | Builds | Tests in CI | Never done |
|---|---|---|---|
| Linux x64 | CI Debug and Release, ASan/UBSan, TSan, clang-tidy, release packages. The headless `VICTUS_WITH_GRAPHICS=OFF` build compiles locally (not yet in CI). | Full suite on Mesa lavapipe (software Vulkan) with `VICTUS_REQUIRE_GPU=1`, the GPU suite again on GLES (llvmpipe), and a player smoke run under Xvfb. | Run on a discrete GPU. |
| Windows x64 | CI Debug and Release (Visual Studio generator) and release packages. | Runs with no GPU. Both lanes were red on 2026-11-21: an editor test lacked a GPU skip (fixed in 92aadf2), and the sequencer tests hit an MSVC Debug iterator abort (fixed in a0f02bf). | Studio or player opened by a person. D3D12 is unusable (no DXIL), so Windows depends on Vulkan drivers. |
| macOS arm64 | CI Debug and Release and release packages. | Runs on the runner's virtualised Apple GPU (Metal). | Studio opened by a person, or a run on a physical Mac GPU. Metal is rated Experimental. |
| Android arm64 | APK built in CI by Gradle (`apps/android`, ADR-0048). There is a GLES 3.0 fallback (ADR-0144) and orientation follows the manifest (ADR-0150). | Build only. | Installed on a device or emulator, profiled, thermally tested, or run through suspend/resume on hardware. |
| iOS | CMake treats iOS like Android for app packaging (`if(ANDROID OR IOS)`), but there is no Xcode project, signing or app shell. | None. | Everything. |
| Web | No Emscripten or WebGPU path. SDL_GPU 3.4 has no WebGPU backend. | None. | Everything. Unity 6.6 and Unreal 5.9 now target the web (GAP-34a in the [gap register](audits/ENGINE_LANDSCAPE_AND_GAP_AUDIT.md)). |
| XR | No OpenXR code. | None. | Everything. |
| Consoles | Nothing, as intended. There is no platform-holder access. | None. | — |
| Dedicated server | `victus serve` runs the authoritative session (NETWORKING_SPEC). It runs on loopback only. | Net tests on loopback. | A deployment, or a client and server on separate machines. |

The capability matrix above is still the goal. Its "Required" cells for Windows and macOS are not met: neither the editor nor the runtime has been run on them by a person.
