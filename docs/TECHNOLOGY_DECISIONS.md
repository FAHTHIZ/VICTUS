# Technology Decisions

A candidate is not adopted until exact version, license, platform matrix and maintenance status are verified.

| Area | Current position | Status |
|---|---|---|
| Language | C++20 baseline | Provisional |
| Build | CMake + CTest | Implemented |
| Platform | SDL3 3.4.20 (window, input, gamepad, lifecycle) behind `victus::platform` | Accepted (ADR-0007) |
| Graphics | VICTUS RHI with SDL_GPU (Vulkan/Metal/D3D12) as first backend | Accepted (ADR-0003) |
| Shaders | GLSL 450 → SPIR-V (glslang) + MSL 2.1 (SPIRV-Cross), offline, committed | Accepted (ADR-0003) |
| ECS | Compare Flecs, EnTT, custom SoA | Open |
| Scheduler | Compare Taskflow, enkiTS, custom | Open |
| Physics | Jolt candidate | Open |
| Navigation | Recast/Detour candidate | Open |
| Animation | ozz-animation candidate | Open |
| Editor UI | Dear ImGui vs Qt/Avalonia/native shell | Open |
| Asset interchange | glTF 2.0 first; evaluate OpenUSD | Open |
| Audio | miniaudio candidate | Open |
| Scripting | Compare Lua, Wren, AngelScript, managed options | Open |

## ADR-0001 — C++20 dependency-light foundation
Accepted for bootstrap. Standard-library-only core gives CI a reliable baseline. Revisit C++23 only after compiler/platform validation.

## ADR-0002 — Main-branch delivery
Accepted by owner. Commit coherent verified increments directly to main; keep commits small, preserve history and never force-push.

## ADR-0003 — Graphics backend (accepted 2026-11-16)
**Problem.** VICTUS needs real 2D/3D rendering on Windows, Linux, macOS, Android and iOS now, without a multi-year investment in three native backends before any game can render, and without becoming a wrapper around another engine.

**Candidates.** bgfx (BSD-2; own shader language/toolchain and build system, renderer-level abstraction), Diligent Engine (Apache-2.0; large, heavy build), direct Vulkan + Metal + D3D12 (maximum control, largest cost), SDL_GPU from SDL 3.4.x (zlib; explicit modern API: command buffers, render/copy/compute passes, cycling resources, Vulkan/Metal/D3D12 backends, Android/iOS support, small).

**Decision.** Engine renderers program only against the VICTUS RHI (`engine/rhi/include/victus/rhi/rhi.hpp`: generational handles, formats, pipelines, passes, uploads, readback). Its first backend is SDL_GPU (`engine/rhi/src/rhi_sdl.cpp`). Render graph, renderers, materials, shaders and all scene logic are VICTUS-owned. Shaders are authored once in GLSL 450 using SDL's resource-set convention and compiled offline by `tools/shaders/compile_shaders.py` into SPIR-V + MSL embedded in `engine/render/generated/builtin_shaders.cpp`; CI verifies the generated file matches sources by SHA-256 without needing the tools.

**Evidence.** Vulkan via Mesa lavapipe 26 (Vulkan 1.4) headless and through an X11 swapchain under Xvfb: clear/readback, sprite batching, PBR + shadow-map scene, tonemap and HUD overlay verified by pixel probes and visual inspection (see STATUS.md).

**Risks / gaps.** Metal path compiles shaders to MSL but has not been executed on Apple hardware; D3D12 requires DXIL (not yet generated) so Windows currently uses SDL_GPU's Vulkan driver; no GPU timestamp queries in SDL_GPU 3.4 (CPU profiling only); bindless/ray tracing/mesh shaders are not available through SDL_GPU. **Revisit trigger:** a measured bottleneck or required feature that SDL_GPU cannot provide; a native backend would then be added behind the same RHI. DXC (linux release v1.9.2609) is the planned DXIL path.

## ADR-0007 — Platform layer (accepted 2026-11-16)
SDL3 3.4.20 (zlib), fetched by pinned URL + SHA-256 (`cmake/VictusDependencies.cmake`), statically linked; `VICTUS_USE_SYSTEM_SDL` allows distro packages. Engine code sees only `victus::platform` (windows, keyboard/mouse/gamepad/touch input, lifecycle events, clipboard). `VICTUS_WITH_GRAPHICS=OFF` builds the core headless without SDL (servers, simulations).

