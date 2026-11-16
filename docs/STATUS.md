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
| Benchmark harness (`benchmarks/`) with JSON output + machine metadata | Implemented | `victus_bench_core` |

## Log
### 2026-11-16 — Slice 1: core runtime foundation
- Audit: prior main contained one `EngineClock` class (+ test) and documentation only; no engine systems existed. The layout was replaced with per-module targets (`engine/<module>/include|src`), the clock was kept and its test migrated to doctest.
- Verification (local, Ubuntu 26.04, GCC 15.2, 2 vCPU Intel Xeon @ 2.90 GHz):
  - `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` → 39 test cases, 11 439 assertions passed, `-Werror`.
  - `asan` and `tsan` presets → all tests pass, no sanitizer reports.
  - Release `victus_bench_core` baseline (p50): 100k empty jobs 44.8 ms; parallel_for integrate 1M vec3 0.44 ms vs serial 1.10 ms; arena 100k allocs 0.23 ms vs malloc/free 3.82 ms; JSON parse 20k entities (2.7 MB) 23.1 ms.

## Next task
Slice 2: platform layer (SDL3) + VICTUS RHI on SDL_GPU with offline SPIR-V/MSL shader pipeline, headless render readback tests on lavapipe, then real 2D sprite and 3D mesh render paths.

## Resume protocol
At each session inspect main/latest commit, this file, AGENTS.md, ROADMAP.md, CI and the working tree. Continue the first unblocked task in "Next task". Never mark documentation-only work as implemented.
