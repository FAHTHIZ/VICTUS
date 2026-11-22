# VICTUS Visual Quality and Performance Gap Register

**Baseline:** 2026-11-22, `main` at `203dbfd071457ab60752b8c3fd6f0c3c89058060` (ADR-0203 / ADR-0204).  
**Purpose:** one practical register of what VICTUS still needs to make visually excellent games while keeping performance predictable. This is a work plan, not a claim of feature parity with another engine.

## How to read this register

A game engine does not become visually capable merely by accumulating rendering features. The final image depends on four connected things:

1. **Rendering:** lighting, materials, geometry, camera, post-processing, effects and correct image composition.
2. **Content:** high-quality models, textures, animation, sound, environments and consistent art direction.
3. **Authoring:** tools that let creators build, preview, tune, profile and iterate on those assets and scenes.
4. **Performance:** measured frame time, memory, bandwidth, streaming, thermal behaviour and stable quality settings on the actual target devices.

The engine can provide the systems and workflows; it cannot automatically supply good art direction or make weak assets look premium. A procedural city is a layout/content generator, not a substitute for authored architecture, materials, lighting and set dressing.

### Status language

- **In place:** implementation and automated evidence are recorded in `docs/STATUS.md`.
- **Partial:** a real implementation exists, but one or more important paths, tools or behaviours are missing.
- **Unproven:** automated or software-renderer tests exist, but the required physical hardware, workload or sustained measurement does not.
- **Optional:** useful for selected games or hardware tiers, not a prerequisite for a good game.

Do not change an item to complete because a class, shader or Inspector field exists. Its acceptance criteria must be met for the intended target.

## 1. What VICTUS already has

The following are real foundations in the current codebase. They are not all equally mature and their presence does not prove visual quality or physical-device performance.

| Area | Current foundation |
|---|---|
| Core 3D renderer | HDR forward+ clustered PBR, cascaded sun shadows, local light shadow atlas, SSAO, image-based lighting, reflection probes, irradiance volumes, decals, fog, sky and quality tiers |
| Image quality | TAA, temporal upscaling, FXAA/MSAA paths, bloom, auto exposure, tone mapping, grading/vignette controls, lens distortion, chromatic aberration and film grain |
| Lighting | Baked lightmaps, directional lightmaps, shadow masks, baked probes, rectangle area lights, point-light radius, per-light baked mode, spot/point/directional cookies |
| Reflections and water | Water SSR; opaque glossy-surface SSR using TAA history on supported quality paths; environment/probe fallback; Gerstner water |
| Materials | PBR material assets and instances, normal/emissive/ORM maps, material graphs, custom surface shaders, vertex functions, world-space triplanar mapping and clear coat |
| Geometry | Automatic instancing, mesh LODs, hysteresis/dithered LOD transitions, Hi-Z occlusion culling, retained instance groups, chunked heightfield terrain |
| Foliage and effects | Instanced foliage groups, CPU and GPU particle emitters, trails, sub-emitters, volumetric fog/clouds, procedural weather and decals |
| Characters and crowds | GPU skinning, animation state machines, IK/retargeting foundations, Orion/Nova Frame characters, and baked-palette `CrowdAgent` rendering |
| Content pipeline | glTF/GLB, FBX and OBJ import paths, cooked assets, BC/ASTC texture variants, asynchronous runtime model/texture loading and partial mip streaming |
| Procedural content | Seeded procedural city generation with roads, blocks, lots, basic buildings, parks, props, traffic agents and sidewalk crowd agents |
| Validation | Automated GPU tests, reference scenes, perceptual image comparison, render-path reports, counters and performance-budget tests |

### What the current evidence does and does not prove

