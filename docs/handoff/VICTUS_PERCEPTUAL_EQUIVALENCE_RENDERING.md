# VICTUS Perceptual-Equivalence Rendering
## Adaptive fidelity without visible compromise

**Owner-requested engineering handoff · 2026-11-19**

**Status:** Approved product direction and implementation brief. This describes the target and work programme; it does not claim the target is implemented.

**Coordinates with:** ROADMAP.md priority 6e A.E.G.I.S.; docs/handoff/VICTUS_AEGIS_ADAPTIVE_GEOMETRY.md; docs/handoff/VICTUS_CAMERA_RESPONSIVE_VISUAL_FIDELITY.md; V.A.U.L.T. (6g), world streaming (6h), animation (6i), the renderer/RHI, materials, VFX and mobile platform work.

## 1. The owner’s intended result

VICTUS must let a developer create a visually ambitious game and deliver substantially the **same perceived image quality** across capable, supported devices, even when those devices cannot afford the same rendering workload as a high-end desktop.

The engine should take different computational routes to reach the same visible result:

- A high-end PC may compute detailed geometry, dynamic lighting, reflections, shadows and effects directly.
- A lower-cost target may use precomputed data, cached results, cheaper representations, temporal reconstruction, selective updates, better culling, compression, streaming or approximations.
- Those substitutions must preserve what a player actually perceives: silhouettes, composition, lighting relationships, material response, sharpness, motion stability, important effects and environmental detail.
- During ordinary play, the player should not be able to identify the cheaper path through obvious low-poly geometry, muddy textures, flat lighting, missing shadows, flicker, popping, ghosting, reduced effect density or unstable frame pacing.

**The target is perceptual equivalence, not identical internal computation, identical algorithms or bit-for-bit identical pixels.** Different displays, resolutions, colour pipelines and hardware will produce unavoidable differences. The engineering task is to keep those differences within explicit, tested visual tolerances at final output resolution and intended viewing conditions.

This is not a promise that every feature can run at maximum fidelity on every device. VICTUS must define a sensible minimum supported hardware floor. Very old or severely constrained devices may be unsupported. On hardware within the supported floor, however, the engine must spend its budget intelligently instead of automatically making the game look visibly cheaper.

### Product rule

**Reduce the cost of producing the image before reducing the quality of the image.**

Do not interpret this as “add more Low/Medium/High presets.” Presets are capability envelopes. The core capability is content-aware, view-aware and hardware-aware selection of alternate rendering methods that preserve the same visual intent.

## 2. This is a whole-pipeline objective, not another renderer

This handoff coordinates existing and planned VICTUS systems; it does not create a competing renderer, geometry LOD implementation, asset manager or simulation scheduler.

| Concern | Existing/planned owner | Role in perceptual fidelity |
|---|---|---|
| Geometry and visible detail | A.E.G.I.S. + mesh/terrain LOD | Spend geometry and memory where they affect visible shape and detail |
| Camera and per-view selection | Camera-responsive fidelity handoff | Use actual projection, crop, FOV, target size and view purpose |
| Asset loading and residency | V.A.U.L.T. | Keep the right geometry, textures and mips ready without stalls |
| World/cell lifetime | World streaming | Load detail before it becomes visible; preserve safe fallbacks |
| Materials and shading | Renderer/material graph | Preserve material response with validated cheaper shader paths |
| Lighting and shadows | Renderer + future lighting work | Bake/cache stable lighting; spend dynamic work where change is visible |
| Reflections and indirect light | Renderer + future GI/reflection work | Combine screen data, probes, cached results and more expensive tracing where supported |
| Reconstruction and anti-aliasing | Renderer/post-processing | Recover a crisp, stable final image from a cheaper internal render |
| VFX and particles | Particle/VFX system | Preserve effect silhouette, energy, density impression and timing |
| Animation and background simulation | Animation + A.E.G.I.S. policy | Reduce update cost only where a cheaper representation remains convincing |
| Authoritative simulation | Gameplay/physics/network systems | Preserve game rules; rendering shortcuts must not change authoritative state |
| Device adaptation | Platform/RHI + shared budget policy | Respect measured CPU, GPU, memory, bandwidth, power and thermal constraints |
| Proof and authoring | Profiler, captures, Studio and tests | Explain decisions and compare output against the high-end reference |

