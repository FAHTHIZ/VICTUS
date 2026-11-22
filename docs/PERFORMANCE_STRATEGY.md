# VICTUS Performance Strategy

Performance is a measured property of a specific build, device, driver, workload and quality profile—not a marketing adjective. Scale simulation and rendering from capable desktops to constrained mobile hardware without silently changing authoritative game rules.

The prioritized visual-quality and performance work items are maintained in [VISUAL_QUALITY_AND_PERFORMANCE_GAP_REGISTER.md](VISUAL_QUALITY_AND_PERFORMANCE_GAP_REGISTER.md). This document defines measurement discipline, workloads and benchmark evidence; the register defines what to build next.

## Measurement
Track CPU/GPU frame time and subsystem timings; simulation tick and queue depth; resident/peak memory and allocations; asset import/decode/upload; streaming latency/cache hit rate; shader compilation; network bandwidth/latency; save/load; startup/package size; mobile battery and sustained thermals where hardware permits. Record hardware, OS, driver, power mode, build flags, scene, settings, cache state, repetitions and variance. Report percentiles where meaningful.

## Workload coverage
Maintain reproducible benchmarks for:
- ECS iteration, structural changes, transforms, jobs and large populations.
- 2D sprite batches, tilemaps, overdraw, 2D lighting and particles.
- 3D draw submission, geometry, materials, shadows, lighting, post effects and GPU memory.
- Physics broadphase, character movement, navigation and crowds.
- Skeletal animation, blend graphs, VFX simulation and audio mixing.
- Asset import/cook/cache, texture transcode, shader compile and streaming.
- Multiplayer replication, serialization, packet budgets and server tick stability.
- Save/load, world transitions, startup, editor responsiveness and package size.

## Simulation
Profile ECS layout, locality, component size, structural changes and scheduler overhead. Use deterministic random streams and explicit event ordering where feasible. LOD tiers may include full entity, simplified entity, regional aggregate and event-driven dormant state. Preserve population/resource/ownership/time/relationship/event invariants. Bound catch-up and define overload behavior. Keep authoritative ticks separate from render interpolation.

## Rendering
Use explicit resource lifetimes/synchronization and avoid unnecessary CPU/GPU stalls. Evaluate GPU culling, indirect draws, instancing, occlusion, mesh LOD, texture residency and streaming on target hardware. Use capability-constrained shader variants and quality tiers. Dynamic resolution/adaptive quality must be measurable and user-overridable. Profile CPU submission and GPU execution separately. Advanced effects are optional where bandwidth, thermal or memory budgets make them unsuitable.

## Mobile
Budget CPU/GPU memory and bandwidth; manage thermal throttling; handle suspend/resume and graphics context/device loss; bound asynchronous uploads; minimize startup and package size; validate touch and controller input. Emulators cannot establish sustained thermal or battery behavior. Publish physical-device results only when actually measured.

## Server and network
Measure server tick budget, entity count, replication bandwidth, serialization cost, packet loss behavior, connection count, memory per session and recovery. Test headless servers independently of render workloads.

## Benchmark quality
Use stable workloads, warm/cold cache cases, repeated runs and machine-readable results. Never compare unequal scenes, quality settings, drivers or thermal conditions. Keep raw data and commands so another developer can reproduce every published claim. Compare `victus.bench.v1` captures with `python3 tools/perf/compare_bench.py BEFORE AFTER [--limit-pct N]`. The comparator refuses captures with different machine/settings, workloads or case sets; exit 0 means reported, 1 means the requested timing limit was crossed, and 2 means the captures are not comparable. This check is not a substitute for repeated runs on matched target hardware.

## Performance budgets (CI gate)

`victus_perf_budgets` (CTest labels `perf;gpu`, script `tests/perf/perf_budgets.cmake`, budgets `tests/perf/budgets.json`) runs every template (3d, 2d, simulation, multiplayer) and the arena, dungeon_crawler and platformer samples (`scenes/autoplay.vscene`) with `victus run --frames 120 --width 640 --height 360 --quality medium --no-hot-reload --profile`. It checks the last frame's counters from the `--profile` JSON against `[min, max]` ranges and fails on any regression:

