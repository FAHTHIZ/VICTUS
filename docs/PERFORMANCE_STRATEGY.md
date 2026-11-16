# VICTUS Performance Strategy

Scale simulation and rendering across desktop/mobile. Do not promise identical desktop effects on every phone. Define tiers and preserve simulation semantics.

## Measure
CPU/GPU frame time and subsystem timings; simulation tick/queue depth; resident/peak memory and allocations; asset import/decode/upload; streaming latency/cache hit rate; startup/package size; mobile battery/temperature where hardware permits. Record hardware, OS, driver, power mode, build flags, scene/settings, cache state, repetitions and variance. Report median/tail percentiles where meaningful.

## Simulation
Profile ECS iteration, structural changes, locality and component size. Batch independent systems and declare read/write dependencies. Use deterministic random streams/event queues. LOD tiers: full entity, simplified entity, regional aggregate, event-driven dormant. Preserve population/resource/ownership/time/relationship/event invariants. Bound catch-up and define overload. Separate authoritative ticks from render interpolation. Use spatial indexes/incremental updates.

## Rendering
Render graph with explicit lifetime/synchronization; GPU culling/indirect draws where beneficial; mesh LOD, instancing, occlusion and streaming; mipmaps/platform compression/residency budgets; capability-constrained shader variants; dynamic resolution/adaptive quality with user override; scalable shadows/post effects. Profile CPU submission and GPU execution separately.

## Mobile
Respect sustained thermals, budget CPU/GPU memory, handle suspend/resume/context loss, async-load with bounded upload budgets, reduce overdraw/bandwidth, validate physical devices. Emulators cannot prove thermal or battery behavior.

## Benchmarks
Stable workloads for entity scaling, settlement simulation, terrain/foliage streaming, animation crowds, physics broadphase, asset cook/cache, render-pass/GPU memory, save/load. Another developer must reproduce every published result.