Ownership boundaries are mandatory. A.E.G.I.S. must not absorb texture residency, world-cell loading or authoritative simulation. V.A.U.L.T. owns asset lifetime and residency. The camera handoff owns projection correctness. This brief defines the cross-system visual objective and how those systems coordinate.

## 3. Architecture: a high-quality reference and validated alternate paths

### 3.1 Reference output

For each representative scene and target display condition, produce a high-quality reference render on capable hardware using the best validated VICTUS path available. Record the exact scene, assets, camera, lighting, animation state, simulation seed, output resolution, colour settings and frame sequence.

That reference is the visual target—not a marketing screenshot and not a hand-authored approximation. It is not required to be the runtime path on every device.

### 3.2 Fidelity policy, not device-name checks

A VICTUS-owned policy should consume:

- Hardware/backend capabilities and verified feature support.
- CPU/GPU frame-time measurements where genuinely available.
- Memory/bandwidth pressure, asset residency and upload queues.
- Sustained thermal/power state and platform performance signals.
- Active camera projection, output dimensions, render scale and view purpose.
- Scene content, screen coverage, silhouette importance, material/light complexity and effect significance.
- Available precomputed, cached, streamed and alternate representations.

Do not key the rendering design to marketing device names. Discover capabilities through VICTUS RHI/platform interfaces, then validate profiles on named physical hardware.

The policy must distinguish GPU bottlenecks from CPU submission, memory/bandwidth, shader/overdraw and thermal bottlenecks. Lowering geometry when texture bandwidth or fragment shading dominates is wasted quality. Never use fabricated GPU utilisation or GPU wait time as a substitute for GPU execution timing.

### 3.3 Fidelity floors

Each content class and feature needs a minimum acceptable representation. Prefer invisible or near-invisible savings first. A target must not silently cross a known visible-quality floor just to hit a frame-rate number.

When a device cannot meet both the project's visual floor and performance target, expose the conflict in diagnostics and allow a deliberate policy: prefer fidelity, prefer frame rate, or declare the device below minimum requirements. Do not pretend the conflict has disappeared.

## 4. Where to remove rendering cost

The agent must audit current code and select techniques based on measured bottlenecks. This is a research menu, not a requirement to implement every technique or reproduce another engine's proprietary design.

### 4.1 Geometry: keep the image's shape, not every source triangle

- Extend the existing meshoptimizer and screen-space-error LOD path. Preserve silhouettes, thin structures, hard edges, UV/material boundaries, alpha-cutout coverage, vertex colour, skinning and morph deformation.
- Use stable transitions, hysteresis and per-view choices. Main camera, shadow cascade, reflection capture and editor view may need different representations.
- Investigate cluster/HLOD representations, compact per-LOD streams, geometry compression, instanced representations and distant impostors where the final image remains convincing.
- Cull invisible work early. Avoid submitting sub-pixel geometry that contributes no stable visible detail, while preserving coverage and temporal energy for wires, foliage, fences and other thin elements.
- Do not replace close-up silhouettes with flat impostors or use simplification that becomes obvious during camera motion.
- Preserve LOD 0 or a separately validated collision representation for gameplay, collision, navigation and authoritative queries.

### 4.2 Lighting: preserve the light, not necessarily the expensive calculation

Treat lighting components according to how quickly they change:

- Bake or precompute static scene lighting where content and workflows permit.
- Represent slowly changing indirect illumination with probes, irradiance volumes, cached radiance or other validated approximations.
- Reserve dynamic direct lighting and contact effects for changes and surfaces where movement or player proximity makes them perceptible.
- Reuse stable shadow information and update only affected regions/cascades when correctness permits. Shadow visibility is not the same as main-camera visibility.
- Combine screen-space information with a robust world-space fallback so off-screen objects do not lose all indirect light or reflections.
- Select hardware tracing, software tracing, screen-space methods, probes, baked data or other approaches according to capabilities and scene needs. Evaluate the fallback image; a feature checkbox is not proof of equivalent appearance.
- Preserve colour, contrast, contact, bounce-light direction and shadow softness. Flat ambient light is not an acceptable substitute for realistic illumination merely because it is cheap.

