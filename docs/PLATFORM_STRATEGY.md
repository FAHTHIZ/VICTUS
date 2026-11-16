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
