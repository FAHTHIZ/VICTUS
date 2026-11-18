# VICTUS Camera-Responsive Visual Fidelity and Per-View Budgets

**Owner-approved cross-system engineering handoff · 2026-11-18**

**Status:** Approved design and audit brief. This is not an implementation claim. Execute in dependency order on main, rechecking all source assumptions before changing code.

**Coordinates with:** VICTUS Adaptive Geometry System (V / Vectors), Competitive Maturity and Production-Readiness, VICTUS Studio, and Signature Starter Characters.

## Mission

Make VICTUS preserve the detail that matters in the image a player or creator is actually viewing, while avoiding work that contributes little or nothing to that image. The renderer must respond to camera projection, zoom, output resolution, view purpose, device capability and sustained workload—not distance or a one-size-fits-all quality switch.

This is a coordinated system, not a promise of free FPS. When geometry is the bottleneck, removing invisible or sub-pixel geometric work can improve performance. When fill rate, texture bandwidth, shadows, animation, CPU submission or thermals dominate, geometry changes alone may have little effect. The system must identify the bottleneck and prove the result.

## 1. Verified current-main baseline

The following were verified in the 2026-11-18 main tree. Recheck before implementation.

### 1.1 Main-camera geometry already reacts mathematically to FOV

- Camera3D exposes vertical FOV.
- In scene_renderer.cpp, the active main-view LOD scale is calculated from render height divided by twice the tangent of half the camera's vertical FOV.
- The draw's LOD selection then projects mesh simplification error using that pixel scale, distance to the nearest point of world bounds and model scale.
- Therefore, when the active camera narrows its FOV, pixels per world unit rise and the existing selection should choose a finer LOD on the next rendered frame. Camera dolly/physical movement also changes the distance term.
- This is a source-level behaviour, not yet a dedicated zoom-regression guarantee. Existing gpu_lod_tests cover near/far selection, forced LOD, batches and a far-view image comparison, but do not explicitly vary FOV, target resolution or a second simultaneous view.

### 1.2 Current geometry limitations

- The current renderer computes one LOD choice for each draw from the active SceneRenderer camera. Do not assume that one choice is correct for every independent camera, reflection capture, editor viewport or light-space shadow view.
- The audited draw path reuses the selected mesh LOD across render passes. Independent main-view, shadow-view and reflection/probe LOD policies must be designed and tested rather than inferred from the current main-view calculation.
- LOD levels share the mesh vertex/skin streams and are resident together in the current GPU mesh representation. This allows a resident coarser mesh to move back toward finer indices without waiting for a geometry-streaming request, but it is not a complete geometry-residency solution.
- The current transitions are discrete; do not assume cross-fade or hysteresis exists just because the selection is recalculated.

### 1.3 Current texture foundation and limits

- The renderer has mip-chain construction/upload and mip-aware sampler configuration. Linear sampling can use anisotropy when the device advertises support; the current code caps that sampler setting at 8x.
- Texture compression variants include desktop BC formats and mobile ASTC formats.
- GPU sampling derivatives select an appropriate mip for the texture footprint of the current draw. This is distinct from an asset streamer deciding which mip levels are resident.
- A complete, view-prioritized texture-mip streaming/residency policy has not been established by this audit. Do not claim that the engine streams sharper mips into memory on demand. Verify the mip policy of each importer/cooked asset path before relying on it.

### 1.4 Measurement limits

- The current profiler records CPU scopes and GPU wait/resource counters, but backend GPU execution timestamps are still pending in the roadmap. GPU wait time is not GPU execution time.
- Android APK build support exists, but no physical Android phone renderer/performance run is recorded in PLATFORM_SUPPORT.md. There is no valid phone FPS, thermal or battery claim yet.

## 2. Zoom must be treated as a projection change

A user may change the apparent size of an object without physically moving the camera. That is why world distance alone cannot drive detail selection.

Support and validate these distinct cases:

