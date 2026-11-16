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
