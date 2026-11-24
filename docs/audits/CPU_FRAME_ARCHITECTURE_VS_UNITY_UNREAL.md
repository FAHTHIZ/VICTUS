# CPU frame architecture: why VICTUS trails Unity and Unreal

Owner direction (2026-11-23): find out why VICTUS is not "disgustingly optimized" next to Unity and Unreal, and fix it continuously on `main`. This audit covers the CPU side of a frame (simulation bookkeeping, transforms, ECS queries, render extraction and submission, the job system). GPU-side gaps (GPU culling, bindless, async compute) are in [ENGINE_LANDSCAPE_AND_GAP_AUDIT.md](ENGINE_LANDSCAPE_AND_GAP_AUDIT.md) and [VISUAL_QUALITY_AND_PERFORMANCE_GAP_REGISTER.md](../VISUAL_QUALITY_AND_PERFORMANCE_GAP_REGISTER.md).

All VICTUS figures are from the 2-vCPU sandbox (Intel Xeon @ 2.90 GHz, GCC 15.2, Release) recorded in [STATUS.md](../STATUS.md); none are vendor numbers for Unity or Unreal, which are not reproduced here.

## Summary

VICTUS's algorithms are mostly sound; what it lacks is the *retained, change-driven* architecture both commercial engines moved to. Each frame VICTUS re-derives state that did not change:

| Area | Unity / Unreal | VICTUS before this work | Consequence |
|---|---|---|---|
| Transform hierarchy | Unity packs each hierarchy in parent-first order with per-transform dirty bits set by the setters and dispatches only changed transforms (`TransformHierarchy`, `TransformChangeDispatch`) | Every `update_transforms()` walked the whole hierarchy with four or five out-of-line `World::get` lookups per entity and rebuilt every TRS matrix and product | 10k static entities cost 0.56 ms per frame for no change |
| Mesh draw submission | Unreal caches `FMeshDrawCommand`s for static primitives at scene-add time and only re-sorts/merges visible ones; Unity's GPU Resident Drawer keeps instance data on the GPU through BatchRendererGroup and records with multiple threads | Every mesh renderer is extracted, copied (with its full `Material`) into a `Draw`, culled, sorted by a multi-field comparator and batched every frame; only foliage and statics groups are retained (ADR-0162) | ~0.4 µs of CPU per draw per frame (16,384 cubes: 6.83 ms per-frame vs 0.43 ms retained CPU time, ADR-0162) |
| Parallelism | Unreal Tasks System, parallel pass setup and `FDrawVisibleMeshCommandsAnyThreadTasks`; Unity C# Job System with Burst | A work-stealing job system exists, but six engine call sites use `parallel_for`; extraction, culling, sorting and command recording are single-threaded; no render thread | A 2-core and an 8-core machine run the frame's CPU work at nearly the same speed |
| ECS change tracking | Unity Entities stamps chunks with per-component change versions so systems skip unchanged chunks | No change versions; no chunking (one column per component per archetype) | Systems cannot cheaply skip unchanged data |
| Math | Unity.Mathematics + Burst emit SIMD; Unreal `VectorRegister` SSE/NEON paths | Scalar `Vec4`/`Mat4`, `Mat4::trs` and `Aabb::transformed` out of line, no LTO | Hot loops depend on auto-vectorisation across translation units that cannot inline |

## What this work changes (in order)