1. **FOV zoom:** the camera projection narrows or widens. Recompute projected error from the active projection every frame or whenever the view changes.
2. **Dolly / physical camera movement:** the camera actually moves closer or farther. Recompute from the new view transform and bounds.
3. **Render-target crop or optical magnification:** a scene is rendered into a sub-rectangle or separate target and then enlarged. Use the source viewport dimensions, crop, projection and final presentation scale correctly. Do not count magnification twice or use the full display dimensions if the scene only occupies a smaller source target.
4. **Dynamic resolution and temporal upscaling:** distinguish internal raster resolution from final display resolution. Detail that is not represented by internal samples cannot be recovered merely by choosing LOD0. Define the error policy using both render-target dimensions and final output; test the actual temporal reconstruction path.
5. **Split-screen, picture-in-picture and secondary gameplay cameras:** each camera has its own projection, viewport dimensions and visible set. One camera's LOD decision must not be reused blindly for another.
6. **Editor viewports, thumbnails and preview captures:** use their real render target and camera state. A small thumbnail must not force full-resolution scene detail; a zoomed asset preview must not remain stuck on the thumbnail's coarse LOD.
7. **Reflection/probe captures and shadow views:** these are distinct projections. Select detail from the capture view or light-space texel footprint, not only from the player's main camera. Shadow quality needs its own bounded error/importance policy.
8. **Rapid camera cuts, zoom ramps and moving objects:** avoid stale view state, hysteresis oscillation, delayed quality recovery and visible level popping.

Use the active projection matrix and actual render target dimensions as the source of truth. If a shortcut uses FOV, aspect ratio or a focal-pixel scale, prove it is mathematically equivalent for the projection being rendered. Guard invalid/near-zero FOV, invalid dimensions, non-finite transforms, camera-inside-bounds cases and degenerate bounds. Handle non-uniform scale conservatively; the largest axis length is a safe but potentially over-conservative starting point, not a perfect screen-space error bound for every transform.

## 3. Coordinated fidelity systems

Each subsystem owns its own representation and data. Vectors coordinates geometry decisions; it must not grow a duplicate renderer or absorb the texture, animation, material or shadow systems.

### Geometry

- Use projected geometric error, silhouette importance and object bounds.
- Keep collision, navigation and authoritative gameplay representations independent of visual LOD.
- Preserve boundaries, hard edges, material sections, UV seams, vertex colours, alpha-tested coverage, skinning and any supported deformation data.
- Use manual/protected LODs or attribute-aware simplification for high-importance silhouettes and hero assets where automatic simplification is not perceptually safe.
- Re-evaluate per view; use hysteresis and optional fades only where their extra draw/overdraw cost is measured.

### Textures and materials

- Let mip filtering respond to the actual screen-space texture footprint of each rendered view.
- Keep mip generation, mip selection and mip residency as three separate concerns.
- When streaming is introduced, keep a coarse safe mip resident; prioritize sharper mips by projected texel demand, visibility, view importance, memory budget and predicted camera motion. Add request hysteresis, cancellation, deduplication, upload budgeting, fallback and cache accounting.
- Avoid mip thrashing during a quick zoom sequence. A zoomed view should request useful detail early enough to avoid a blurry hold, but speculative requests must not evict essential content.
- Validate normal-map mips, alpha-tested cutouts, UI/atlas textures, texture compression quality and anisotropic filtering. A correct geometric LOD cannot repair a blurry or incorrectly filtered material.

### Shadows, visibility and capture passes

- Main-view visibility and shadow-caster visibility are not interchangeable. An object outside the player's current view can still cast a visible shadow.
- Use per-light/per-cascade projected footprint and shadow-map resolution to select caster detail. Bound shadow work with the light's importance, coverage and quality tier.
- Evaluate reflection/probe and editor capture views independently. Avoid recursively multiplying view cost without a per-frame budget.
- Keep Hi-Z, frustum and occlusion decisions scoped to the view/depth source that generated them. Never reuse a main-view occlusion result for a different camera or light view.

### Characters and animation

- Character mesh LOD and animation-evaluation LOD are related but separate decisions.
- Preserve face/helmet/eye details, hands, silhouette, clothing and important attachments when they occupy meaningful screen area. Use measured perceptual importance, not an unconditional exemption from all LOD.
- Validate simplification over representative animation poses and deformation extremes. Do not claim morph-target preservation while the relevant import/cook/runtime path is unsupported.
- Do not change gameplay root motion, collision, hitboxes or authoritative state when visual LOD changes. Any reduced animation update frequency must use bounded interpolation and be separately validated for responsiveness and determinism.

### Foliage, alpha-tested and small geometry

- Test thin railings, wires, leaves, fence meshes, grass cards, decals and alpha-cutout materials. Geometric triangle reduction can change coverage and silhouettes even when average surface error is low.
- Preserve coverage using appropriate alpha-aware LOD, density representation or validated fade/transition methods. Do not solve disappearing foliage by blindly forcing every instance to LOD0.
- Large populations require batching/instancing and density budgets as well as LOD. A low triangle count with one draw per item can remain CPU-bound.

## 4. Per-view resource-budget policy

A scalable system must consider total work, not only whether each individual object passes a pixel threshold.

- Maintain per-view budgets for triangles/vertices, draw submissions, shadow work, resident geometry bytes, texture bytes, streaming uploads and optional effects.
- Account for all simultaneously active views in the frame budget. A second camera is not free; define priority and fallback policy if views compete.
- Rank work using projected contribution and semantic importance: primary playable view, central/high-contrast silhouettes, nearby characters, interactable objects, then low-contribution distant detail. Importance must be configurable and inspectable.
- Set a quality floor. The allocator must report when the requested view quality cannot fit the device budget rather than silently exceeding memory or degrading critical gameplay visuals.
- Smooth adaptation with hysteresis and bounded rates. Avoid visible quality pumping as frame time fluctuates.
- On mobile, use actual capability profiles and sustained thermal/power signals when available. Reduce optional work first and preserve stable frame pacing. Thermal adaptation is not permission to silently change simulation rules.
- Keep player overrides and a deterministic fixed-quality mode for captures, automated testing and debugging.

## 5. Required implementation sequence

1. **Audit and baseline:** verify the latest projection, render-target, pass, texture-mip, sampler, asset-cooking and profiler paths. Record exact source references, current unsupported cases and representative image/performance baselines.
2. **Projection correctness:** add unit tests for projected-error math across FOV, viewport sizes, aspect ratios, distance, scale and invalid inputs. Add GPU regression tests for wide/normal/narrow FOV and rapid zoom in/out.
3. **Per-view correctness:** introduce explicit view identity/state only where required by current architecture. Prove separate LOD and visibility for a main view plus a secondary view; then validate editor/probe/shadow passes. Avoid a generic framework until two real consumers require it.
4. **Transition quality:** add measured hysteresis and optional fade/dither. Test camera shake, animation, moving objects, rapid zoom and camera cuts.
5. **Texture integration:** verify mip completeness and minification sampling for every supported texture path. Instrument the currently resident mip/texture memory. Design streaming only after asset/IO/upload lifecycle and fallback behaviour are understood.
6. **Scene budgets:** add per-view and frame-wide budgets with priority, quality floors, user overrides and diagnostics. Do not use fabricated GPU utilization as an input.
7. **Device validation:** compare fixed-detail and adaptive runs on named physical desktop and Android hardware, including sustained thermal/soak sessions. Continue other slices while hardware is unavailable and mark this gate blocked.

## 6. Acceptance matrix

