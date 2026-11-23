# VICTUS Rendering Specification

## Goal
Deliver real-time 2D and 3D rendering through a VICTUS-owned rendering architecture, with measurable quality/performance tiers. High-end visuals are a long-term target, not a claim of current capability. Hardware-specific features require validated backends and fallbacks.

## Shared foundation
- RHI for devices, queues, resources, descriptors, surfaces, synchronization and capability discovery.
- Render graph with explicit resource lifetime, barriers, transient allocation and diagnostics.
- Shader source/build pipeline, reflection, variants, cache, hot reload and compile errors.
- Scene extraction, camera/view model, visibility, materials, lights, render queues and debug overlays.
- GPU timestamps, captures, resource inspection and CPU/GPU frame breakdown.
- Color management, units, exposure, tone mapping and consistent asset interpretation.

## 3D rendering
- Mesh and instance submission, vertex formats, skinning and morph targets.
- Physically based material workflow and texture sampling.
- Direct/indirect lighting, environment lighting, probes and scalable shadows.
- Reflection techniques and screen-space effects with quality tiers.
- Anti-aliasing, temporal reconstruction research, post-processing and color grading.
- Terrain, foliage, water, atmosphere, sky, clouds, weather and large-world precision.
- Decals, transparency, particles, trails, volumetric effects and dynamic environment presentation.
- Mesh/texture LOD, occlusion, instancing, GPU culling and asynchronous streaming.
- Advanced research: ray tracing, global illumination, virtualized geometry and virtual texturing. These are selected by measured value and platform support, not feature-count pressure.

## 2D rendering
- Sprites, sprite atlases, batching, sorting/layers, camera and pixel-art controls.
- Tilemaps, tile sets, autotiling, grid editing, parallax and 2D scene composition.
- 2D lights/shadows, particles, animation, canvas/UI integration and debug drawing.
- Efficient handling of large tile worlds, texture atlases and overdraw.

## VFX and cinematics
VFX authoring must include emitters, CPU/GPU simulation choices, curves, forces, trails, collision, sub-emitters, deterministic seed controls and preview/debugging. Cinematic rendering must integrate sequencer tracks, camera rigs, animation, audio, events, render settings and frame capture.

## Quality tiers
Define capability-driven high, standard, mobile and compatibility profiles. Profiles may vary resolution, shader variants, shadow quality, reflections, effects, texture formats, geometry detail and streaming budgets. Dynamic resolution/adaptive quality must expose diagnostics and user override. Never silently change simulation correctness to preserve frame rate.

## Validation
Use repeatable scenes for indoor/outdoor lighting, material variety, transparency, foliage, large geometry, 2D tilemaps, particles and stress workloads. Record hardware, driver, build, settings, CPU/GPU timings, memory and image comparisons. Validate physical mobile devices before claiming mobile performance.

## Implementation status

This table describes the renderer at code baseline `203dbfd` (2026-11-22). It is a compact map, not a substitute for the detailed evidence. The [visual quality and performance gap register](VISUAL_QUALITY_AND_PERFORMANCE_GAP_REGISTER.md) is the prioritized work plan; [known limitations](KNOWN_LIMITATIONS.md) describes subsystem maturity; [engineering status](STATUS.md) records the commands and results. Platform execution is listed in [PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md).

**Validation boundary:** automated image tests currently use Mesa lavapipe (software Vulkan), Mesa GLES and CI's virtualised Apple GPU. No physical GPU performance capture is recorded. Android packaging is exercised in CI, but the platform matrix records no install on a physical phone. Do not interpret software-renderer timing as a phone or desktop-GPU benchmark.

