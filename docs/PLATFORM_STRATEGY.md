# VICTUS Platform Strategy

## Target order
1. Windows x64 — primary editor/runtime.
2. Linux x64 — CI, headless/server and desktop.
3. Android arm64 — mobile runtime/performance target.
4. macOS Apple Silicon — editor/runtime with Metal.
5. iOS — after Apple build/signing workflow is established.

Never advertise support until build, launch and core smoke tests pass.

| Capability | Windows | Linux | Android | macOS | iOS |
|---|---|---|---|---|---|
| Core/headless | Required | Required | Practical build/test | Required | Practical build/test |
| Editor | Primary | Primary | Not initially | Planned | Not initially |
| Runtime | Required | Required | Required | Required | Planned |
| Graphics | RHI decision | Vulkan candidate | Vulkan + fallback | Metal strategy | Metal strategy |
| Input | Mouse/keyboard/gamepad | Same | Touch/gamepad | Mouse/keyboard/gamepad | Touch/gamepad |

Requirements: CMake toolchains/compiler checks; capability queries not scattered macros; filesystem/path abstraction; threading constraints; graphics device loss/context recreation; suspend/resume; asset variants/package signing; save paths; touch/mouse/keyboard/controller input; privacy-conscious diagnostics; packaging/update strategy.

Mobile runtime is first-class from day one; editor need not run on phones initially. Validate physical devices and maintain tested OS/device matrix, minimum requirements and limitations. Without physical validation, label experimental.