- `docs/PLATFORM_SUPPORT.md` records Mesa lavapipe software Vulkan and Mesa GLES testing, plus a virtualised Apple GPU in CI. It records **no physical GPU validation**.
- The Android player can be packaged by CI, but the platform matrix records no install or performance run on a physical Android device. Frame rate, memory, thermals, battery use and input feel therefore remain unmeasured on phones.
- The 1,000-agent crowd timing is for a **two-joint test rig**. It is not evidence for 1,000 Orion/Nova characters or an entire populated city.
- The procedural-city CPU timing was recorded on a two-core sandbox. It does not establish GPU frame time, physics cost or sustained performance at city scale.
- Clear-coat testing proves the tested material path and highlight response. It does not establish the look of a complete scene under varied real-world lighting.
- A passing offscreen test proves the tested code path and assertions. It does not prove that a game is visually polished, content-complete or performant on every listed platform.

The exact commands and per-slice evidence belong in `docs/STATUS.md`; platform execution belongs in `docs/PLATFORM_SUPPORT.md`.

## 2. Priority zero — establish a measurable target and prove the current renderer

These are prerequisites. More graphics features should not be prioritised above them.

### VQ-001 — Define device classes and quality budgets
**State: incomplete; target decision and physical measurements required.**

Define supported performance profiles rather than promising one maximum-quality setting everywhere:

- Entry-level Android: select a representative minimum CPU, GPU, RAM, graphics API and storage class. Decide whether the sustained target is 30 FPS or another explicit target.
- Mainstream Android: define a separate 30/60 FPS and quality expectation based on measured devices.
- Desktop: define a standard 60 FPS profile and a higher-quality profile with a separate GPU/memory budget.
- Compatibility: document which effects are disabled or replaced when a device lacks the required capability.

For each profile, set CPU frame time, GPU frame time, memory, texture residency, draw/triangle/overdraw, loading hitch and thermal limits. Treat 16.67 ms (60 FPS) and 33.33 ms (30 FPS) as frame-budget arithmetic, not promises that current hardware meets them.

**Accept when:** the device matrix and budgets are written down, each budget has a measurement method, and the selected reference devices have repeatable captures.

### VQ-002 — Physical hardware validation
**State: unproven.**

Run the same reference projects on real devices, not only virtual or software renderers. Start with the lowest supported Android device and a representative newer Android device. Add physical Windows GPU and Apple Silicon testing before making desktop GPU claims.

Capture GPU vendor/model, driver, API, OS, display resolution, quality profile, CPU/GPU frame times, memory, temperatures, battery/power behaviour and image output. Repeat after a sustained play session, not only a short boot.

**Accept when:** the platform matrix contains reproducible device results for the actual supported platforms and the visual baselines are captured on those devices.

### VQ-003 — GPU timestamps and per-pass profiling
**State: missing at the RHI level.**

The current SDL_GPU path exposes CPU timings but not the GPU timestamp breakdown needed to distinguish CPU-bound from GPU-bound frames. Add a backend-aware timestamp-query abstraction where supported, with a clearly labelled fallback when a backend cannot provide it.

The profiler should show:
- Total CPU and GPU frame time.
- Render-graph pass timings and pass dependencies.
- Shadow, depth, lighting, reflection, fog, transparency, post and UI costs.
- Draw calls, batches, triangles, visible instances, shader/material switches and overdraw.
- Texture/buffer/render-target residency and upload/eviction activity.
- Simulation, animation, physics, navigation, particles and audio timings.

**Accept when:** a creator can identify the top three frame-time costs in a real scene on a physical GPU and export the report with the device and build metadata.

### VQ-004 — A visual and performance reference-scene suite
**State: partial.**

Keep small deterministic scenes that isolate a problem, but add complete production-like test scenes:

1. Outdoor daylight: terrain, foliage, architecture, shadows, sky and atmospheric depth.
2. Night city: emissive windows, street lights, wet roads, signs, vehicle lights and fog.
3. Interior: mixed materials, baked and dynamic lights, glass, indirect light and contact shadows.
4. Character close-up: armour, skin-like materials where supported, eyes/visors, facial animation, hair/cloth if used, and several poses.
5. Water: shore, opaque reflections, transparency, waves, foam and underwater view.
6. Effects stress: smoke, fire, particles, transparency, decals and multiple lights.
7. Crowd/city stress: buildings, foliage, vehicles, pedestrians, animation and streaming.
8. 2D stress: large tilemap, sprites, normal-mapped lights and particles once those paths exist.

