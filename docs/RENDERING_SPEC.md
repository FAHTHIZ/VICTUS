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

The sections above are the target. This section maps them to what main contains, as of commit `002e83d` (2026-11-22). The authority for status and limits is [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md). Each decision is recorded in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

**Hardware caveat.** Every rendering result was produced on Mesa lavapipe (software Vulkan) or CI's virtualised Apple GPU. No result has been measured on a discrete desktop GPU or a physical phone. That means the "Validation" requirements above are not met yet.

| Area | On main | Missing or limited |
|---|---|---|
| RHI and backends | VICTUS RHI over SDL_GPU 3.4 (ADR-0003). Vulkan is usable, Metal is experimental, and there is an OpenGL ES 3.0 fallback (ADR-0144). The RHI supports compute, indirect draws, readback and whole-volume 3D uploads (ADR-0115). Device-loss recovery rebuilds the GPU layer (ADR-0157). | D3D12 does not work (no DXIL). A frame render graph exists (`render_graph.hpp`): passes declare reads and writes, and `compile()` culls unused passes and aliases transient textures. Barriers are left to SDL_GPU. There is no ray-tracing or mesh-shader access through SDL_GPU. |
| Shaders and materials | GLSL 450 sources compiled offline into `builtin_shaders.cpp`. Project surface shaders compile at load time with glslang and SPIRV-Cross (ADR-0035). Material assets and instances (ADR-0034, ADR-0036), material graphs (ADR-0062), vertex functions and named parameters (ADR-0170, ADR-0171), and an FP16 PBR variant (ADR-0137, ADR-0153). | Four parameter slots and four custom textures per surface shader. No shader variant or permutation cache beyond the built-in set. No subsurface, anisotropic hair or clear-coat models. |
| Lighting | Clustered forward PBR with CPU light assignment (ADR-0046). Cascaded sun shadows with receiver culling and per-cascade LOD (ADR-0124, ADR-0132). Local light shadow atlas (ADR-0052), alpha-tested and soft shadows (ADR-0067), and sun contact shadows (ADR-0183). Rectangle area lights, point-light bulb radius and a Baked light mode (ADR-0194). | No dynamic GI beyond the probe refresh. No ray-traced or screen-space GI and no virtual shadow maps. Area lights cast no real-time shadows, and spot lights have no cookies yet (GAP-13). |
| Indirect light and reflections | IBL with an SH9 diffuse term (ADR-0042), physical sky (ADR-0043), reflection probes (ADR-0044), and baked irradiance volumes (ADR-0056) with round-robin dynamic refresh (ADR-0057). Baked lightmaps, directional lightmaps and a shadowmask (ADR-0122, ADR-0125, ADR-0130, ADR-0131), plus light probes for dynamic objects (ADR-0133, ADR-0138). | The bake uses the analytic sky only. No screen-space reflections. No real-time GI on mobile; Unity 7's Surface Cache GI targets that (see [the Unity/Unreal audit](audits/UNITY_UNREAL_2026_11.md)). |
| Anti-aliasing and upscaling | TAA from depth reprojection (ADR-0045), temporal upscaling (ADR-0055) with disocclusion, thin-feature locks, mip bias and a reactive mask (ADR-0141 to ADR-0143, ADR-0147, ADR-0148). Per-material responsive flag (ADR-0187), spatial upscale with CAS (ADR-0066), 4x MSAA with alpha to coverage on Low (ADR-0152), and the Low tier switches to TAAU below 0.65 scale (ADR-0188). | No DLSS, FSR 3 or XeSS integration. Arm ASR was evaluated but not adopted (ADR-0141). Thresholds were tuned on lavapipe only. |
| Post and camera | HDR with an optional packed R11G11B10F target (ADR-0050, ADR-0126, ADR-0129), bloom, auto exposure, tonemapping, lens effects (ADR-0146), SSAO, and an overdraw view (ADR-0071). | No depth of field, motion blur, colour-grading LUTs or physical-camera exposure units were found in `engine/render`. |
| Atmosphere and environment | Froxel volumetric fog (ADR-0098) on blended surfaces (ADR-0119), heterogeneous fog with sun self-shadowing (ADR-0186), volumetric clouds and cloud shadows (ADR-0111, ADR-0112), weather with wetness, puddles, ripples and occlusion (ADR-0113, ADR-0114, ADR-0117, ADR-0118), and Gerstner water (ADR-0095). | The sky is evaluated on the CPU into an equirect (ADR-0043), not a GPU atmosphere model. Water is Gerstner waves, not an FFT ocean. Reflections show the sky only (no planar reflections or SSR). |
| Geometry, LOD and culling | Automatic instancing (ADR-0013), meshoptimizer LODs (ADR-0049) with hysteresis (ADR-0077) and dithered cross-fade (ADR-0184), Hi-Z GPU occlusion with indirect draws (ADR-0051), retained instance groups with cluster culling (ADR-0162), and foliage groups with card LODs (ADR-0151, ADR-0164). | No virtualised geometry (Nanite-class) or GPU-driven scene submission. Large-world precision is not addressed. |
| Terrain and decals | Chunked heightfield terrain (ADR-0063) with height-blended layers (ADR-0123), and projected decals (ADR-0064). | No terrain streaming or holes, and no runtime virtual texturing. |
| Transparency | Blended geometry is sorted far to near. Fog is applied to blended surfaces, and GPU particles are depth-sorted (ADR-0107). | No order-independent transparency. Unreal 5.9 adds AVBOIT (GAP-39 in the [gap register](audits/ENGINE_LANDSCAPE_AND_GAP_AUDIT.md)). |
| Streaming | Asynchronous loads with an upload budget, a residency budget, and mip streaming by projected footprint (ADR-0087 to ADR-0089, ADR-0092). | No virtual texturing and no streaming of mesh LODs. |
| VFX | CPU emitters, flipbooks and stretched billboards (ADR-0016, ADR-0053, ADR-0084), ribbon trails (ADR-0085), GPU simulation with curl noise and depth collision (ADR-0097), sub-emitters (ADR-0108), GPU trails (ADR-0110) and per-tier budgets (ADR-0109). | No node-based VFX graph editor. |
| 2D | Sprite batching, sprite sheets (ADR-0038), tilemaps with autotiling, texture filtering per import (ADR-0039), and 2D lights (ADR-0065). | No 2D shadows or normal maps, and tiles are not culled by the camera. |
| Quality tiers | `QualityTier` is Low, Medium, High or Ultra (`scene_renderer.hpp`) rather than the four names in the target above. There is an "auto" device-class tier (ADR-0127), a frame cap (ADR-0128), dynamic resolution from CPU frame time (ADR-0045), and per-platform texture profiles (ADR-0149). | Dynamic resolution uses CPU time, not GPU timestamps. |
| Diagnostics and validation | Render-path report with reason codes (ADR-0103, ADR-0105), perceptual compare with FLIP and SSIM (ADR-0102), and deterministic reference scenes (ADR-0104, ADR-0140). | No GPU timestamp breakdown per pass is shown to users, and there is no RenderDoc or PIX capture integration. |
