# VICTUS Performance Strategy

Performance is a measured property of a specific build, device, driver, workload and quality profile—not a marketing adjective. Scale simulation and rendering from capable desktops to constrained mobile hardware without silently changing authoritative game rules.

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
Use stable workloads, warm/cold cache cases, repeated runs and machine-readable results. Never compare unequal scenes, quality settings, drivers or thermal conditions. Keep raw data and commands so another developer can reproduce every published claim.

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
