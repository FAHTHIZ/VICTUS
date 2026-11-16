# VICTUS Engineering Status

This file records only verified state. Every entry lists what was built, how it was verified and what is next.

## Verified capability summary
| Area | State | Evidence |
|---|---|---|
| Core runtime (`engine/core`) | Implemented | `victus_core_tests` (Debug, Release, ASan+UBSan, TSan) |
| Result/error types, logging (console/file/ring sinks), assertions, crash context | Implemented | core tests |
| JSON DOM/parser/writer with depth/size limits | Implemented | core tests, json benchmark |
| Filesystem (atomic writes, sandboxed relative paths, user/exe dirs) | Implemented | core tests |
| CLI parser, build metadata | Implemented | core tests |
| Hashing, deterministic PCG32, UUID v4 | Implemented | reference-vector tests |
| Memory: linear arena, object pool, opt-in global allocation tracking | Implemented | core tests, benchmark |
| Job system (work stealing, priorities, helping waits, parallel_for, task graph) | Implemented | core tests incl. TSan |
| Profiler zones, Chrome/Perfetto trace export, percentile windows | Implemented | core tests |
| Math (vec/quat/mat4, 0..1 depth, frustum, AABB, color) | Implemented | core tests |
| Benchmark harness (`benchmarks/`) with JSON output + machine metadata | Implemented | `victus_bench_core`, `victus_bench_render` |
| Platform (`engine/platform`, SDL3 3.4.20): windows, keyboard/mouse/gamepad/touch input, lifecycle events, clipboard | Implemented (desktop Linux verified) | `victus_gpu_tests`, `victus_render_showcase` under Xvfb |
| RHI (`engine/rhi`): generational handles, textures/buffers/samplers/shaders/pipelines, uploads, mips, swapchain, blit, readback | Implemented on SDL_GPU; **Vulkan verified**, Metal/D3D12 not executed | `victus_gpu_tests` (lavapipe) |
| Shader pipeline: GLSL → SPIR-V + MSL + reflection, embedded, CI staleness check | Implemented | `tools/shaders/compile_shaders.py --check` |
| Render graph: dependency derivation, culling, lifetime-based transient aliasing, transient pool | Implemented | `victus_render_tests` |
| 2D: SpriteBatch (layer/texture sorting, batching, rotation/pivot/flip, lines), Camera2D | Implemented | pixel-probed GPU test |
| 3D: forward HDR PBR (metallic-roughness, normal/emissive maps, alpha cutoff), directional light + PCF shadow map, 16 point lights, analytic sky + environment specular, ACES tonemap, debug lines, frustum culling | Implemented | pixel-probed GPU test + showcase screenshot |
| Images (stb): PNG/JPEG/TGA/BMP/HDR decode, PNG encode, sRGB-correct mip chains; procedural meshes with tangents | Implemented | `victus_render_tests` |
| ECS (`engine/ecs`): archetype SoA world, generational entities, runtime-registered components, cached queries, exclusion filters, parallel `par_each` on the job system, deferred command buffers, resources, structural-change assertions during iteration | Implemented | `victus_ecs_tests` (10 cases), `victus_bench_scene` |
| Scene (`engine/scene`): UUID identity, names, transforms, parent/child hierarchy with cycle rejection and keep-world reparenting, world-transform propagation, cameras/lights/mesh renderers/sprites, duplicate/find, versioned JSON scene format (`victus.scene` v1) preserving unknown components | Implemented | `victus_scene_tests` (5 cases) |
| Asset pipeline (`engine/assets`): project asset database with `.meta` sidecars (stable UUIDs, importer settings, duplicate-UUID repair), texture import (mips, sRGB/linear heuristics), glTF 2.0/GLB import via cgltf 1.15 (sandboxed file access, data URIs, PBR materials, normals/tangents generation, KHR unlit/emissive_strength), versioned cooked cache (`.victus/cache/<uuid>.vcooked`) keyed by source+settings hash with up-to-date checks, parallel import | Implemented (static meshes; skins/animations not yet imported) | `victus_asset_tests` (4 cases, 404 assertions) |
| Runtime scene rendering (`engine/runtime`): GPU asset cache, glTF model instantiation into scene hierarchies, scene → SceneRenderer/SpriteBatch extraction | Implemented | `victus_gpu_tests` end-to-end case: project folder → import → cooked cache → scene save/load → GPU render, pixel-probed |
| UI layer (`engine/ui`): Dear ImGui 1.93 docking rendered through the VICTUS RHI (own renderer: font atlas + user textures, scissored indexed draws, 16-bit index splitting), SDL3 platform backend via native event hook, dark theme, DPI-aware fonts | Implemented | `victus_gpu_tests` UI case (pixel-probed window + texture) |
| Gameplay framework (`engine/gameplay`): `Simulation` fixed-step scheduler (accumulator, max-substep clamp, pause/step, time scale), built-in `Rotator`/`Oscillator` behaviours with serialization | Implemented | `victus_project_tests` |
| Projects (`engine/project`): `victus.project` manifest (versioned JSON), create/open/validate, recent-projects list, three ready-to-run templates (3D lit scene, 2D sprite scene, 400-agent wave simulation) | Implemented | `victus_project_tests`, `victus_cli_smoke` |
| `runtime::SceneView`: renders a scene (2D or 3D) into an offscreen RGBA8 target for viewports, players and screenshots | Implemented | editor tests, CLI smoke |
| Editor (`victus_editor`): launcher (create from template, open folder, recent), docked workspace (menu, Play/Pause/Stop toolbar, viewport with orbit/pan/zoom/WASD and 2D pan/zoom, click picking, grid + selection overlays), hierarchy with drag reparent/reorder, inspector for every component, add/remove components, command-based undo/redo with edit coalescing, asset browser (instantiate model, assign texture), console, unsaved-changes and Save As modals, shortcuts; play-in-editor on an isolated scene copy | Implemented (no transform gizmos, no asset thumbnails) | `victus_editor_tests` (3 cases), Xvfb screenshots of launcher/workspace/play inspected |
| Player (`victus_player`) and CLI (`victus new/templates/info/import/run/version`) | Implemented; packaged layout `victus_player` + `game/` verified | `victus_cli_smoke` (all templates, packaged player) |