The long-term target includes convincing indirect lighting and global illumination. Current rendering status documents PBR, direct/clustered lighting, shadows, SSAO, IBL, physical sky and reflection probes, but do not establish a complete dynamic GI solution. Treat this as a real gap.

### 4.3 Reflections and surface response

- Use screen-space reflection information when valid, then fill missing information with probes, environment data, cached results or a more expensive supported path.
- Prioritise accurate reflections on visible, smooth, reflective surfaces and close range; use cheaper representations where roughness and screen coverage hide differences.
- Preserve roughness response, specular highlights, reflection direction and temporal stability. Avoid obvious missing objects, leaks, streaks and disocclusion artifacts.
- Cache static or slowly changing captures and invalidate them only when relevant content changes.

### 4.4 Materials and textures: maintain crispness at final output

- Cook material variants for actual backend capabilities, with stable visual contracts and tested fallbacks.
- Bake expensive static material graph work into texture data when it does not need to change at runtime.
- Use platform-appropriate texture compression and preserve normal-map, roughness, alpha and colour-space correctness.
- Add view-prioritised mip residency through V.A.U.L.T.; retain a safe coarse mip and request sharper data before it is needed.
- Use anisotropic sampling and mip bias carefully with dynamic resolution and temporal reconstruction. Lower internal resolution can require sharper source mips; otherwise an upscaler cannot reconstruct detail that was never sampled.
- Avoid visible blur, texture pop-in, moiré, specular aliasing and inconsistent materials between paths.

### 4.5 Reconstruction: use time intelligently

- Extend the existing TAA and temporal-upscaling foundation toward a high-quality temporal reconstruction path.
- Investigate spatial/temporal upscaling options portable across the VICTUS RHI; do not make a vendor-specific accelerator mandatory.
- Use motion vectors, depth, exposure, history rejection, disocclusion handling, reactive masks and detail-preservation logic as required by the chosen method.
- Evaluate output sharpness, fine geometry, foliage, particles, thin lines, animated textures, camera cuts, rapid motion and newly revealed areas—not just still screenshots.
- Avoid over-sharpening, ghost trails, flicker, smearing and temporal instability. Never claim “native-resolution quality” without representative motion tests.

Dynamic resolution is one lever, not the whole solution. Combine it with correct mip bias, reconstruction quality and per-effect policies.

### 4.6 VFX and particles: preserve the impression

- Use GPU simulation, instancing, batching, culling and bounded spawn/update budgets where they improve measured performance.
- Build effect-specific representations: detailed particles near the camera; cheaper instanced or volumetric forms for dense distant effects; aggregates for large low-salience clouds where they preserve screen-space density and motion.
- Preserve silhouette, brightness/energy, colour distribution, timing, direction, screen coverage and interaction cues.
- Reduce invisible secondary work before reducing prominent sparks, smoke structure, impact flashes, trails or other defining details.
- A particle system that is cheap but flickers, pops or visibly thins out is not a successful optimisation.

### 4.7 Animation and simulation: never corrupt game truth

- Separate authoritative fixed-step gameplay/physics from visual presentation and non-authoritative background simulation.
- Allow validated lower-frequency visual updates with interpolation, pose reuse, simplified distant rigs and animation LOD where they remain smooth.
- Keep player-critical characters responsive. Preserve silhouette, face/hand details, important attachments, contact, root motion and animation timing.
- Use aggregation or event-driven updates for distant/background populations only when gameplay invariants are preserved.
- Never use render visibility as a reason to stop authoritative timers, combat/gameplay state, server simulation, persistence or important off-screen events.
- Keep visual LOD, simulation LOD and network relevancy LOD separate, as required by A.E.G.I.S.

### 4.8 CPU, memory, bandwidth and thermals

The goal is not GPU-only optimisation. Use batching, instancing, job scheduling, data locality, upload budgets, streaming, cache reuse, memory accounting and bounded work queues. Adapt slowly enough to avoid oscillation. Thermal adaptation needs real platform signals and sustained tests; a short cool-device benchmark does not prove stable long-session performance.