1. **Change-driven transform update (ADR-0226, done).** `Scene::update_transforms` keeps a flattened parent-first order with direct component pointers, valid while the hierarchy version and the new `World::structural_version()` are unchanged, plus a per-slot snapshot of the local `Transform` that produced each world matrix. Unchanged subtrees cost a 40-byte compare; changed ones recompute from the parent's cached matrix. This is Unity's packed-hierarchy model, adapted to VICTUS's plain mutable `Transform` component (no setters, so change detection compares values instead of reading dirty bits).
2. **`parallel_for` without per-chunk jobs (done, same commit).** One shared atomic cursor; at most `worker_count()` helper jobs, each claiming chunks with `fetch_add` (the batch-claiming scheme of Unity's `IJobParallelFor` and TBB's partitioners). Submit only takes the sleep lock and wakes a worker when one is asleep.
3. **Next: retained draw records for unchanged mesh renderers** (the Unreal cached-mesh-draw-command model) so extraction and sort keys are rebuilt only for entities whose WorldTransform, mesh or material changed — the natural consumer of the transform snapshots above.
4. **Sort keys and lighter draws (ADR-0227, done):** per-frame material interning (each `Draw` points at one shared `Material` instead of copying 264 bytes), draw lists sorted by interned state ranks with a stable radix sort, and affine normal matrices from cross products. Per-draw visibility runs on the job system in two phases (parallel classification, serial in-order gather; ADR-0228). Material interning hashes and compares packed signatures (ADR-0229). Unchanged keyed renderers are promoted into automatic instance groups (`submit_static`, ADR-0230; the analogue of Unreal's cached mesh draw commands and Unity's GPU Resident Drawer), cutting `render()` for 10k static cubes from about 2.0 to 0.3 ms. Extraction then walks renderers per archetype (optional components read from columns, Unity's chunk-iteration model) and finds retained records through a direct-mapped hint: extract 0.93 → about 0.68 ms. **Next:** parallel batching and instance packing.
5. **Inline hot math (done, a6d5dcd):** `Mat4::trs`, `Mat4::rotation`, `Aabb::transformed` and `Frustum::intersects` are header-inline. **Next:** an optional IPO/LTO Release build, measured.
6. **Later: ECS change versions per archetype column** so systems beyond transforms can skip unchanged data.

## Measured so far

See the ADR-0226 entry in [STATUS.md](../STATUS.md) for the commands. `victus_bench_scene` p50 on the sandbox:

- `scene/update_transforms_10k` (static scene): 0.562 ms → 0.033 ms.
- `scene/update_transforms_10k_all_moving` (every root moves, so every matrix is recomputed; new case): 0.16-0.25 ms against the old algorithm's 0.56 ms, which always recomputed everything.
- `scene/update_transforms_10k_component_churn` (a component added or removed every frame; new case): 0.18 ms.
- `jobs/parallel_for_integrate_1M`: heap allocations per call 7.6 → 0.075; time unchanged within the 2-vCPU sandbox's noise (±20%).
- `math/trs_compose_100k` (inline math): 4.17 → 3.58 ms.
- 16,384 per-frame cube draws, steady-state CPU per frame (ADR-0227, `victus_bench_render` "steady state" line, three alternating runs): submit 1.24-1.45 → 0.63-0.68 ms, `render()` 5.9-7.0 → 3.19-3.25 ms (visibility stage 3.2-3.7 → 1.8-1.9 ms). The retained instance-group path stays at about 0.4 ms.

## Sources (accessed 2026-11-23)

- Unity, "Best practices from the Spotlight Team: optimizing the hierarchy" (TransformHierarchy packing, TransformChangeDispatch): https://unity.com/cn/blog/engine-platform/best-practices-from-the-spotlight-team-optimizing-the-hierarchy
- Unity, "Unity's evolving best practices" (Unite talk slides, transform change dispatch): https://www.slideshare.net/slideshow/unitys-evolving-best-practices/113330551
- Unity Scripting API, `Transform.hierarchyCapacity`: https://docs.unity.com/ko-kr/engine/6000.3/script-reference/unityengine/transform/hierarchycapacity
- Unity Manual, GPU Resident Drawer: https://docs.unity.com/en-us/engine/6000.6/manual/analysis/graphics-performance-profiling/in-urp/reduce-draw-calls-urp/reduce-rendering-work-on-cpu/gpu-resident-drawer
- Unity Manual, how BatchRendererGroup renders: https://docs.unity.com/en-us/engine/6000.6/manual/analysis/graphics-performance-profiling/in-urp/reduce-draw-calls-urp/batch-renderer-group/how
- Unity Manual, choosing a draw-call optimisation method: https://docs.unity3d.com/ja/6000.0/Manual/optimizing-draw-calls-choose-method.html
- Unity Entities, `ArchetypeChunk.GetChangeVersion`: https://docs.unity3d.com/Packages/com.unity.entities@1.4/api/Unity.Entities.ArchetypeChunk.GetChangeVersion.html
- Epic, Mesh Drawing Pipeline (cached mesh draw commands, parallel pass setup): https://dev.epicgames.com/documentation/unreal-engine/mesh-drawing-pipeline-in-unreal-engine?application_version=5.8
- Epic, Tasks System: https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine
