# Profiling and character movement: VICTUS against Unity 6.6 and Unreal 5.8/5.9 (2026-11-22)

Research note for GAP-04 (GPU timings) and the profiler and character rows of
[ENGINE_LANDSCAPE_AND_GAP_AUDIT.md](ENGINE_LANDSCAPE_AND_GAP_AUDIT.md). It compares documented features only. No competitor was run, and no benchmark numbers are claimed.

## Profiling

### What VICTUS has (checked in source at c5b6238)

- `core::FrameProfiler` (`engine/core/include/victus/core/profiler.hpp`): scoped CPU zones per thread, per-scope statistics over a sample window, and `record(N)`, which writes the next N frames as Chrome/Perfetto trace-event JSON. A `FrameReport` carries extra host counters (memory, GPU resources, draw calls).
- `core::memory::allocation_stats()`: process-wide totals (allocations, frees, live and peak bytes). These are recorded only when the executable links `victus_alloc_tracking`; there are no call stacks or per-asset tags.
- Studio Profiler panel (`editor/src/profiler_panel.cpp`): `pass.*` scopes are CPU encode time per render pass, and `gpu.*` scopes are CPU time spent waiting on the GPU (swapchain, readbacks). GPU texture and buffer counts and bytes come from `rhi::DeviceStats`.
- There are no GPU timings. `render_path.hpp` reports `gpu_timing` as "unavailable: the SDL_GPU backend exposes no GPU timestamp queries" (GAP-04).
- Capture/replay with divergence reports, frame-sequence capture and `victus compare` cover correctness, not performance.

### Unreal Insights (5.8, 5.9 docs)

