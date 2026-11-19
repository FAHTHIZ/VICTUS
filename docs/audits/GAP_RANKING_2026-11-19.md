# Missing-capability ranking (2026-11-19)

Engine-features session. Method: read ROADMAP.md (every open `[ ]`/`[~]` item and priorities 6b-6l), docs/KNOWN_LIMITATIONS.md and the code paths named below, then score each gap on three 1-5 axes:

- **Impact**: how much it limits shipping a complete, competitive game today.
- **Gap**: how much is missing (5 = nothing exists).
- **Fan-out**: how many other systems wait on it.

Score = Impact x Gap + 2 x Fan-out. Items owned by another session (ROADMAP "Parallel sessions") are listed for completeness but not taken: Studio/editor UX (6c), clang-tidy, foliage, platformer, A.E.G.I.S. (6e), animation depth (6i), plugin SDK, Workstream G (6d).

| Rank | Gap | Evidence in code today | I | G | F | Score | Owner |
|---|---|---|---|---|---|---|---|
| 1 | **V.A.U.L.T. runtime resource manager (6g)**: async requests, leases, upload budgets, residency/eviction, then texture-mip streaming | `GpuAssetCache::texture()/model()` read, decode and upload synchronously the first time `extract_scene` meets an asset, so every new asset stalls the frame; no byte accounting, budgets, priorities, cancellation or eviction policy | 5 | 5 | 5 | 35 | this session |
| 2 | **World and scene streaming (6h)**: cells, streaming sources, additive load/unload, cross-cell references | Scenes load whole (`Scene::load`); KNOWN_LIMITATIONS: "no additive/streamed loading"; needs rank 1 for async dependencies | 5 | 5 | 3 | 31 | this session (after 1) |
| 3 | **A.R.C. articulated characters (6f)**: ragdolls from skeletons, pose-driven motors, partial/full blend, get-up | No ragdoll code anywhere (`Ragdoll` appears in no engine source); Jolt 5.7.0 ships `RagdollSettings`/`Ragdoll` with motor pose driving | 4 | 5 | 2 | 24 | this session (physics claim) |
| 4 | **Destruction (6j)**: pre-fractured assets, clusters, connectivity, bounded debris | Only breakable joints exist | 4 | 5 | 1 | 22 | this session |
| 5 | **Gameplay framework primitives**: game mode/state, tags, attributes, timers, inventory/ability modules (ROADMAP "Gameplay framework") | Games hand-write these in Luau per sample | 4 | 4 | 2 | 20 | unclaimed |
| 6 | **Water rendering and buoyancy** | KNOWN_LIMITATIONS 3D rendering: "no ... water" | 4 | 5 | 0 | 20 | unclaimed |
| 7 | **Acoustic occlusion, rooms and zones (6k)** | Voices/budget done (ADR-0083); no occlusion, transmission, zones or early reflections | 3 | 4 | 1 | 14 | this session |
| 8 | **GPU particle simulation** | CPU emitters only (ADR-0084/0085 extended them) | 3 | 4 | 0 | 12 | done (ADR-0097); alpha emitters depth sorted on the GPU (ADR-0107); birth/death sub-emitters (ADR-0108); GPU trails open |
| 9 | **Volumetric fog / clouds / weather** | Height fog only | 3 | 4 | 0 | 12 | fog done (ADR-0098); clouds/weather open |
| 10 | **Script debugging (breakpoints, stepping)** | Trace debugger for graphs only | 3 | 4 | 0 | 12 | done (ADR-0099): breakpoints, stepping, inspection, DAP adapter; Studio play integration done (ADR-0106); attach to a running player open |
| 11 | **Versioned format migrators and editor safe mode (6l)** | Every format still at v1; no safe mode | 3 | 3 | 1 | 11 | done: migrators (ADR-0100), editor safe mode (ADR-0101) |
| — | **Perceptual capture/compare harness** (handoff Slice 1) | Only `assets::image_psnr`; renders not repeatable run to run | | | | | done (ADR-0102): FLIP/SSIM/worst tile/temporal, `victus compare`, deterministic captures; rendering-path reason codes (ADR-0103); deterministic exterior/interior reference scenes with lavapipe goldens (ADR-0104); decals/water/particles/probes in the path report and an effects reference scene (ADR-0105); per-tier effect budgets with tier/setting/budget reasons (ADR-0109); GPU cost blocked on SDL_GPU timestamp queries |
| — | Studio UX, A.E.G.I.S., animation depth, plugin SDK | see owners | | | | | other sessions |
| — | Hardware validation (priority 9) | blocked on physical Windows/macOS/Android devices | | | | | blocked |

## Order of work

1. V.A.U.L.T. slice 1: asynchronous texture/model requests on the job system, deduplicated, prioritised, with a per-frame GPU upload budget, placeholder/skip behaviour while loading, failures cached with context, cancellation of unreferenced requests and stats. Slice 2: residency accounting with CPU/GPU byte budgets and reference-aware LRU eviction. Slice 3: texture-mip streaming by projected footprint.
2. World streaming on top of V.A.U.L.T.
3. A.R.C. ragdoll foundation, then powered pose tracking.
4. Destruction, then the remaining items in score order.

Each slice follows AGENTS.md: research, implement, test, document, commit to `main`.