Each scene needs fixed cameras, documented lighting, known assets, expected output, workload scale and test commands.

**Accept when:** every major rendering change is checked against relevant scenes and performance budgets, and images can be compared at the same resolution and settings.

### VQ-005 — Separate visual correctness from visual quality
**State: partial.**

Pixel tests and FLIP/SSIM comparisons are useful for regressions. Add image-review criteria for composition, material readability, lighting consistency, silhouette, texture scale, temporal stability, aliasing, ghosting and atmosphere. A numerically similar image can still look poorly art-directed.

**Accept when:** each showcase milestone has fixed-camera runtime captures, a short human review checklist and a linked hardware/performance report.

## 3. Priority one — make the renderer capable of high-quality scenes

### VQ-010 — Complete the material model
**State: partial.**

Current PBR and clear-coat foundations cover many common surfaces. Remaining material families depend on the art direction and assets:

- Sheen for fabric, velvet and micro-fibre surfaces.
- Anisotropy for brushed metal, hair and directional highlights.
- Subsurface scattering or a cheaper approximation for skin, wax, leaves and other translucent solids.
- Thin transmission/refraction for glass, lenses, liquids and translucent materials.
- Better cloth, leather, rubber, paint, ceramic, wet and layered-material workflows.
- Clear-coat texture inputs and consistent behaviour across all light types.
- Multi-UV support and predictable handling of tangent space, normal maps, AO and ORM packing.
- Material validation for missing maps, wrong colour spaces, invalid normal maps and extreme values.

Do not implement every BRDF layer at once. Add a layer when a target asset or reference scene demonstrates that the current model cannot represent it acceptably.

**Accept when:** each supported material family has an authored reference asset, predictable import/export, editor controls and tests under direct, indirect and grazing light.

### VQ-011 — Remove custom-shader pipeline inconsistencies
**State: partial.**

Project shaders already have surface and vertex functions, but the custom path is narrower than the built-in PBR path. The documented limits include parameter/texture-slot constraints and vertex functions that do not fully participate in every depth, motion-vector and culling path.

Required work:
- Give custom vertex deformation correct bounds and a consistent depth-prepass, shadow, velocity and motion-vector path.
- Expand parameter types and resource binding deliberately; avoid arbitrary limits that become a new compatibility problem.
- Add shader include/module reuse, reflection validation, useful compile diagnostics and hot-reload recovery.
- Cache compiled variants and pipeline state objects rather than recompiling or stalling during play.
- Ensure custom shaders participate correctly in TAA, SSAO, shadows, reflections and render-path diagnostics.
- Add a node-based shader authoring workflow only when its generated code can be debugged and versioned.

**Accept when:** the same custom material renders correctly in colour, depth, shadow, motion-vector and reflection-relevant paths, with cached compilation and actionable errors.

### VQ-012 — Improve indirect lighting and global illumination
**State: partial.**

VICTUS has baked lightmaps, directional lightmaps, shadow masks, baked probes, irradiance volumes and IBL. The major gap is flexible indirect-light response when objects, lights or time of day change.

Possible work, ordered by cost:
- Improve bake quality, validation, UV handling, denoising, incremental rebakes and artist feedback.
- Add a dependable probe-placement, blending, streaming and debug workflow.
- Support blended baked lighting states for authored day/night transitions where appropriate.
- Improve dynamic object integration with baked lighting and probe volumes.
- Evaluate a low-resolution probe-based dynamic GI option for higher tiers.
- Evaluate screen-space or hardware ray-traced GI only on platforms where measured performance justifies it.