| Area | Implemented foundation | Material gaps / limits |
|---|---|---|
| RHI and backends | SDL_GPU RHI with Vulkan, Metal and GLES paths; render graph, resource management, compute and indirect draws | Backend execution and features vary by platform; Windows D3D12 is unavailable because the DXIL path is not produced; no GPU timestamp-query abstraction; physical device coverage is missing |
| Materials and shaders | Metallic-roughness PBR, material assets/instances/graphs, normal/emissive/ORM maps, custom surface and vertex functions, world-space triplanar mapping, clear coat, sheen and anisotropic specular with glTF `KHR_materials_clearcoat` / `KHR_materials_sheen` / `KHR_materials_anisotropy` factor import | Clear-coat, sheen and anisotropy textures are not imported; anisotropy is isotropic under area and sphere lights; clear coat and sheen do not shade area lights; shader parameters/resources remain constrained; custom vertex functions do not participate in every depth/motion/culling path; moving objects need object-space triplanar mapping |
| Direct lighting and shadows | Clustered forward lights, cascaded sun shadows, local shadow atlas, contact shadows for the sun, rectangle area lights, point-light radius, baked area-light mode, spot/point/directional cookies | Area lights have no real-time shadow path and use representative-point specular; sun contact shadows have limited coverage; shadow stability, cache behaviour and cost need physical-GPU profiling |
| Indirect lighting | IBL, environment maps, reflection probes, irradiance volumes, baked lightmaps, directional lightmaps, shadow masks and baked probes | No general dynamic GI solution; bake and probe workflow, light leaking, moving-object response and time-of-day transitions need further work |
| Reflections and water | Water SSR; opaque glossy-surface SSR reusing TAA history; environment/probe fallback; Gerstner water | Opaque SSR depends on TAA and is disabled on unsupported quality/backend paths including GLES; screen-space reflections cannot see off-screen/hidden geometry; planar reflections, refraction and higher-fidelity ocean work are not complete |
| Anti-aliasing and post | TAA/TAAU, FXAA/MSAA paths, temporal reconstruction controls, bloom, auto exposure, tone mapping, grading/vignette and selected lens effects | Quality and thresholds have not been tuned on physical target GPUs; depth of field and motion blur are not implemented in the documented renderer; post effects need target-specific budgets |
| Sky and atmosphere | Analytic/physical sky, IBL integration, volumetric fog and clouds, cloud shadows, procedural rain/snow, wetness and puddles | Several systems are approximations with documented integration limits; fog/cloud/weather capture, temporal stability, authored masks, probe consistency and mobile cost need work |
| Geometry, terrain and foliage | Automatic instancing, mesh LODs with dithered transitions, Hi-Z occlusion, retained instance groups, chunked heightfield terrain, foliage scatter and cluster-level retained groups | No city/building HLOD pipeline; no foliage wind or far-field impostors; world-static GPU-driven submission/culling is incomplete; mesh LOD streaming and terrain streaming remain limited |
| Characters and crowds | GPU skinning, state machines/blend spaces, IK/retargeting foundations, Orion/Nova Frame characters, baked-palette `CrowdAgent` rendering | Crowd clips are sampled at 30 FPS and do not blend, cross-fade, use IK/root motion/morphs, or automatically hand off to near-camera Animators; impostors and full crowd spawning/LOD are incomplete |
| Procedural environments | Seeded procedural city with roads, blocks, lots, box-based buildings, parks, trees, basic props, traffic and sidewalk pedestrian loops | Not a general artist-directed PCG graph; generated city is transient and currently appears in Play; architecture kits, detailed facades, terrain integration, curved roads, cell output/streaming and realistic traffic/pedestrian behaviour remain |
| VFX and transparency | CPU/GPU particles, trails, sub-emitters, fog integration and sorted blended geometry | No visual VFX graph editor; transparency remains order-dependent; GPU particles have documented sorting/collision/source limitations; lit/mesh particles and broader compositing need work |
| 2D | Sprite batching/animation, tilemaps, autotiling and 2D lights | 2D normal maps/shadows and camera culling for large tilemaps are absent |
| Profiling and validation | Render-path reports, pixel-probed tests, reference scenes, FLIP/SSIM comparison, CPU profiling and CI performance counters | No per-pass GPU timestamp breakdown through the RHI; no physical-device reference captures or sustained thermal results; visual art-direction review is separate from automated correctness tests |

For precise, changing details, prefer [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) and the dated [STATUS.md](STATUS.md) evidence over older prose summaries. Update this table whenever a renderer capability materially changes.