## 5. Adaptation order: hide the cost before reducing visible quality

Prefer these savings, subject to actual bottleneck evidence:

1. **Eliminate work that cannot affect output:** frustum/occlusion culling, hidden surfaces, redundant updates and duplicate work.
2. **Remove imperceptible detail:** sub-pixel geometry, excessive updates, unnecessary overdraw and detail below final-output sampling limits, while preserving silhouettes and temporal coverage.
3. **Reuse or precompute:** baked static lighting, cached shadows/reflections, cooked material data, cached animation/pose work and reusable visibility results where invalidation is correct.
4. **Choose another representation:** mesh LOD/HLOD, impostors, cheaper material variants, probes, screen-space plus world-space fallback, particle aggregates and simplified non-authoritative simulation.
5. **Reconstruct intelligently:** render fewer internal pixels and recover a crisp final image with temporal/spatial methods and correctly biased texture sampling.
6. **Reduce clearly visible quality only as a last resort:** expose the trade-off, maintain the project's minimum fidelity floor and do not hide an unsupported device behind misleading “Ultra” or “High” labels.

This is a policy, not a rigid sequence. If measurements show a different cost dominates, address that cost.

## 6. Stable and explainable runtime adaptation

- Use one shared budget policy or coordinator with explicit subsystem contracts; do not let each subsystem independently react to spikes and create contradictory changes.
- Keep per-view visual budgets and global CPU/GPU/memory/streaming budgets distinct but coordinated.
- Use hysteresis, cooldowns, bounded adjustment rates and recovery thresholds so quality does not oscillate frame by frame.
- Distinguish cold-start, streaming, steady-state and thermal-soak conditions.
- Make decisions deterministic where required for tests and reproducible captures. Adaptation must not change authoritative simulation determinism.
- Preserve artist overrides and project quality floors. Automatic adaptation must be inspectable, not a black box.
- Report actual reasons, such as “shadow cache reused,” “distant cluster selected,” “texture mip pending,” or “render scale adjusted due to measured GPU budget.”
- Avoid per-frame CPU readbacks and synchronisation stalls just to make decisions. Prefer asynchronous measurements and platform signals.

## 7. Prove that players do not see the shortcuts

A cheaper path is accepted only with both performance evidence and image-quality evidence.

### 7.1 Deterministic reference scenes

Build scenes that exercise the whole renderer together:

1. **Photorealistic exterior:** foliage, long sight lines, terrain, buildings, sun/sky, indirect light, contact shadows, weather/fog and reflective/wet surfaces.
2. **Complex interior:** mixed materials, local lights, indirect illumination, shadowing, glass/reflective surfaces, small props and occlusion.
3. **Character close-up and motion:** detailed surface response, animation, skin deformation, face/hands/attachments, changing light and camera movement.
4. **Effects stress scene:** layered GPU particles, trails, smoke, sparks, debris, transparency and dynamic lighting.
5. **Streaming/large-world scene:** fast camera movement, geometry/texture streaming, memory pressure, world-cell transitions and camera cuts.
6. **Combined game workload:** gameplay, physics, animation, VFX, audio, UI and rendering active together.

Each scene needs fixed seeds and reproducible camera, animation and simulation paths.

### 7.2 Compare final images and sequences

Compare each target path to the high-end reference after both are produced at the intended final output resolution and colour pipeline. Record:

- Perceptual image metrics (for example SSIM or LPIPS where suitable), plus pixel/error captures for diagnosis.
- Human-reviewed A/B comparisons for lighting, material response, silhouette, detail, temporal stability and obvious artifacts.
- Frame sequences, not only stills: motion, disocclusion, camera pans, zoom, particles, shadows and lighting changes.
- Different display resolutions, aspect ratios, render scales and representative viewing sizes.
- Asset-class-specific tolerances. Do not invent one universal threshold or let a good average metric hide a broken face, silhouette, thin wire, shadow or effect.

Store exact build, scene/asset revision, camera, seed, output settings, device/backend/driver, rendering path, frame sequence, raw timings, memory counters and comparison results.