A full Lumen-like system is not a prerequisite for attractive mobile games. Baked lighting, probes, strong direct lighting and carefully authored reflection sources may be the better solution for the target device.

**Accept when:** indoor/outdoor reference scenes show stable indirect light, no obvious light leaks, correct dynamic-object response and documented bake/runtime costs.

### VQ-013 — Shadows and local-light quality
**State: partial.**

The renderer has cascaded sun shadows, a local shadow atlas, soft-shadow options and sun contact shadows. Remaining work includes:
- Real-time area-light shadows or a documented approximation with clear quality tiers.
- Better local shadow filtering, bias controls, temporal stability and caster/receiver diagnostics.
- Contact shadows that behave consistently for local lights and blended objects where affordable.
- Shadow LOD, update scheduling, cached static shadows and per-light budgets.
- Improved foliage alpha-tested shadow quality without excessive overdraw.
- Debug views for cascade splits, atlas occupancy, shadow texel density and missing casters.

**Accept when:** indoor, outdoor, foliage and character tests have no distracting acne, peter-panning, severe shimmer or unbudgeted shadow cost on target devices.

### VQ-014 — Reflections, refraction and glossy surfaces
**State: partial.**

Water SSR and opaque glossy-surface SSR exist, but opaque SSR depends on TAA and quality/backend support and cannot see off-screen or hidden objects. Continue with:
- Improve SSR stability, disocclusion handling, roughness response and fallbacks.
- Evaluate planar reflections for selected water/mirror surfaces.
- Improve reflection probe capture, blending, prioritisation and streaming.
- Add refraction/transmission for glass and water where required.
- Keep environment/probe fallback for missing screen-space data.
- Provide a visual debug view for reflection source, hit/miss and fallback.

**Accept when:** the same reference scenes show stable reflections from close-up to distance, and their GPU cost is known for each tier.

### VQ-015 — Camera, tone mapping and post-processing
**State: partial.**

Current exposure, bloom, tone mapping, grading/vignette controls, TAA/upscaling and selected lens effects are a useful base. Evaluate:
- Depth of field with foreground/background controls and temporal stability.
- Motion blur with correct object/camera velocity and a mobile-off path.
- Colour-grading LUT workflow and colour-space validation.
- Consistent exposure units and camera settings for artists, while retaining simpler controls.
- Lens response, highlight roll-off and bloom threshold controls.
- Sharpening and upscaler controls that do not amplify aliasing.
- HDR display output only where the complete swapchain, colour management and platform path can support it.

Effects must be optional and quality-tiered. They must not hide poor lighting or low-quality assets.

**Accept when:** the cinematic reference scene can be art-directed repeatably and post effects remain stable in motion at measured frame budgets.

### VQ-016 — Transparency and compositing
**State: partial.**

Blended objects are currently order-dependent, and particles have their own sorting behaviour. Evaluate weighted blended order-independent transparency or another bounded solution for scenes that need overlapping glass, smoke, hair cards, particles and water.

Also improve:
- Refraction and thickness-aware glass.
- Soft intersections and depth fade.
- Correct sorting across particle systems and transparent meshes.
- Transparency-aware fog and temporal reactive masks.
- Overdraw visualisation and per-layer budgets.

**Accept when:** a controlled scene with intersecting transparent objects has predictable output and bounded cost on supported tiers.

### VQ-017 — Geometry LOD, HLOD and distant representation
**State: partial.**

Mesh LODs, instancing, Hi-Z occlusion and retained instance groups exist. A large city or dense natural environment still needs:
- HLOD clusters for buildings and static props.
- Tree/vegetation impostors or other far-field representations.
- Crowd impostors and animation LOD.
- Cook-time LOD generation and validation.
- Streaming of mesh LODs based on screen-space size and memory budget.
- Seam-free transitions, silhouette preservation and stable shadows.
- Per-cell merged meshes and material atlases where they reduce measured cost.