| Counter | Meaning |
|---|---|
| `frame.draw_calls`, `frame.render_passes` | GPU draws and executed render-graph passes in the last frame |
| `frame.triangles`, `frame.visible` | submitted triangles; meshes that passed culling |
| `frame.main_batches`, `frame.shadow_batches`, `frame.transparent_draws` | instanced batches per pass |
| `frame.shadow_triangles` | triangles drawn into directional cascades and local shadow tiles |
| `frame.transparent_triangles`, `frame.alpha_tested_draws`, `frame.surface_shader_draws` | shading cost apart from geometry: blended-layer triangles (each layer shades its pixels again), visible alpha-tested opaque draws, visible draws running a project surface shader |
| `frame.sprites`, `frame.ui_items` | 2D sprites and UI quads |
| `scene.entities`, `sim.systems` | live entities; registered simulation systems |
| `gpu.texture_bytes`, `gpu.buffer_bytes`, `gpu.live_textures`, `gpu.live_buffers` | GPU memory held by the renderer |

Fixed-frame runs use a fixed script seed (`kFixedRunSeed`, overridable with `--seed`), so these counters are identical from run to run on one machine; `victus_cli_determinism` guards that. Maxima are the measured value + 10 % (at least +2); minima are half the measured value for counters that prove the frame drew something. `frame_p95_ms_max` is a coarse ceiling (about 5x the Debug lavapipe time) that only catches catastrophic slowdowns, because CI runners are shared and timing cannot be gated tightly.

The gate runs only where a GPU is required (`VICTUS_REQUIRE_GPU=1`, i.e. the Linux CI lanes on Mesa lavapipe) and reports skipped elsewhere. To update budgets after an intended change, run `ctest --preset dev -R perf_budgets`, read `build/dev/test_output/perf_budgets/measured.json` and edit `budgets.json` in the same commit, explaining why. Frame-time budgets per real device class are not established yet.

## Iteration times (GAP-40)

Unity 6.6 makes Fast Enter Play Mode the default, and 6.7 adds a Fast Build Profile (see [the Unity/Unreal audit](audits/UNITY_UNREAL_2026_11.md)). Iteration speed is now something competitors advertise. VICTUS measures only the shared cooked cache (ADR-0180). The engine has no timers for the numbers below, so until it does they are measured as wall-clock time from outside the process.

| Metric | Definition | How to measure now |
|---|---|---|
| Project open | `victus_editor <project>` start → first frame with the scene loaded, warm cooked cache | Not instrumented. Needs a timestamp at `begin_startup()` and at the first presented frame (`editor/src/editor.cpp`). |
| Enter Play | Play pressed → first simulated frame of the copied scene | Not instrumented. |
| Script reload | `.luau` file saved → new code running | Not instrumented. |
| Shader edit | `.vshader` saved → recompiled pipeline used | Not instrumented. |
| Headless run | `victus simulate <project> --frames 1` wall time, warm | `/usr/bin/time -f %e` |
| Player start | `victus run <project> --frames 1` wall time, warm, offscreen | `/usr/bin/time -f %e` with `SDL_VIDEO_DRIVER=offscreen` |
| Export | `victus export <project> --out <dir>`, cold and warm | `/usr/bin/time -f %e` |

Record the machine, build type, driver (lavapipe numbers say nothing about real GPUs), and cold or warm cache, as the rest of this document requires. Results go in STATUS.md with the commands used. The goal is to track trends per release, not to compare these numbers with Unity or Unreal.

### First measurements (2026-11-22)

Release CLI built at c5b6238 (GCC, `VICTUS_WITH_GRAPHICS=ON`), 2 vCPU Intel Xeon @ 2.90 GHz, 7 GB RAM, Mesa lavapipe (llvmpipe, LLVM 21.1.8), `SDL_VIDEO_DRIVER=offscreen`. Wall-clock times from `date +%s.%N` around each command; projects fresh from `victus new` with their cooked caches already filled by the template bake. Software rasterizer numbers: real GPUs will differ, especially for the 3D rows.

