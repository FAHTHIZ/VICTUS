# VICTUS Adaptive Geometry System — V / Vectors

**Agent handoff · owner-requested · design and implementation brief · 2026-11-18**

**Working name:** V (also referred to by the owner as “Vectors”). Keep the name as a subsystem identity, not a new math/vector module or a separate renderer.

**Status:** Approved direction for research, audit, and incremental implementation. This document does not claim that the proposed capabilities have been implemented. Work directly on \`main\`, following \`AGENTS.md\`.

## 1. Owner intent

The owner wants VICTUS to spend geometric detail intelligently:

- A surface close to the camera should retain the geometry needed for a convincing, highly detailed view.
- As the same surface occupies fewer pixels, the renderer should draw a substantially simpler representation while preserving the appearance that is still perceptible.
- The system should adapt to the actual capabilities and sustained performance of the device. A capable desktop and a constrained phone must not be forced to use the same geometry workload.
- Distant objects should remain visually coherent and crisp. Reducing triangle count must not turn the scene into visibly crude low-poly geometry.
- The capability should be useful to ordinary VICTUS projects, not hard-coded to Special Ops or any one showcase game.

The product goal is **the best useful geometric detail per unit of rendering and memory cost**, within a measurable quality and performance budget.

This is broader than conventional distance-based LOD, but it must start by improving VICTUS’s existing LOD pipeline. Do not create a second LOD implementation beside the one that already works.

## 2. Current-main audit — important: VICTUS already has mesh LODs

The following was verified from the repository at main commit \`3260825f90259e6d7f339cd1d58e226de717b128\`. Recheck main and these files before every implementation slice; the owner’s agent may have advanced the code.

### 2.1 Existing static/skinned mesh optimization and LOD generation

- \`engine/render/include/victus/render/mesh.hpp\`
  - \`MeshLod\` stores a coarser index list and a geometric error value in mesh units.
  - \`MeshData\` stores the full-resolution LOD 0 indices, skin stream, and up to \`kMaxMeshLods == 8\` entries (LOD 0 plus up to seven coarser levels).
  - The documented invariant is that collision, navigation, and CPU readers continue to use the full mesh indices.
  - \`LodSettings\` currently defaults to three coarser levels, a 0.5 triangle ratio per level, 0.05 maximum simplification error, and a minimum of 24 triangles.
- \`engine/render/src/mesh_optimize.cpp\`
  - VICTUS uses the pinned meshoptimizer 1.3 dependency (MIT), via \`meshopt_simplifyWithAttributes\`.
  - The simplifier currently weights normals, UVs, and the dominant joint index for skinned vertices. It simplifies every LOD from the full-resolution base, rather than cascading simplification errors from the previous LOD.
  - It then applies vertex-cache and overdraw ordering, remaps vertex fetch order, and keeps the skin stream aligned.
  - The current attribute metric does not include every possible authored attribute. In particular, inspect vertex colours, tangent frames, all skin influences, morph targets, seams, and material-specific boundaries before changing simplification policy.
- \`engine/assets/include/victus/assets/importers.hpp\` and \`engine/assets/src/importers.cpp\`
  - Model import defaults to mesh optimization and three generated coarser levels with 0.05 relative error. Settings are persisted in the asset \`.meta\` only when changed.
  - The glTF importer generates LODs per primitive, which keeps separate primitive/material sections separate. Preserve that boundary; do not simplify unrelated material sections as one surface.
  - Cooked model format and importer versions already account for LODs. Any representation/schema change must version and invalidate the cache correctly.
- \`engine/render/include/victus/render/gpu_context.hpp\`
  - The current \`GpuMesh\` has one vertex buffer, one optional skin buffer, and one concatenated index buffer with per-LOD ranges.
  - The current levels share the full mesh vertex/skin streams. A distant draw submits fewer indices and therefore can reduce triangle and vertex processing, but the high-detail vertex stream remains resident. This is **not yet a complete geometry-memory residency solution**.

### 2.2 Existing runtime selection

- \`engine/render/src/scene_renderer.cpp\` selects the coarsest level whose projected geometric error fits the configured pixel error budget.
- It projects mesh error using render height, camera vertical FOV, world transform scale, and distance to the nearest point of the draw’s world bounds. It is not merely a hard-coded “switch at X metres” rule.
- Current quality defaults include approximately 2.0 pixels on Low, 1.5 on Medium, the struct default on High, and 0.5 on Ultra. \`force_lod\` exists for debugging.
- The renderer batches instances by selected LOD, so instances at different LODs form different batches while identical far LODs can still instance together.
- Current selection is effectively a discrete switch. ADR-0049 records visible LOD popping as a known consequence; cross-fade/dithering is follow-up work.

### 2.3 Existing visibility, terrain, and profiling systems

- Scene rendering already has CPU frustum culling, automatic identical-mesh/material instancing, and Hi-Z occlusion culling from a depth pre-pass with compute and indirect draws.
- ADR-0051 explicitly records that GPU frustum culling and GPU LOD selection are not implemented, and SDL_GPU 3.4 does not expose a draw-count buffer for fully GPU-built draw lists.
- Terrain already uses chunk meshes with five index-list LODs and crack-hiding skirts. Keep the terrain-specific chunk/neighbor logic; do not replace it with a generic static-mesh LOD path.
- \`docs/KNOWN_LIMITATIONS.md\` records that foliage currently issues one draw per instance and is not GPU-instanced. This is a concrete, relevant gap for V and a scene with many repeated objects.
- The current RHI is VICTUS’s SDL_GPU-based abstraction. ADR-0003 records that the current path does not expose mesh shaders. The current profiler also lacks backend GPU timestamp queries; CPU frame timings and GPU wait time are not a substitute for actual GPU execution time.
- Android can build an APK, but \`docs/PLATFORM_SUPPORT.md\` states that no physical Android phone or emulator has yet run the renderer. No mobile performance, thermal, or battery claims are currently validated.

### 2.4 Existing evidence and what it proves

Relevant tests already exist:

- \`tests/render/mesh_lod_tests.cpp\`: simplifier output, increasing error bounds, triangle reduction, vertex remapping, skin alignment, and border-lock behavior.
- \`tests/assets/mesh_lod_asset_tests.cpp\`: importer settings, cooked LOD round-trip, and malformed cooked LOD rejection.
- \`tests/render/gpu_lod_tests.cpp\`: near/far selection, forced LOD, triangle counts, per-LOD instancing, and a far-view sphere image comparison.
- \`tests/render/gpu_occlusion_tests.cpp\` and the render statistics cover existing occlusion/culling paths.

These tests establish that the current discrete LOD path works for its covered fixtures. They do **not** prove robust LOD quality for production walls, modular environments, foliage, alpha-tested geometry, detailed characters, animated deformation, multiple views, or physical phones. Extend evidence with representative assets; do not mistake a sphere test for complete validation.

## 3. Core design principles

1. **Extend, do not duplicate.** Keep \`MeshData\`, \`MeshLod\`, meshoptimizer, cooked models, \`GpuMesh\`, \`SceneRenderer\`, the existing RHI, render graph, culling, and instancing as the foundation. Add narrow capabilities to those seams.
2. **Precompute, select at runtime.** Heavy simplification belongs in import/cook time or an explicit editor build step. Do not run the general mesh simplifier every frame.
3. **Screen-space quality, not distance alone.** Use projected geometric error and projected size. Distance is only one input; FOV, render resolution, object scale, view, and the asset’s error all matter.
4. **A frame-wide budget, not only per-object decisions.** Per-object error thresholds can still leave a scene with too many triangles or too much resident geometry. V should eventually allocate a view’s geometry budget across visible objects.
5. **Keep visual and gameplay representations separate.** LOD changes must never silently alter collision, navigation, hit queries, authoritative simulation, or save data. Continue using LOD 0 or an explicitly cooked gameplay collision mesh for those systems.
6. **Preserve important features.** Simplification must account for silhouettes, open borders, hard normals, UV seams, vertex colours, material sections, alpha-tested cutouts, skin deformation, and eventually morph-target deformation.
7. **Make every automatic decision inspectable and overridable.** Artists and developers need to see selected LOD, source triangle count, submitted triangle count, projected error, reason for selection, memory cost, and any budget pressure. Provide opt-outs and per-asset/per-instance controls.
8. **Mobile-first portability.** The standard path must work on the existing indexed-draw RHI and constrained GPUs. Advanced hardware paths are optional enhancements, never required for a game to render.

## 4. V architecture — layers that work together

Treat V as a coordinated policy and diagnostics layer over existing systems, not necessarily as a new top-level CMake module.

### Layer A — Asset-side geometry preparation

At import/cook time:

- Retain the source LOD 0 mesh unchanged as the quality and gameplay reference.
- Generate several useful simplification levels with geometric error metadata. Avoid generating many levels that remove too few triangles to justify their storage and management overhead.
- Support manually authored LODs where automatic simplification cannot preserve the intended silhouette or topology.
- Add validation for monotonic error, valid index ranges, non-degenerate triangles, minimum useful triangle reduction, bounds consistency, material-section preservation, and cooked format integrity.
- Investigate attribute-aware simplification settings for normals, UVs, vertex colours, tangents, all skin influences, and feature importance. Do not blindly change current weights: use representative meshes and measured visual comparisons to choose them.
- Check the pinned meshoptimizer 1.3 API before using flags or algorithms found in newer upstream documentation. In particular, evaluate whether \`meshopt_SimplifyErrorClamped\` is available and appropriate for attribute-aware error values used for LOD selection.
- Allow protected/high-priority vertices or boundary locking where justified. Do not lock every edge by default: excessive locking can prevent useful simplification.
- Treat each material/primitive section independently unless a tested representation explicitly preserves material boundaries.
- Keep skinned LODs compatible with the same skeleton and skin stream. Validate deformation at multiple animation poses, not only in bind pose.
- Do not claim morph-target/facial LOD support until morph targets are actually imported, cooked, rendered, and tested. Current status reports glTF morph-target weights are skipped.

### Layer B — Per-view LOD selection and transition stability

Improve the current \`select_lod\` path rather than replacing it.

- Base selection on the actual render view’s resolution and projection, projected mesh error, transform scale, bounds, and per-asset quality settings.
- Support per-asset LOD bias, per-instance minimum/maximum detail, a pinned LOD for debugging, and an explicit “never simplify” choice for hero assets or critical silhouettes.
- Add temporal stability. A small hysteresis band or a stateful threshold should prevent rapid oscillation when an object sits on a boundary. State must be scoped safely to the relevant render instance/view and must not leak across cameras or scene lifetimes.
- Add optional transition dithering or cross-fade after measuring its cost. Rendering both levels at once costs extra vertex/fragment work and overdraw; it should not be mandatory on Low/mobile tiers.
- Evaluate separate LOD decisions for the main camera, shadow cascades, reflection/probe captures, editor previews, and other render views. A main-camera distance is not automatically the correct error metric for every pass.
- Keep transparent and alpha-tested geometry on a tested path. Alpha-cutout leaves, hair cards, wires, and thin geometry can disappear or change silhouette under naive simplification.
- Preserve deterministic behavior for equivalent inputs and stable captures.

### Layer C — Hardware-aware geometry budgets

V must understand more than the device name.

Build a profile from measured capabilities and selected runtime settings, including:

- RHI/backend capabilities and supported formats/features.
- Available GPU/CPU memory and current asset residency.
- Render resolution and view configuration.
- Measured CPU and, when supported, GPU frame timings.
- Draw submissions, batches, submitted triangles/indices, visible instances, and streaming/upload pressure.
- Sustained thermal/power state when a platform API is available and the project opts into it.

Define named quality profiles (for example, High, Standard, Mobile, Compatibility) with editable budgets. Profiles are safe defaults, not hard-coded claims that a specific chipset can sustain a particular scene.

Eventually add a per-view scene budget allocator:

1. Calculate each visible draw’s desired LOD from projected error.
2. Estimate its cost at each available LOD (triangles/indices, vertex references, batch impact, and any additional residency cost).
3. Prioritize important objects and visible detail. Hero/near-camera characters, faces, hands, and high-contrast silhouettes should not be degraded like low-importance distant clutter.
4. If the desired set exceeds the frame budget, degrade lower-priority objects in measured increments. Do not apply a uniform distance multiplier to everything.
5. Report when the budget cannot be met without exceeding the allowed visual error. Never silently claim both quality and performance targets were met when they were not.

Use slow, smoothed adaptation with hysteresis to prevent visible quality oscillation. Allow users to override automatic profiles. A thermal response should reduce optional rendering workload before the device reaches sustained throttling, but should not alter authoritative simulation rules.

**Important measurement limitation:** current SDL_GPU integration does not provide the GPU timestamp-query path documented as pending in VICTUS’s profiler status. Do not infer GPU execution time from CPU submission time or GPU wait time. Until reliable asynchronous GPU timing exists on a backend, use platform GPU profilers and reproducible device benchmarks to tune profiles; do not build a fake “GPU load detector.”

### Layer D — Geometry memory and residency

The current shared-vertex-buffer LOD representation reduces the number of indices drawn but keeps the LOD 0 vertex/skin buffers resident. V should investigate whether separate compact vertex/index payloads per LOD, using meshoptimizer’s vertex-fetch/compaction facilities, are beneficial.

Compare at least these options:

- **Shared vertex buffer + multiple index ranges:** current low-complexity path; minimizes duplicated vertex data, but keeps high-detail vertex data resident.
- **Compact per-LOD vertex/index streams:** each level contains only referenced vertices; can reduce upload/fetch and permit selective residency, but can duplicate vertices and increase metadata/buffer count.
- **Paged/streamed detail:** keep coarse representations resident and load/unload finer geometry based on view demand and memory budget. Needs robust asynchronous IO, upload scheduling, fallback behavior, and cache accounting.

Do not adopt a new representation just because it sounds more advanced. Measure cooked size, resident CPU/GPU bytes, upload traffic, vertex work, draw cost, and cache behavior on target hardware. Preserve old cooked-format compatibility through explicit versioning and cache invalidation.

Also distinguish **simplification** from **compression**. VICTUS currently uses meshoptimizer for mesh optimization and LOD simplification; that does not by itself mean that its cooked mesh payloads use meshoptimizer vertex/index compression or glTF \`EXT_meshopt_compression\`. Inspect the real cook format before describing or changing compression.

### Layer E — Visibility, batching, terrain, and distant-world representation

- Reuse existing frustum and Hi-Z occlusion culling; profile their CPU/GPU cost and benefits at each quality tier.
- Preserve per-LOD instancing. Fix and benchmark the existing foliage limitation: many scattered instances currently cause one draw per instance.
- Consider HLOD for distant static groups: combine or represent groups of small static objects with simplified meshes/material atlases or another appropriate representation. Preserve occlusion boundaries, material correctness, and useful culling granularity.
- Keep terrain’s chunk LOD and seam/skirt logic terrain-aware. Evaluate neighbor-level constraints and cracks under camera movement, streaming, and terrain edits.
- Consider impostors/billboards only for suitable very distant assets, with lighting/view-angle limitations tested.
- A meshlet/cluster path can be investigated for finer culling and streaming, but do not require mesh shaders to deliver V’s baseline.

### Layer F — Advanced geometry research (later, not the first implementation)

Research clustered/meshlet rendering and virtualized geometry as a possible future tier.

Unreal Nanite is a useful public reference for concepts such as cluster-based geometry, automatic detail selection, compression, and fine-grained streaming. It is not a specification to clone, and the public feature set does not mean those techniques are appropriate on every mobile GPU.

The current VICTUS SDL_GPU RHI does not expose mesh shaders, and Vulkan \`VK_EXT_mesh_shader\` is an optional capability that must be queried and enabled. The Vulkan documentation itself cautions that mesh shaders are not automatically faster than the traditional vertex path. Therefore:

- Do not make mesh shaders, task shaders, bindless resources, or a Nanite-style renderer a dependency of the first V release.
- Do not claim V is “Nanite for mobile” or an equivalent virtualized geometry system.
- First prove the portable indexed-mesh path, LOD quality, memory budgets, culling, batching, and physical-device performance.
- Only pursue a clustered/virtualized path after a measured bottleneck and a backend/API capability audit justify the cost. Keep it behind an optional renderer path with a conventional fallback.

## 5. Important visual-quality distinction: geometry is not the entire image

The owner’s “close wall looks extremely detailed, far wall stays crisp” goal needs several systems working together.

- Geometry LOD preserves silhouette, large shapes, gaps, corners, and other features that affect the outline or depth.
- Normal maps, material textures, texture mip chains, filtering, and appropriate compression preserve surface appearance when tiny geometric details are sub-pixel.
- Anti-aliasing and temporal stability reduce shimmer and crawling details.
- Displacement/tessellation may add geometric detail on platforms that support it, but it is not a universal mobile baseline and is not a substitute for authored LODs.
- A large flat wall may need very little geometry even at close range; its perceived detail may come mainly from materials and shading. Do not waste triangles reproducing texture detail that the pixel shader can represent more efficiently.
- Conversely, a silhouette-changing bolt, broken edge, railing, leaf, or character contour may still need geometry when it occupies enough pixels.

The correct choice is content- and view-dependent. Do not use polygon count as the sole quality metric.

## 6. Character and Special Ops requirements

Special Ops can later serve as a demanding real-game validation project, but V must remain general-purpose.

For character assets:

- Maintain a high-quality close representation for characters that occupy a substantial part of the screen.
- Validate the silhouette, face profile, hands, clothing outline, and deformation at multiple animation poses and camera distances.
- Never simplify across material/UV seams or joint boundaries without dedicated visual tests.
- Check all skin weights and the same skeleton binding across levels. The current dominant-joint attribute heuristic is only one part of preserving deformation quality.
- Keep morph-target facial detail out of automatic LOD claims until the importer and runtime support morph targets end to end.
- Evaluate animation/geometry LOD separately from mesh triangle LOD. Reducing bone updates, facial evaluation, or cloth simulation frequency is a separate policy with separate correctness and visual tests; V must not silently change those systems.

For walls and environments:

- Retain important corners, holes, thin structures, trim, and silhouette-changing damage.
- Test baked material/normal detail and texture mips along with geometry changes.
- Modular pieces must not create visible cracks or mismatched borders as neighboring LODs change.

## 7. Recommended implementation order

This is a sequence of verified vertical slices, not permission for a speculative renderer rewrite.

### Slice 0 — Re-audit and record baselines

1. Re-read current main, \`AGENTS.md\`, \`ROADMAP.md\`, \`docs/STATUS.md\`, \`docs/KNOWN_LIMITATIONS.md\`, \`docs/RENDERING_SPEC.md\`, ADR-0049, ADR-0051, and the exact source/tests listed above.
2. Record current meshoptimizer version/hash, cooked format/importer versions, importer defaults, selection formula, per-pass behavior, and available RHI capabilities.
3. Establish repeatable geometry test assets: a detailed architectural wall with openings/trim, modular environment pieces, a detailed prop, thin/alpha-cutout foliage, and a skinned character with multiple animation poses. Add no third-party copyrighted game assets.
4. Capture baselines: LOD0/LOD1/... triangle counts, cooked bytes, CPU/GPU buffer bytes where measurable, draw batches, render output comparisons, CPU frame time, and available external GPU timings.
5. Record which facts cannot yet be measured on a physical phone. Do not substitute lavapipe CI timings for mobile GPU results.

### Slice 1 — Correctness and asset quality

1. Audit simplifier behavior for hard edges, UV seams, vertex colours, material sections, open borders, alpha cutouts, and skin deformation.
2. Compare current attribute weights to representative assets. Evaluate error clamping and vertex protection supported by the pinned meshoptimizer version.
3. Add deterministic import tests and image/regression tests for the representative content. Include skinned poses, not just bind-pose meshes.
4. Fix demonstrable artifacts and ensure cache-version changes are correct. Do not change default LOD ratios globally without before/after evidence.

### Slice 2 — Stable per-view selection and creator diagnostics

1. Add hysteresis or another measured way to avoid rapid threshold switching.
2. Evaluate optional cross-fade/dither transitions and provide a low-cost fallback.
3. Validate main-camera, shadow, probe/reflection, editor and multi-camera selection independently.
4. Add a renderer/editor debug view: colour by LOD, wireframe, LOD index, original/current triangle count, geometric error, projected pixel error, and forced-LOD controls.
5. Add per-asset and per-instance controls only where they fit existing reflection, serialization and inspector conventions.

### Slice 3 — Hardware profiles and scene geometry budgets

1. Define device capability/profile data from the existing RHI and platform seams; do not branch on marketing device names.
2. Establish target frame and memory budgets on named hardware before choosing numeric targets.
3. Add a per-view budget allocator that degrades low-priority objects first while reporting quality-budget violations.
4. Add slow, stable adaptation and manual override. Use GPU timing only after a trustworthy asynchronous RHI timing capability exists.
5. Add telemetry and tests for oscillation, profile changes, camera changes, thermal-state changes, and deterministic captures.

### Slice 4 — Residency and mobile asset costs

1. Benchmark the current shared vertex buffer against compact per-LOD streams.
2. Evaluate mesh vertex/index quantization/compression and cooked payload size with explicit format/version compatibility.
3. Add streaming only with async IO/upload budgets, memory accounting, cache eviction, fallback LODs, and no rendering stalls or missing geometry.
4. Test cold and warm transitions, rapid camera turns, memory pressure, scene changes, and device lifecycle events.

### Slice 5 — World-scale geometry and draw-cost improvements

1. GPU-instance foliage and repeated static content using the existing renderer’s batching path.
2. Evaluate HLOD for static groups and suitable impostors for very distant content.
3. Preserve the specialized terrain chunk/neighbor LOD system and validate it alongside object LOD.
4. Only then profile whether meshlet/cluster or virtualized geometry research is justified.

## 8. Required validation and completion gates

V must not be marked complete because one far-away test sphere uses fewer triangles.

### Correctness and quality

- Every LOD has valid indices, bounds, monotonic geometric error, and useful triangle reduction.
- The simplified image stays within a documented, reproducible error/quality budget for representative assets at several resolutions and FOVs.
- No unacceptable popping, shimmer, cracks, material changes, UV distortion, missing cutouts, broken silhouettes, or visible skinning defects.
- LOD transitions are stable under camera movement and object motion.
- Multiple cameras and shadow/probe views select appropriate detail.
- Gameplay collision/navigation and authoritative state remain independent of render LOD.
- Malformed cooked meshes fail safely; cache versioning, import determinism, and asset validation remain covered.

### Performance and memory

Measure on release builds and named hardware:

- CPU scene/LOD selection time.
- CPU submission time and draw/batch counts.
- Submitted indices/triangles and, where measurable, vertex invocations.
- Resident CPU/GPU vertex, skin, index, and total asset bytes.
- Import/cook time, cooked file size, upload bytes/time, streaming misses/stalls, and peak memory.
- CPU/GPU frame-time percentiles, frame pacing, and variance.
- Sustained device temperature/thermal state and battery/power behavior where available.

Run equivalent scenes and quality settings on at least low-, mid-, and high-capability physical Android devices before publishing mobile claims. Record device model, SoC/GPU, OS/build, driver, refresh rate, power mode, resolution, quality profile, thermal state, scene, cache state, build configuration, repetitions, and raw results. Include sustained play/soak testing; a short cool-device benchmark does not prove long-session performance.

### Creator workflow

A developer must be able to:

- Import a model and inspect generated LODs.
- Preview the selected LOD as the camera moves.
- See why a level was selected and what it costs.
- Set an asset/instance override or disable LOD where necessary.
- Validate LODs and receive actionable warnings through the existing \`victus check\` / export path where appropriate.

### Completion claim

Only claim V is a robust, hardware-aware adaptive geometry system when the portable baseline, content preservation, transitions, profiling, memory behavior, editor diagnostics, automated tests, and physical-device evidence all exist. Describe advanced virtualized/mesh-shader functionality separately unless it is implemented and validated.

## 9. Research references

These are public technical references, not instructions to copy proprietary implementations. Re-check the exact version, license, platform support, and API before adopting any code or dependency.

1. **meshoptimizer — core pipeline and simplification documentation**  
   https://github.com/zeux/meshoptimizer  
   Relevant: indexing, vertex-cache ordering, overdraw ordering, vertex-fetch ordering, quantization, simplification, attribute-aware simplification, vertex locks, and error clamping. VICTUS already pins meshoptimizer 1.3 under MIT; compare upstream recommendations against that exact pinned release before using newer API flags. The upstream documentation notes that target triangle counts may not be reached when topology or error constraints prevent further simplification, and that attribute-aware error values should be handled carefully when used for LOD selection.

2. **Unity — LOD Group reference (current 6000.3 documentation)**  
   https://docs.unity.com/en-us/engine/6000.3/manual/analysis/graphics-performance-profiling/lod/lod-group/class-lodgroup  
   Relevant: transitions based on screen-relative object height, previewing LOD changes, and configurable transition widths. This is a useful creator-workflow reference, not a requirement to copy Unity’s component model.

3. **Unreal Engine — Nanite Virtualized Geometry overview**  
   https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine  
   Relevant: cluster-oriented virtualized geometry, fine-grained visibility/detail, compression, and geometry streaming. Use it as a high-level research reference only; do not equate discrete LODs with Nanite or promise a Nanite-equivalent system.

4. **Unreal Engine — Nanite technical details**  
   https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details  
   Relevant: fallback representations and relative simplification error. A conventional fallback mesh remains useful when the advanced path is unsupported or other engine systems require traditional geometry.

5. **Android Developers — Optimize power efficiency**  
   https://developer.android.com/games/optimize/power  
   Relevant: sustained CPU/GPU load causes heat and thermal throttling; use thermal-state information to reduce optional workload rather than tuning solely for a short benchmark.

6. **Vulkan Documentation — VK_EXT_mesh_shader**  
   https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html  
   Relevant: mesh/task shader architecture, capability/limit queries, and the fact that these are optional programmable geometry stages. The specification explicitly notes that mesh shaders are not guaranteed to outperform a well-optimized traditional vertex path.

7. **Khronos glTF extension registry — EXT_meshopt_compression**  
   https://github.com/KhronosGroup/glTF/blob/main/extensions/README.md  
   Relevant: glTF’s registered meshoptimizer compression extension. Do not conflate the presence of meshoptimizer simplification in VICTUS with use of this compression extension.

## 10. Agent instructions

- Treat this as an actionable approved handoff, but follow the existing roadmap order and verify every baseline assumption against current main.
- Do not create \`engine/vectors\`, a second renderer, a parallel LOD data model, or a replacement for working meshoptimizer code.
- Do not make Nanite, mesh shaders, or a proprietary engine’s feature set the acceptance target for the first milestone.
- Do not inflate polygon counts or frame-rate claims. Baseline first; measure changes on actual hardware.
- Preserve LOD 0 for gameplay/physics/navigation unless a separately validated collision representation is intentionally cooked.
- Add source-linked tests, diagnostics, docs, and a real sample consumer with every implementation slice.
- Update \`ROADMAP.md\` and \`docs/STATUS.md\` only with facts supported by completed and verified work. Keep hardware blockers explicit.
- Commit coherent verified changes directly to \`main\`, with no branch or PR, and continue to the next unblocked task under \`AGENTS.md\`.