Virtualised geometry is an optional high-end research path, not a requirement for the entry-phone target.

**Accept when:** a large reference environment maintains stable silhouette and detail while reducing CPU submission, GPU work and memory at distance.

### VQ-018 — GPU-driven scene submission and culling
**State: partial.**

Retained instance groups and cluster-level foliage culling reduce CPU work, but the full world-static path is not yet a persistent GPU-driven pipeline. Continue with:
- Persistent instance buffers and dirty-range updates.
- GPU frustum and occlusion culling where supported.
- Indirect draw generation and compacted visible-instance lists.
- Per-instance or appropriately fine-grained LOD selection.
- A CPU fallback for GLES or devices without the relevant compute features.
- Correct handling of moving objects, shadows, multiple cameras and editor viewports.

**Accept when:** benchmark scenes demonstrate lower CPU submission cost at the same visible result, with no culling errors and measured GPU overhead.

### VQ-019 — Terrain, vegetation and biome quality
**State: partial.**

Heightfield terrain, chunks, sculpt/paint and foliage scattering exist. Important additions:
- Terrain holes/caves or a separate mesh-terrain path where the game requires them.
- Spline-based roads, rivers, paths, coastlines and terrain editing.
- Biome authoring with slope/height/paint masks and deterministic scatter.
- Vegetation wind, interaction and distance-aware animation.
- Per-instance culling/LOD, impostors and shadow-distance controls.
- Ground blending, decals, wetness and material transitions without obvious tiling.
- Terrain/foliage streaming and memory budgets.

**Accept when:** an environment artist can create a coherent landscape with natural distribution and maintain the target frame budget at close, medium and far distances.

### VQ-020 — Water and atmosphere
**State: partial.**

VICTUS has Gerstner water, water SSR, fog, clouds and weather, but these systems need stronger authoring and cross-system consistency:
- Better shoreline integration, depth colour, foam, spray, wakes and underwater effects.
- Optional higher-quality ocean simulation for platforms that can afford it.
- Water interaction with rain, objects, particles and lighting.
- Clouds and weather that integrate with sun shadows, reflection probes and changing lighting.
- Local weather volumes and authored density/masks.
- Better fog-volume authoring, local-light response and reflection/probe integration.
- Stable temporal behaviour without visible jitter or hard edges.

**Accept when:** a day/night, weather and water reference scene maintains consistent appearance across all supported tiers.

### VQ-021 — Character rendering and asset quality
**State: partial.**

Orion and Nova demonstrate the current stylised hard-surface direction, not realistic human rendering. Further work depends on the game's art direction:
- Authorable skin/eye/hair materials if realistic humans are a target.
- Hair cards or strand rendering with a quality/performance tier.
- Cloth shading, cloth simulation only where necessary, layered clothing and equipment.
- Corrective blend shapes/drivers and facial animation authoring.
- Better deformation around shoulders, elbows, wrists, hips and knees.
- Character LODs, material/texture budgets and close-up validation.
- A Studio character workbench for skeletons, sockets, clips, retargeting and pose inspection.

For the current Frame characters, first prove their materials and silhouettes in a finished runtime scene. Do not add realistic-human rendering features unless the art direction actually needs them.

**Accept when:** characters survive close-up lighting and the full animation library without visible intersections, broken deformation or unbudgeted cost.

### VQ-022 — Animation authoring and crowd fidelity
**State: partial.**

The runtime has state machines, blend spaces, skinning, IK, retargeting and baked crowd palettes. Remaining work includes:
- IK poles and per-joint limits with usable Studio gizmos.
- Sync markers, reliable foot locking/contact IK and better retarget pose correction.
- Root motion on configurable axes and controller integration.
- GPU morph-target evaluation and correct deformation motion vectors.
- Crowd animation LOD, clip blending/cross-fades, near-camera Animator hand-off and far impostor hand-off.
- Crowd spawning, navigation integration and stress tests with real character rigs.
- Animation compression and memory budgets for large clip libraries.

