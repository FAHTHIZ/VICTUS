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
