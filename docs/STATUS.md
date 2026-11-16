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

## Next task
Slice 3: scene/ECS + serialization and the asset pipeline, proven by loading a glTF model into the 3D renderer: VICTUS ECS (archetype SoA, queries, hierarchy/transforms), JSON scene format with stable UUIDs, cgltf import, texture import with `.meta` sidecars and cooked cache, then the editor shell (Dear ImGui docking) on top.

## Resume protocol
At each session inspect main/latest commit, this file, AGENTS.md, ROADMAP.md, CI and the working tree. Continue the first unblocked task in "Next task". Never mark documentation-only work as implemented.