| Scenario | Required evidence |
|---|---|
| Same static scene at wide, normal and narrow FOV | Narrower FOV selects finer geometry where its projected error becomes visible; quality returns cleanly on zoom-out |
| Camera dolly toward and away from a wall | Detail follows actual projection and distance, without popping or stale bounds |
| Zoomed crop / reduced render target | Correct source pixel dimensions and final-upscale policy; no double-counted magnification |
| Dynamic resolution / temporal upscale | Stable detail under scale changes; recorded internal/output resolution and image-quality comparison |
| Two simultaneous cameras with different FOV/resolution | Independent view decisions and an explicit combined frame budget |
| Main camera plus shadow/probe capture | Appropriate per-view geometry and visibility; no missing visible shadows/reflections |
| Close-up character over multiple poses | Face/hands/silhouette/deformation preserved; no animation or gameplay state corruption |
| Thin/alpha-tested foliage and geometry | Coverage and silhouette remain stable at every LOD and during transitions |
| Texture close-up after long-distance view | Correct mip footprint; no stale coarse mip; streaming claims only if residency/upload is implemented |
| Memory pressure and fast repeated zoom | Bounded requests/uploads, safe coarse fallback, no unbounded allocation or mip thrashing |
| Low/mid/high physical mobile devices | CPU/GPU frame times where measurable, frame pacing, memory, temperature and power/thermal behaviour over time |

Use repeatable scenes and captures. Record the exact build, device, OS/driver, GPU, viewport, FOV, render scale, quality settings, cache state and test sequence. Compare triangle and draw counts, CPU time, actual GPU time when supported by real timestamps or platform tools, resident memory, image quality and sustained thermals. If GPU execution time cannot be measured, say so; GPU wait time is not a substitute.

No arbitrary universal image-difference threshold should be invented. Establish tolerances per asset class and pass, review silhouette/coverage and temporal stability, and retain golden captures. A lower triangle count is not a win if it creates visible quality loss or moves the bottleneck elsewhere.

## 7. Creator-facing controls and diagnostics

The Studio handoff owns the UI. At minimum, creators and engine developers need:

- Current view name, target dimensions, FOV/projection and render scale.
- Selected LOD, triangle count, source geometric error and projected pixel error.
- Per-view LOD and visibility overlays, plus a forced-LOD/debug mode.
- Current texture mip/format/residency information only when the renderer can report it accurately.
- Geometry, texture, shadow and total view-budget usage with explicit unavailable/unknown states.
- A reproducible camera/zoom sweep for assets and scenes.
- Clear warnings when an asset cannot provide a finer representation or when a requested quality floor exceeds budget.

The UI must not imply a feature exists before the runtime exposes correct data. Every control must change the real renderer, or be clearly labelled as a read-only diagnostic.

## 8. References

- Unity 6 LOD Group, screen-relative transitions and fade controls: https://docs.unity.com/en-us/engine/6000.6/manual/analysis/graphics-performance-profiling/lod/lod-group/class-lodgroup
- Unreal Engine texture streaming configuration and screen-size-based wanted resolution: https://dev.epicgames.com/documentation/unreal-engine/texture-streaming-configuration-in-unreal-engine
- Khronos glTF 2.0 sampler filtering and mipmap modes: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- Android game power and thermal guidance: https://developer.android.com/games/optimize/power
- Android Dynamic Performance Framework: https://developer.android.com/games/optimize/adpf

Use these as evidence and design references, not instructions to reproduce another engine's architecture or claim feature parity.

## 9. Agent instructions

- Read AGENTS.md and all linked VICTUS handoffs before editing.
- Preserve the working projected-error LOD baseline; extend it rather than creating another LOD subsystem.
- Re-audit every current-main assumption before code changes. Roadmap/status documentation may have moved since this note was written.
- Implement in small, buildable slices. Add regression tests and a real sample consumer with each slice.
- Do not claim texture streaming, simultaneous per-view LOD, cross-fades, GPU timing, mobile performance or thermal adaptation until the implementation and validation actually exist.
- Keep visual LOD separate from collision, navigation, gameplay and authoritative simulation.
- Commit directly to main. Update ROADMAP.md and docs/STATUS.md only with verified results, exact commands, hardware and evidence.