## Log
### 2026-11-16 — Slice 1: core runtime foundation
- Audit: prior main contained one `EngineClock` class (+ test) and documentation only; no engine systems existed. The layout was replaced with per-module targets (`engine/<module>/include|src`), the clock was kept and its test migrated to doctest.
- Verification (local, Ubuntu 26.04, GCC 15.2, 2 vCPU Intel Xeon @ 2.90 GHz):
  - `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` → 39 test cases, 11 439 assertions passed, `-Werror`.
  - `asan` and `tsan` presets → all tests pass, no sanitizer reports.
  - Release `victus_bench_core` baseline (p50): 100k empty jobs 44.8 ms; parallel_for integrate 1M vec3 0.44 ms vs serial 1.10 ms; arena 100k allocs 0.23 ms vs malloc/free 3.82 ms; JSON parse 20k entities (2.7 MB) 23.1 ms.

### 2026-11-16 — Fix: portable warning flags (commit fd365b9)
- Hosted CI for 28311e0 failed on MSVC (`fopen`/`getenv` C4996 under /WX) and Apple Clang (`-Wshadow=local` is GCC-only). Fixed in `cmake/VictusTargets.cmake`; hosted CI run 43843161288 green on ubuntu/windows/macos Debug+Release and both sanitizer lanes.

### 2026-11-16 — Slice 2: platform, RHI, render graph, real 2D and 3D rendering
- Built: `engine/platform`, `engine/rhi` (SDL_GPU backend), `engine/render` (render graph, GpuContext, SpriteBatch, SceneRenderer, images, meshes, 11 built-in shaders), `samples/render_showcase` (windowed: orbit camera, PBR sphere grid, animated cubes, moving point lights, shadows, 2D frame-time HUD, `--frames/--screenshot`).
- Verification (local, Ubuntu 26.04, GCC 15.2, Mesa lavapipe Vulkan 1.4, 2 vCPU):
  - Debug `-Werror`, Release `-Werror`: `victus_core_tests`, `victus_render_tests` (9 cases), `victus_gpu_tests` (3 cases, pixel probes: clear colour, sprite layering/texture/position, red cube top, shadowed vs lit ground luminance, blue sky, transient pool reuse on frame 2) all pass with `VICTUS_REQUIRE_GPU=1`.
  - ASan+UBSan Debug: all tests incl. GPU tests and 10 windowed showcase frames pass with no reports (`detect_leaks=0` because Mesa itself is uninstrumented).
  - `xvfb-run victus_render_showcase --frames 30 --screenshot` renders through a real X11 Vulkan swapchain; screenshot inspected visually.
- Release `victus_bench_render` (p50, lavapipe = CPU software rasteriser, not representative of GPUs): render graph compile 64 passes 0.012 ms; SpriteBatch build+upload 20k sprites 1.18 ms; 20k-sprite 720p frame incl. GPU 27.4 ms; 1024-cube PBR scene with 2048² shadow map at 720p incl. GPU 141 ms.
- Known gaps: Metal path untested (no Apple hardware here; CI macOS runner result recorded below once available); no DXIL so D3D12 unusable and Windows relies on Vulkan drivers; no GPU timestamps; per-draw uniform pushes (no instancing yet); no MSAA/FXAA; no text rendering yet.
- Hosted CI for cb182da: run 43845863811 green on ubuntu/windows/macos Debug+Release and both sanitizer lanes. macOS GPU tests ran in 2.47 s; whether that was Metal is confirmed by the new "GPU backend report" CI step (recorded in the Slice 3 entry).