## ADR-0004 — ECS and scheduler (accepted provisionally 2026-11-16)
**Decision.** VICTUS owns its ECS (`engine/ecs`): archetype storage with one aligned SoA column per component, archetype add/remove edges, generational 32+32-bit entities, runtime-registered components (needed for scripting and editor-defined data), cached queries over archetypes with exclusion filters, deferred command buffers and parallel iteration via the existing VICTUS job system (chunked `parallel_for`). Structural changes during iteration assert. Scenes (`engine/scene`) layer identity (UUID), hierarchy (intrusive sibling lists in a `Hierarchy` component) and serialization on top; serialization is a registry of named component serializers so unknown components round-trip unchanged.

**Why not Flecs/EnTT now.** Both are MIT and excellent; adopting either would expose its API across editor, scripting and networking. A small owned core keeps the replication/serialization/scripting seams under VICTUS control. **Evidence so far:** `victus_bench_scene` (STATUS.md, Slice 3). **Still required:** an identical-workload comparison against Flecs and EnTT; if VICTUS is materially slower on iteration or structural changes, either optimise or adopt behind the `victus::ecs` API. The job system remains custom (Slice 1); Taskflow/enkiTS comparison is still open.

## ADR-0008 — Scene format and asset pipeline (accepted 2026-11-16)
- Scenes: JSON `{"format":"victus.scene","version":1,"entities":[...]}` with entity UUIDs, parent UUIDs and components sorted by name for diff-friendly files. Loading is 3-pass (create, parent, components). A binary cooked scene format is planned for shipped builds.
- Assets: every source file gets a `<file>.meta` sidecar (`victus_meta`, `uuid`, `importer`, `settings`) that is meant to be committed with the project. Cooked results go to `<project>/.victus/cache/<uuid>.vcooked` (header `VCOOK`, format version, key hash of source bytes + settings + importer version, dependency hashes); the cache is disposable. Invalid metas are reported, never silently overwritten; duplicated UUIDs (copied files) are reassigned.
- glTF: cgltf 1.15 (MIT, single header, no dependencies) chosen over tinygltf (pulls in JSON library + stb) and fastgltf (C++20, simdjson dependency, faster but larger). File access is sandboxed to the asset's directory; remote URIs are rejected.
- Layering note: `engine/assets` depends on `engine/render` only for CPU types (`Image`, `MeshData`); a future split of those types into a GPU-free module will let headless cookers drop the render dependency.

## ADR-0005 — Editor UI (accepted 2026-11-16)
**Decision.** The VICTUS editor (`victus_editor`) is built on Dear ImGui v1.93.0-docking (commit `b916186e`, MIT), vendored unmodified in `third_party/imgui`. Only the upstream SDL3 *platform* backend is used; rendering goes through VICTUS's own renderer in `engine/ui` on the VICTUS RHI, so the editor exercises the same GPU path as games (Vulkan verified, Metal/D3D12 via SDL_GPU). The editor is a library (`victus_editor_core`) plus a thin executable so it can be driven headlessly by tests.

**Candidates.** Qt 6 (LGPL-3.0/commercial; strong accessibility, text and native widgets but heavy packaging, dynamic-linking obligations and a second rendering stack); Avalonia/.NET (MIT but needs a managed runtime and a GPU interop layer); a custom retained UI (largest effort, no ecosystem). ImGui wins today on embeddability, licensing, one-process debugging and iteration speed.

**Risks / revisit triggers.** ImGui has limited accessibility (screen readers), IME and rich-text support and the docking branch API moves. Mitigation: editor features live in VICTUS code behind panels, undo commands and the scene/asset APIs, not in ImGui state, so a shell replacement does not touch engine data. Revisit before a 1.0 editor if accessibility or text-heavy tools (script editing, localisation) require it. Runtime game UI is a separate decision (Phase 9); ImGui is not the shipped game UI.

**Fonts.** ImGui's embedded ProggyClean/ProggyForever fonts (MIT, Tristan Grimmer / Disco Hello) are used; no other fonts are bundled.

## ADR-0006 — Original license
Selected: Apache-2.0 for original VICTUS code. Third-party components retain their own licenses/notices. Maintain SBOM.

## ADR template
Date/status; problem/constraints; candidates; exact versions and primary sources; license/platform matrix; prototype/benchmark method/results; decision/rejected options; risks/rollback/revisit trigger.