- Architecture: trace events are recorded by the Unreal Trace Server and analysed in the Unreal Insights application. Memory Insights traces every allocation, reallocation and free, with call stacks and LLM tags. Its query system finds live allocations at a point in time, short- and long-lived allocations, and leaks. Android call-stack tracing has been supported since 5.4 ([Unreal Insights](https://dev.epicgames.com/documentation/unreal-engine/unreal-insights-in-unreal-engine?lang=it), [Memory Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/memory-insights-in-unreal-engine)).
- Channels gate the data rate. Examples include CPU, GPU, Frame, LoadTime, MemAlloc, Net, NetworkPrediction, Physics (Chaos Visual Debugger), RDG, Slate, Task and Screenshot. Presets group channels ([Unreal Insights reference](https://dev.epicgames.com/documentation/unreal-engine/unreal-insights-reference-in-unreal-engine-5?application_version=5.5&lang=it)).
- A third-party summary of the 5.8 release notes, not cross-checked against Epic's notes, lists these additions:
  - World Partition Insights.
  - Audio Insights is production-ready.
  - A "snapshot hitches" feature writes trace data and screenshots when a hitch is detected.
  - Trace snapshots can be added to crash reports (Windows).
  - TraceQuery, a command-line tool that turns `.utrace` files into JSONL for scripted analysis.

  Source: [irurueta.com profiling notes](https://www.irurueta.com/graphics-games/unreal-engine/profiling-and-optimization.html).

### Unity Profiler (6000.6 docs)

- Modules: Highlights, Screenshots, CPU, GPU, Rendering, Memory, Audio, Video, Physics, Physics 2D, UI Toolkit, UI Toolkit Details, uGUI, Realtime GI, 2D, 2D Tilemap, Virtual Texturing, Adaptive Performance, File Access and Asset Loading. GPU Usage is off by default because of its overhead ([Profiler window reference](https://docs.unity.com/en-us/engine/6000.6/manual/analysis/profiler/window)).
- The Highlights module flags frames over a target frame time in separate CPU and GPU lanes. It reports main-thread utilisation, bottleneck counts, the systems with the most impact, and GC allocations with their top contributors ([Highlights module](https://docs.unity.com/en-us/engine/6000.6/manual/analysis/profiler/visualizing-data/highlights)).
- Separate tools: Memory Profiler (snapshots, package 1.1.x; [manual](https://docs.unity3d.com/Packages/com.unity.memoryprofiler@1.1/manual/index.html)), Profile Analyzer (comparing two captures), and Project Auditor (static analysis of scripts, assets and settings, built into the Editor from 6.4; [package manual](https://docs.unity3d.com/Packages/com.unity.project-auditor@1.1/manual/index.html), [Unity guide](https://unity.com/how-to/profiling-and-debugging-tools)).

### Gaps and what to borrow

| Capability | Unity | Unreal | VICTUS | Proposed step |
|---|---|---|---|---|
| GPU time per pass | GPU Usage module | GPU/RDG channels | None (GAP-04) | Vulkan/GLES timestamp extension behind the RHI, as GAP-04 says. The Highlights-style CPU/GPU split depends on this. |
| Over-budget frame view | Highlights (target FPS, CPU vs GPU lanes) | Frames panel and longest-event shortcuts | Per-scope averages only | Add a target frame time to the Profiler panel and mark frames over it. The CPU side is possible today. |
| Allocation call stacks and leaks | Memory Profiler snapshots | Memory Insights with LLM tags | Process totals only, opt-in | Per-module tags on `record_alloc` first (cheap). Call stacks later. |
| Hitch capture | Not reviewed | Snapshot hitches (trace plus screenshot), per the third-party summary | `record(N)` needs a manual start | Ring-buffer the last N frames and write a trace automatically when a frame exceeds a threshold. Fits the existing `FrameProfiler`. |
| Scripted trace analysis | Profile Analyzer (GUI) | TraceQuery to JSONL | Chrome JSON is already scriptable | Document `jq`/Python recipes in DIAGNOSTICS.md. No new tool is needed. |
| Static project audit | Project Auditor | Not reviewed | `victus check` (validation, not performance) | Add performance rules to `victus check`, such as texture variant size (the 3D template pack is 73 MB, see PERFORMANCE_STRATEGY). |

## Character movement

### What VICTUS has

- `physics::CharacterControllerComponent`, a kinematic capsule on Jolt CharacterVirtual. It has radius, height, `max_slope`, `step_height` (stairs and ground snapping), gravity scale, mass and `push_force` for shoving dynamic bodies, `air_control`, layer and mask. `MovingPlatformComponent` (a no-code behaviour) carries characters standing on it, and gameplay drives the controller with `move_character()` and `jump_character()` (`engine/physics/include/victus/physics/physics.hpp`).
- A 2D equivalent (`CharacterController2D`) and no-code behaviours: side-view run and jump, top-down or ground walking (`engine/script/include/victus/script/behaviours.hpp`).
- No movement-mode objects (walk, fall, swim, fly as separate states), no crouch or stance component, no root-motion layering, and no non-capsule shapes for characters.
- Networking predicts only the local player, with no rollback of other entities (see [NETCODE_RESEARCH_2026_11.md](NETCODE_RESEARCH_2026_11.md)).

### Unreal Mover (Experimental in 5.8)

- Mover is a modular movement plugin with rollback networking through the Network Prediction plugin or Chaos Networked Physics. Epic intends it to replace the Character Movement Component (CMC) eventually but still labels it Experimental. Epic also says CMC stays supported for the foreseeable future ([Mover](https://dev.epicgames.com/documentation/en-us/unreal-engine/mover-in-unreal-engine)).
- Epic's comparison table describes Mover as modular, actor-type agnostic, with broader shape support, unified rollbacks, client-side corrections, guarded state, server-driven move cadence, networked physics support, and "yet untested". CMC is described as monolithic, needing `ACharacter`, capsule-only, and battle-hardened ([Comparing Mover and CMC](https://dev.epicgames.com/documentation/en-us/unreal-engine/comparing-mover-and-character-movement-component-in-unreal-engine)).
- The physics backend lives in the ChaosMover plugin (Experimental, 5.8 API docs). It has crouch, launch and requested-move inputs, a swimming mode, pathed movement for platforms, and a predicted-trajectory output ([ChaosMover API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/ChaosMover?application_version=5.8)).
- Physics-driven Mover pawns run ahead of the game thread at a fixed tick, which adds some input delay. They do not synchronise with Network Prediction-driven pawns ([Mover examples](https://dev.epicgames.com/documentation/unreal-engine/mover-examples-in-unreal-engine?lang=en-US)).

### Unity

- The built-in `CharacterController` is a capsule with Slope Limit, Step Offset, Skin Width and Min Move Distance. It does not react to forces or push rigidbodies by itself; scripts do that from `OnControllerColliderHit` ([component reference](https://docs.unity.com/en-us/engine/6000.6/manual/physics-section/physics-overview/character-control-section/class-character-controller)).
- `com.unity.charactercontroller` (1.4.5 for 6000.0) is an ECS kinematic controller with first- and third-person Standard Characters samples ([package page](https://docs.unity3d.com/6/Documentation/Manual/com.unity.charactercontroller.html)).

### Where VICTUS stands

- Against Unity's built-in controller, VICTUS's capsule is comparable or ahead on documented features. It pushes dynamic bodies itself and has air control, and the moving-platform behaviour carries it.
- Against Mover's design, the missing pieces are:
  - Movement modes as data, with transitions.
  - Layered moves (root-motion-like additive moves).
  - Stance modifiers (crouch).
  - Rollback-aware state for every predicted entity.
- VICTUS should not copy Mover's dual backends. It has one physics engine per dimension, so a single fixed-tick mode stack on top of `CharacterControllerComponent` is enough. That would be a new ADR, and it would also unblock predicted non-player entities in the netcode research.

## Sources not reviewed

- Unreal Insights GPU channel cost on mobile.
- Unity Profiler on-device overhead figures.
- Unreal 5.9 Mover changes (the 5.9 release notes were not read for Mover).