| Metric | 3D template | 2D template |
|---|---|---|
| `victus new` (includes the template lightmap bake for 3D) | 1.61 s | 0.02 s |
| Headless run (`simulate --frames 1`) | 0.45–0.47 s | 0.01 s |
| Player start (`run --frames 1`), first / second run | 5.61 s / 2.40 s | 0.55 s / 0.55 s |
| Exported player (`<game> --frames 1`, `--target desktop`) | 2.21–2.53 s | 0.58 s |
| Export, `--target all` (two runs) | 5.25 s / 5.17 s, pack 73.1 MB | 0.06 s, pack 0.19 MB |
| Export, `--target desktop` / `android` | 3.64 s, 42.8 MB / 2.01 s, 22.7 MB | not measured |

The 3D pack is dominated by the two starter characters (ADR-0179): each 20 MB `.glb` cooks to about 178 MB with every texture variant, so `--target all` ships 356 MB of payload compressed to 73 MB. See KNOWN_LIMITATIONS.

### Startup and memory (2026-11-22, commit 60ad501)

Same machine. Each figure was measured in three runs, comparing the build before and after 60ad501.

| Metric | Before (558ef34) | After (60ad501) |
|---|---|---|
| First frame `system.animation.update`, 3D template (`run --frames 2 --quality low --trace`) | 455–597 ms | 71–81 ms |
| Wall time, `victus run --frames 2`, 3D template | 3.08–3.34 s | 2.48–2.80 s |
| Peak resident set, same run | 835–843 MB | 836–844 MB |

- The animation system used to decode each starter character's whole 178 MB cooked model (every texture variant) just to read its skeleton and clips. It now uses a skeleton-only decode over a memory-mapped file.
- Most of the remaining 71–81 ms was the import up-to-date check, about 30 ms per model, which re-hashed each 20 MB source on every load. A per-(path, size, mtime) hash cache brings the first-frame animation update to 9.3–9.6 ms and the two-frame run to 2.35–2.44 s.
- a51dddc decodes only the texture variant the device will upload (`assets::VariantFilter`, from `GpuAssetCache::variant_filter`). On lavapipe, which samples BC7, each starter character's decode drops from every variant (BC7 74.8 MB + ASTC 74.8 MB + RGBA8 6.6 MB) to the BC7 one. With the warm cache, `assets.load_model` takes 34–52 ms per character over three runs at 14edaa7. `GpuAssetCache::add_model` (76–117 ms) and the first `pass.forward` (lavapipe pipeline compilation) are now the largest first-frame costs.
- Exported packs: every read of a pack entry verifies its BLAKE2b digest (about 13 ms for an 11 MB entry) and decompresses it whole (about 52 ms for a 42 MB character). Animation used to read the whole character just for its skeleton. Export now writes a `.vskel` sidecar (1.8 MB raw, about 150 KB compressed). From the 3D template's Android pack (`victus run <pack> --frames 2 --quality low`, three runs each, same binary), the first-frame animation update goes from 150–168 ms to 8.6–8.9 ms, large decompressions drop from 4 to 2, and wall time from 3.11–3.17 s to 2.93–2.95 s. The remaining first-frame cost there is `GpuAssetCache::add_model` (about 500 ms per character), which is lavapipe decoding ASTC on the CPU. A phone samples ASTC natively, so that cost does not apply there.

Peak memory of the exported 3D template, run by the desktop player from the Android-profile pack (ASTC only) at `--quality low` under heaptrack 1.5:

- Peak resident set: 760 MB. The 2D template reaches 208 MB.
- Peak heap: 337 MB. Of that, 239 MB is lavapipe's own allocations (shader compilation), so the engine's heap at peak is about 100 MB.
- Most of the rest of the resident set is lavapipe device memory, which a phone GPU would hold in its own allocation.
- **This sandbox cannot say what a phone will use.** GPU memory has to be read on a device (GAP-02, GAP-03).