### 7.3 Physical-device proof

Test Release builds on named physical low-, mid- and high-capability supported devices, including sustained sessions after thermal conditions stabilise. Record frame-time percentiles/pacing, memory, bandwidth where measurable, thermal status, power/battery where available, resolution, driver, OS, build and settings.

Emulators and software rasterisers are useful for correctness tests but cannot establish real GPU throughput, sustained thermals or battery behaviour. Android is not validated merely because an APK builds.

### 7.4 Acceptance rule

For a supported device class, the adaptive path must:

- Meet the declared sustained frame-time, memory and thermal budget.
- Stay within approved perceptual tolerance for the relevant content class and motion sequence.
- Avoid obvious artifacts, instability, visible quality cliffs and missing effects.
- Recover detail when the camera approaches, zooms, changes view or reveals streamed content.
- Preserve gameplay, simulation and networking invariants.
- Reduce the measured bottleneck rather than moving the same cost elsewhere.

If either performance or visual acceptance fails, the path is not complete. Higher FPS alone is insufficient.

## 8. Current-main baseline and known gaps to audit

At the main snapshot used to write this handoff (8d95a67a78cd6606eaab05d66554ecd3b75e04be, 2026-11-19), project documentation describes a meaningful foundation: forward HDR PBR, clustered direct lighting, cascaded/local shadows, SSAO, IBL, physical sky, reflection probes, TAA/temporal upscaling, dynamic resolution, mesh LOD, Hi-Z occlusion, instancing and material/shader tooling. Existing handoffs cover A.E.G.I.S. and camera-responsive per-view quality.

This is not the complete target. Current documentation also records important gaps or unproven areas:

- A complete dynamic global-illumination solution is not established in rendering status.
- Complete asynchronous texture-mip and geometry residency/streaming are not established; V.A.U.L.T. is the planned owner for asset residency.
- GPU execution timestamps are not currently available through the documented RHI; GPU wait time must not be mislabelled as GPU execution time.
- Advanced ray tracing and virtualized geometry are research directions, not established cross-platform features.
- Physical Android rendering, FPS, memory, sustained thermal behaviour and battery behaviour have not been validated.
- Current particle, water, material and temporal paths need a fresh source audit; do not copy stale maturity claims from older status tables.

The coding agent must re-fetch main, inspect actual renderer/RHI/assets/particles/animation/runtime code and relevant ADRs before implementation. This section is a starting audit, not permission to assume the code has not changed.

## 9. Implementation sequence

Implement coordinated vertical slices, not a giant renderer rewrite.

### Slice 0 — Source audit and perceptual baseline

Re-audit current main, including A.E.G.I.S., camera, RHI, render graph, asset, lighting, shadow, reflection, temporal, VFX and platform code. Map every existing optimisation and its owner; identify overlapping policies. Build deterministic exterior and interior reference scenes. Capture a high-end reference on capable physical GPU hardware when available; separately record software-renderer correctness images. Establish CPU, GPU (only if genuinely measurable), memory, frame pacing and visual baselines.

### Slice 1 — Rendering-path and cost diagnostics

Instrument subsystem/per-view cost with honest unavailable states. Add reason codes and output-quality metadata for existing LOD, culling, render scale, mips, shadows and effects. Separate CPU submission, GPU execution, GPU wait, bandwidth/memory and streaming costs where the platform exposes them. Add a reproducible capture/compare harness. Do not set device targets before hardware baselines exist.

### Slice 2 — Highest-value invisible savings

Choose measured bottlenecks. Likely candidates include better culling/instancing, stable per-view LOD and transitions, geometry/texture residency, cache reuse, material shader specialisation and baked static data. Extend owning systems; do not add a parallel global quality manager.

### Slice 3 — Lighting/reflection fidelity paths

Research and compare precomputed lighting, probes/irradiance representations, cached lighting/shadows/reflections and supported dynamic methods. Define invalidation for static, slowly changing and dynamic content. Select the cheapest method that stays inside image/sequence tolerance for each scene class.

### Slice 4 — Temporal reconstruction and crisp output