**Accept when:** the same character controller, animation set and crowd workload are measured and reviewed at close, mid and far distances on target hardware.

### VQ-023 — VFX and cinematic authoring
**State: partial.**

CPU/GPU particle simulation, trails and sub-emitters exist. Creator-facing gaps include:
- A VFX graph/editor with node authoring, reusable modules, curves, preview and profiling.
- Lit particles or well-defined lighting approximations.
- Mesh particles, flipbook blending and robust ribbon systems where needed.
- Better collision/event handling and deterministic replay.
- Per-emitter, per-effect and per-tier budgets.
- Camera rigs, shot sequencing, animation/audio synchronisation and rendered video/image-sequence export.
- VFX interaction with fog, water, wind, lighting and temporal reconstruction.

**Accept when:** a sample cinematic/gameplay sequence can be authored and profiled in Studio without hand-editing engine code for routine effects.

### VQ-024 — 2D visual pipeline
**State: partial.**

The 2D renderer has batching, sprite animation, tilemaps, autotiling and 2D lights. Remaining gaps for richer 2D games:
- 2D normal maps and shadow casting.
- Camera culling for large tilemaps and sprite-heavy scenes.
- Better lighting layers, occluders and sorting controls.
- Atlas workflows, animation authoring and overdraw diagnostics.
- Consistent post-processing and UI composition.

**Accept when:** a sample 2D scene with layered lighting, effects and a large world is correct and profiled.

## 4. Priority two — make high visual quality affordable

### VQ-030 — Complete resource budgets and residency
**State: partial.**

The current asset budget covers cache-owned textures/models, but not every major GPU resource class. Expand accounting and eviction/prefetch policy to cover:
- Textures and mip levels.
- Meshes and streamed mesh LODs.
- Environment maps, probes, lightmaps and shadow resources.
- Render targets and transient render-graph allocations.
- Animation clips, skeletons and crowd palettes.
- Particle buffers and simulation state.
- Audio streams and decoded assets.
- CPU-side decoded/cooked data.

Expose budgets and current use in Studio. Avoid evicting resources that are visible, about to be visible or required for deterministic simulation.

**Accept when:** a stress project stays within documented CPU/GPU memory ceilings while moving through a streamed level without visible missing assets or frame-blocking reloads.

### VQ-031 — Texture and mesh streaming
**State: partial.**

Texture mip streaming covers selected material/decal/model paths, not every asset category. Complete:
- Mip streaming for sprites, terrain layers, particles, sky/environment assets and other missing categories.
- Mesh LOD streaming by projected size and priority.
- Background decompression/transcode and bounded GPU uploads.
- Prefetch based on camera movement, streaming cells and gameplay hints.
- Pack-range reads so packaged games need not decompress unrelated content.
- Cancellation, retry, progress, dependency reporting and cache invalidation.

**Accept when:** a camera travelling through a large scene does not trigger avoidable stalls or exceed its residency budget.

### VQ-032 — Shader and pipeline compilation
**State: partial.**

Add a robust shader/pipeline cache and prevent first-use compilation hitches:
- Stable cache keys based on shader source, defines, backend, GPU and driver.
- Background compilation and safe pipeline creation.
- Variant stripping based on target project capabilities.
- Prewarming for the exact content in the packaged build.
- Failure recovery and actionable diagnostics for creators.
- A shader permutation report so content does not silently explode the variant count.

**Accept when:** cold and warm project start, scene transition and first-visible-material timings are measured, and no avoidable shader compilation hitch occurs in the showcase.

### VQ-033 — CPU simulation and animation LOD
**State: partial.**

The ECS has system ordering, intervals and selected simulation LOD. Extend it with:
- A dependency-aware phase graph and safe parallel execution.
- Physics, scripts, navigation, animation, particles and audio update budgets.
- Distance/visibility/importance-based LOD with hysteresis.
- Event-driven dormant entities and bounded catch-up.
- Crowd and traffic aggregation while preserving gameplay-critical outcomes.
- Debug views that explain why an entity updates at a given rate.

Visual LOD must not silently change authoritative gameplay or network outcomes.

**Accept when:** a representative populated scene reports per-system timing and population counts at each LOD and remains deterministic where required.

### VQ-034 — Overdraw, bandwidth and transparency budgets
**State: partial.**

Add tools and optimisations for fill-rate-heavy scenes:
- Overdraw and transparent-layer visualisation.
- Particle, foliage, decal and UI budgets.
- Depth prepass decisions based on actual tile/GPU behaviour.
- Texture bandwidth and render-target bandwidth accounting.
- Mip bias and resolution controls for expensive effects.
- Transparency strategy selection per quality tier.
- GPU cost measurements for shadow maps, SSR, fog and post effects.

**Accept when:** each quality tier has documented worst-case fill-rate workloads, and the profiler identifies the pass or content responsible for excessive cost.

### VQ-035 — Mobile thermal and power stability
**State: unproven.**

A brief 60 FPS run is not a mobile performance result. Measure long sessions and device throttling:
- Sustained CPU/GPU frame time and frame pacing.
- Temperature and power/battery draw where the OS exposes it.
- Memory pressure and background/foreground transitions.
- Upload bursts, shader compilation and loading hitches.
- Dynamic resolution and quality adaptation with user-visible settings.
- Suspend/resume and graphics-context/device-loss behaviour on devices.

**Accept when:** a representative play session holds its target profile within the chosen thermal and memory limits, or degrades gracefully and predictably.

### VQ-036 — World streaming and large-world support
**State: partial.**

The current world-cell streaming system needs stronger authoring and lifecycle support:
- Cross-cell references and persistent entities.
- Data layers for quests, seasons, interiors, day/night and event variants.
- Incremental cell save state.
- HLOD generation and streaming.
- Studio world view, cell bounds, streaming-source preview and memory view.
- Server ownership/interest management only for projects that require large multiplayer worlds.
- Large-world coordinate precision and physics/navmesh integration where needed.

**Accept when:** a large reference level streams through cells without stalls, broken references or unbounded resource growth.

### VQ-037 — Asset import and cooking completeness
**State: partial.**

The importer supports common model formats but does not yet provide every content pipeline feature a production art team may expect. Prioritise:
- Reliable ORM packing and normal-map conversion for common source conventions.
- Multiple UV sets and texture-transform handling.
- Clear import diagnostics for missing textures, unsupported extensions and ignored channels.
- Better FBX/glTF handling for animated materials, cameras, lights and additional deformers.
- Optional UDIM or large texture workflows if the art direction needs them.
- Reproducible, incremental asset cooking with dependency tracking.
- Asset validation for LODs, collision meshes, material budgets, texture dimensions and compression.
- Import presets for Blender and other common tools.

**Accept when:** a sample content pack imports from clean source files without manual repair and produces the same cooked assets on a clean machine.

### VQ-038 — Studio authoring and diagnostics
**State: partial.**

Runtime features need accessible, reliable tools. The highest-value missing workflows are:
- Material/shader preview under known lighting, with side-by-side comparisons.
- Lighting and reflection-probe placement/inspection.
- Terrain, foliage, spline road and procedural-world editing.
- Character/skeleton/animation workbench.
- VFX graph editor and effect profiler.
- World streaming/HLOD preview.
- GPU and memory profiler integrated with the viewport.
- Camera rigs and cinematic output.
- Asset dependency, memory and platform-variant views.

Tool polish should be tested with first-time creators and real game tasks, not only automated editor tests.

**Accept when:** a creator can author and debug the showcase's content through Studio without relying on undocumented scripts or direct C++ changes for routine work.

### VQ-039 — Startup, iteration and packaged-build performance
**State: partial.**