### 2026-11-16 — Slice 3: ECS, scene format, asset pipeline, scene rendering
- Built: `engine/ecs`, `engine/scene`, `engine/assets` (+ `third_party/cgltf` 1.15), `engine/runtime` scene-rendering bridge, `GpuContext::create_texture_mips`, `benchmarks/scene_bench.cpp`. CI now prints the GPU backend used and uploads render artifacts on every OS.
- Verification (local, Ubuntu 26.04, GCC 15.2 / clang 21, lavapipe):
  - Debug GCC `-Werror`, Debug clang `-Werror`, Release, ASan+UBSan: `ctest` 6/6 suites pass (core, render, ecs, scene, assets, gpu); `VICTUS_REQUIRE_GPU=1 victus_gpu_tests` 4 cases / 58 assertions incl. the end-to-end scene case (imported orange glTF quad as child of a scene node and a blue primitive cube appear at their projected positions; `test_output/scene_e2e.png` inspected).
  - clang with `-Wshorten-64-to-32 -Wfloat-conversion -Wimplicit-int-conversion`: no warnings in VICTUS code (MSVC proxy check).
  - `tools/shaders/compile_shaders.py --check`: up to date.
- Release `victus_bench_scene` (p50, 2 vCPU): create+2 adds+destroy 100k entities 8.97 ms; `each` integrate 1M entities (2 archetypes) 0.86 ms; `par_each` 0.45 ms; 1M random `get` 27.2 ms; transform propagation 10k-node hierarchy 0.56 ms; scene save 10k 27.1 ms; stringify 20.1 ms; parse+load 62.7 ms.
- Known gaps: no comparison against Flecs/EnTT yet (ADR-0004); glTF skins/animations/cameras/lights not imported; no texture compression (KTX2/Basis); scene save/load speed is JSON-bound (binary cooked scenes planned).

### 2026-11-16 — Slice 4: editor, project workflow, player and CLI
- Hosted CI for f2bb092 (C4324 fix): run 43849791643 green on ubuntu/windows/macos Debug+Release and both sanitizer lanes.
- Built: `third_party/imgui` (v1.93.0-docking, pinned), `engine/ui`, `engine/gameplay`, `engine/project` (+ templates), `runtime::SceneView`, `editor/` (`victus_editor_core` library + `victus_editor`), `engine/player` + `apps/player` (`victus_player`), `tools/cli` (`victus`). Platform gained a native-event hook, quit cancellation and a folder dialog; Scene gained sibling reordering and entity/subtree snapshot+restore (used by undo); `Camera2D::world_to_screen`.
- Fix: long debug-grid lines produced large grey triangle artifacts on lavapipe when the grid half-size was ≥15; `SceneRenderer::grid` now emits one segment per cell (root cause in the clipper not confirmed; observed on llvmpipe only).
- Verification (local, Ubuntu 26.04, GCC 15.2 / clang 21, lavapipe, `SDL_VIDEO_DRIVER=offscreen VICTUS_REQUIRE_GPU=1`):
  - Debug GCC `-Werror`, Debug clang `-Werror`, Release, ASan+UBSan (`detect_leaks=0`): `ctest` 9/9 pass — core 39 cases/11 443 assertions, render 9/3 689, ecs 10/701, scene 6/98, assets 4/404, gpu 5/74, project 4/75, editor 3/125, `victus_cli_smoke`.
  - Editor tests drive the real editor headlessly: create a 3D project from the template, pick entities by viewport click, create/delete/duplicate/reparent with undo/redo, play/stop isolation restoring the authored scene, save and reopen; 2D and simulation templates render (`test_output/editor_viewport_*.png` inspected).
  - `victus_cli_smoke`: `new` for every template (and refusal to overwrite), `info`, `import`, `run --frames 5 --screenshot`, unknown-command/missing-project failures, packaged player with a `game/` folder.
  - Xvfb: editor launcher, 3D workspace and play mode, `victus run`, packaged `victus_player` screenshots inspected visually.
  - `VICTUS_NO_GPU=1`: GPU/editor tests skip cleanly and CLI smoke runs the non-GPU steps.
- Known gaps: no transform gizmos (transforms edited in the inspector); no asset thumbnails; play-in-editor only (external `victus run` is a separate process); no scripting yet, so templates use the built-in Rotator/Oscillator behaviours; no export/packaging command yet (packaged layout verified manually and in the smoke test); macOS/Windows editor not executed (CI builds and runs its tests, GPU cases skip without a device).

## Next task
Slice 5: scripting. Embed Luau (pinned release, MIT) as the gameplay scripting language: `ScriptComponent` referencing `.luau` assets, sandboxed VM per world, entity/transform/input/time API, hot reload in the editor, script errors surfaced in the console, a script in each template, tests running scripts headlessly. Then physics (Box2D v3 for 2D, Jolt for 3D) and audio (miniaudio).

## Resume protocol
At each session inspect main/latest commit, this file, AGENTS.md, ROADMAP.md, CI and the working tree. Continue the first unblocked task in "Next task". Never mark documentation-only work as implemented.