Extend temporal reconstruction only after baseline captures exist. Validate mip bias, detail recovery, motion vectors, reactive/disocclusion handling and final sharpness. Test moving foliage, wires, particles, animated materials and rapid camera motion.

### Slice 5 — VFX, animation and simulation representations

Add effect-specific representation tiers and non-authoritative update policies. Preserve visible energy, density, silhouette and timing. Prove authoritative gameplay, physics, network and persistence state remain unchanged.

### Slice 6 — Unified stable adaptation

Connect device capability, real bottleneck measurements, per-view significance, asset residency and subsystem budgets through explicit contracts. Add hysteresis, quality floors, manual overrides, thermal response and diagnostics. Do not base this on unmeasured GPU utilisation.

### Slice 7 — Device matrix and production proof

Run the same reference scenes on named supported desktop and mobile hardware. Complete sustained thermal/soak testing, blind A/B review, image-sequence comparisons and reproducible performance reports. Only then promote the target from a design goal to a verified capability.

The exact order within slices may change after measurement; dependencies and acceptance rules may not.

## 10. Studio and creator experience

Creators and engine developers need to understand why the engine chose a cheaper method and whether it preserved the reference image. Provide diagnostics for:

- Reference path versus selected runtime path.
- Active backend/device capability profile and actual measured constraint.
- Final output dimensions, internal render dimensions and reconstruction method.
- Geometry/texture/mip residency, per-view LOD and culling decisions.
- Lighting, shadow, reflection and cache source/age/invalidations.
- Material variant and VFX representation selected.
- Per-view/global budget, quality floor and reason for each adaptation.
- Comparison captures and temporal difference heatmaps.

Creators must be able to set a minimum visual quality, force a path for debugging, compare reference/adaptive output and override automatic choices. Do not add controls that are not connected to a real runtime feature.

## 11. Agent operating instructions

1. Read AGENTS.md, this handoff, ROADMAP.md, docs/RENDERING_SPEC.md, docs/PERFORMANCE_STRATEGY.md, docs/KNOWN_LIMITATIONS.md, docs/PLATFORM_SUPPORT.md and the linked A.E.G.I.S./camera handoffs.
2. Re-fetch main and verify every source/status assumption before editing. Current source beats this document if they differ.
3. Trace ownership and reuse existing renderer, render graph, RHI, mesh/texture/asset formats, A.E.G.I.S., V.A.U.L.T., camera and profiler systems. Do not create a second renderer, LOD model, asset manager or conflicting adaptive-quality controller.
4. Research at least two credible approaches for high-risk foundations. Record versions, platform limits, licences and why the selected approach fits VICTUS.
5. Add an ADR before irreversible rendering/RHI/asset-format decisions.
6. Implement small end-to-end slices with tests, diagnostics, a real sample consumer and source-linked status updates.
7. Do not claim perceptual equivalence, native-looking output, GI, texture streaming, GPU timing, physical mobile support or thermal stability without the stated evidence.
8. Keep authoritative game state independent from rendering shortcuts.
9. Work directly on main under AGENTS.md instructions; inspect concurrent changes before every write, serialize commits and never overwrite unseen work.
10. After each verified slice, update ROADMAP/STATUS with exact evidence and continue to the next unblocked task. If physical hardware is unavailable, record the exact blocked proof and continue work that can be tested.

## 12. Research references

Use these as examples of engineering approaches and validation questions, not instructions to reproduce another engine:

- Unreal Engine temporal upscalers: https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-upscalers-in-unreal-engine
- Unreal Engine Temporal Super Resolution: https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine
- Unreal Engine Lumen technical details (screen traces, software/hardware tracing and fallbacks): https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- Unreal Engine Lumen performance/scalability guide: https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine
- Unreal Engine Nanite overview (clustered/virtualized geometry and streaming principles): https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- Android game power and thermal guidance: https://developer.android.com/games/optimize/power
- Android Adaptive Performance / ADPF: https://developer.android.com/games/optimize/adpf

A reference implementation's advertised result is not proof that its approach will work on VICTUS's current RHI, asset pipeline or target devices. Prototype and measure in VICTUS.