The performance strategy contains initial CLI/export measurements, but not a complete per-platform iteration baseline. Track:
- Empty project package size and cold start.
- Project open, first scene visible and first input response.
- Enter Play and return to Edit.
- Asset import/cook and reimport.
- Shader edit/compile/reload.
- Scene transition and streaming.
- Export time, install time and first launch.
- Pack size by assets, textures, audio, shaders and engine modules.

**Accept when:** every release candidate publishes reproducible cold/warm timings and package breakdowns for the supported platforms.

## 5. Optional high-end research — only when the target justifies the cost

These features can improve selected projects, but they should not be treated as blockers for making excellent games:

- Hardware ray tracing and path tracing for high-end desktop rendering.
- Mesh shaders, meshlets or Nanite-class virtualised geometry.
- Runtime virtual texturing.
- DLSS, FSR, XeSS or other vendor/platform upscalers.
- Advanced dynamic GI systems.
- FFT ocean simulation.
- Strand-based hair and advanced cloth simulation.
- Multi-GPU rendering.
- XR and console backends.
- Web/WebGPU export.

Each proposal needs a target device, a concrete reference scene, expected image-quality improvement, memory cost, frame-time cost and a fallback. Do not build a feature solely because a competing engine has a similarly named feature.

## 6. What is not a requirement for a visually impressive game

- VICTUS does not need to reproduce every Unreal or Unity subsystem.
- A mobile-first game does not need Nanite, hardware ray tracing or a full dynamic-GI solution to look excellent.
- More polygons do not automatically improve the image; silhouette, normals, materials, lighting and LODs matter more.
- A procedural city with many boxes is not a finished environment. It needs architectural kits, authored facades, signs, props, lighting, decals, vegetation and art direction.
- More post-processing does not compensate for poor exposure, flat lighting, low-quality assets or weak composition.
- A 1,000-agent benchmark on a two-joint rig does not prove a 1,000-character city workload.
- Passing lavapipe tests does not establish Android or discrete-GPU performance.
- A Blender render does not prove the VICTUS runtime produces the same image. Runtime captures are the acceptance evidence.

## 7. Recommended execution order

1. **Freeze misleading status claims.** Keep one source of truth for implemented/tested status; date every hardware and performance claim.
2. **Set the device and frame budgets.** Choose the minimum supported device and the sustained FPS goal before optimisation work.
3. **Add GPU profiling and complete reference scenes.** Do not optimise blind.
4. **Build one art-directed vertical slice.** A small district/interior with high-quality authored assets, Orion or Nova, finished lighting, materials, animation, VFX, audio and gameplay.
5. **Validate the vertical slice on the target hardware.** Record image captures, CPU/GPU timings, memory, thermals and loading.
6. **Fix the measured bottlenecks.** This will determine whether the next work is HLOD, GPU culling, material cost, shadows, streaming, overdraw or simulation.
7. **Expand the content workflows.** Improve procedural tools and Studio authoring based on what the vertical slice actually needs.
8. **Scale the scene only after it looks good.** Add the wider city, crowds and simulation while retaining the same image-quality and performance gates.

## 8. Definition of success

VICTUS should be considered ready for a particular visual target only when all of the following are true:

- A complete playable game scene has a deliberate art direction and production-quality assets.
- The runtime image, not only an offline render, meets the agreed visual reference.
- Lighting, materials, shadows, reflections, animation, effects and transparency remain correct in motion.
- Every supported quality tier has a documented and measured budget.
- The intended physical devices sustain the target frame rate without unacceptable thermal, memory or loading behaviour.
- Profiling tools can explain where CPU and GPU time are spent.
- The project can be built, exported and reproduced from a clean checkout.
- The limitations that remain are specific, dated, and do not contradict the current code or test evidence.

No finite checklist makes an engine universally complete. This register is complete enough to guide the current visual-quality and performance programme; add new entries only when a reference project, hardware test or creator workflow exposes a real gap.
