# Photoreal on low-end hardware: competitor research and engine audit

Dated: **2026-11-20**. VICTUS revision: `main` at `ded1487`.

The owner's direction for VICTUS: **images that read as photoreal, the way Unreal's do, on hardware as weak as the phones Unity reaches.** This document records what the competitors actually do to get there, where VICTUS stands against that, and the ordered plan that replaces the earlier "creator experience first" ordering in [ROADMAP.md](../../ROADMAP.md). It complements, and does not replace, the workflow comparison in [COMPETITOR_BENCHMARK.md](COMPETITOR_BENCHMARK.md) and the brief in [VICTUS_PERCEPTUAL_EQUIVALENCE_RENDERING.md](../handoff/VICTUS_PERCEPTUAL_EQUIVALENCE_RENDERING.md).

## The physical constraint

An entry-level phone GPU is one to three orders of magnitude weaker than a current console's. Two common entry-level GPUs, from vendor-spec aggregators (<https://gadgetversus.com/graphics-card/arm-mali-g52-mc2-vs-qualcomm-adreno-610/>, <https://hmc-tech.com/gpus/arm-mali-g52-mp2-800mhz-vs-qualcomm-adreno-610>):

| GPU | FP32 peak | Memory bandwidth | Vulkan |
|---|---|---|---|
| Arm Mali-G52 MP2 | ~38–82 GFLOPS | shared LPDDR4X, ~15 GB/s | 1.1 |
| Qualcomm Adreno 610 | ~243 GFLOPS | ~15–17 GB/s | 1.1 |

No engine renders Unreal-on-PC images in real time on that budget. What every competitor does instead is **move the expensive light transport off the device**: bake it, store it compactly, and spend the phone's budget on the parts that must stay dynamic. That is exactly VICTUS's perceptual-equivalence goal, and it decides the plan below.

## What the competitors do

### Unreal Engine 5.8

- **Lumen and Nanite are not on the mobile renderer.** "The Mobile Renderer doesn't support Lumen"; Lumen on Android works only through the desktop renderer on Vulkan SM5 devices ([Lumen technical details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine)). Shipping mobile titles disable both and "bake lighting, use static shadows, and rely on precomputed reflection captures" ([industry summary](https://game-ace.com/blog/unreal-engine-for-mobile-games/)).
- **5.8 mobile renderer:** multi-pass deferred by default, mobile SSAO, SSR, deferred decals, contact shadows, better water, and **explicit FP16 in targeted materials and global passes** ([5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes)).
- **Device profiles + scalability** map per-feature quality to device classes ([UE mobile development](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-creating-mobile-games?application_version=5.8)).
- **Mobile lighting tiers**: fully static lightmaps; or lightmaps plus one per-pixel sun with **precomputed distance-field shadows** ([UE mobile performance guidelines](https://docs.unrealengine.com/5.1/en-US/performance-guidelines-for-mobile-devices-in-unreal-engine/)). Lightmaps are **directional** (normal maps still respond) and HDR-encoded as SH ([PBR on mobile](https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile), [UE 4.27 mobile lighting](https://dev.epicgames.com/documentation/en-us/unreal-engine/lighting-for-mobile-platforms?application_version=4.27)).
- **Upscaling**: Mobile FSR in the engine; Arm ASR as a plug-in.

### Unity 6 (URP)

- **Baked GI is the default answer**: lightmaps plus **Adaptive Probe Volumes** for dynamic objects ([APV](https://docs.unity3d.com/6/Documentation/Manual/urp/probevolumes-use.html)).
- **Lightmap encodings per platform**: RGBM (0–34.49 linear) on desktop, **dLDR** (0–2, ×4.59 decode) on mobile because hardware block compression of RGBM artefacts badly ([lightmap data format](https://docs.unity.com/en-us/engine/6000.6/manual/lighting-overview/direct-and-indirect-lighting/lightmap-data/lightmaps-technical-information)).
- **GPU Resident Drawer** helps CPU-bound scenes but "slightly increases GPU workload; lower-end mobile … experience this effect more strongly" ([GPU Resident Drawer](https://docs.unity.com/en-us/engine/6000.6/manual/analysis/graphics-performance-profiling/in-urp/reduce-draw-calls-urp/reduce-rendering-work-on-cpu/gpu-resident-drawer-performance)).
- **Call of Duty: Mobile** (Unity, custom SRP): fully PBR with IBL and specular occlusion, procedurally placed light probes, reflection probes, **static shadow masks combined with dynamic cascaded shadow maps** ([GDC 2019 talk transcript](https://www.youtube.com/watch?v=KJjOO1FdY1M)).

### Godot 4.7

- **Three renderers**: Forward+ (desktop), Mobile (Vulkan/D3D12/Metal), and **Compatibility (OpenGL) for old and low-end devices** ([renderers overview](https://docs.godotengine.org/en/latest/tutorials/rendering/renderers.html)).
- **LightmapGI on every renderer**, including Compatibility (rendering only; baking needs a RenderingDevice). VoxelGI, SDFGI and SSIL are Forward+ only (same page).
- **Directional (SH) lightmaps** and **shadowmasks** for distant sun shadows ([LightmapGI](https://docs.godotengine.org/de/4.5/classes/class_lightmapgi.html), [LightmapGIData](https://docs.godotengine.org/en/stable/classes/class_lightmapgidata.html)).
- **Explicit FP16 in the Mobile renderer**: ~10 % frame time on Adreno 640 and up to ~25 % with spike removal on Mali-G710 ([godot#107119](https://github.com/godotengine/godot/pull/107119)).

### Others and techniques

- **Genshin Impact** (Unity, cross-platform including phones): clustered deferred lighting with hundreds of local lights, volumetric fog, SMAA+TAA, separate pipelines for characters and scenes ([HoYoverse interview summary](https://www.vanas.us/en/blog/the-incredible-visuals-of-genshin-impact)).
- **Arm Accuracy Super Resolution**: FSR2-derived temporal upscaler optimised for mobile, **MIT-licensed** with a generic library for other engines ([Arm ASR](https://developer.arm.com/mobile-graphics-and-gaming/arm-accuracy-super-resolution), [licence](https://github.com/arm/accuracy-super-resolution/blob/main/LICENSES/MIT.txt)).
- **Tile-based GPUs**: bandwidth, not ALU, is the limiter. Avoid framebuffer round trips, clear or invalidate attachments, keep intermediate data in tile memory, rely on AFBC ([Arm tile-based rendering](https://documentation-service.arm.com/static/636dfb934e6cf12278ad8c27), [Mali best practices](https://armkeil.blob.core.windows.net/developer/Arm%20Developer%20Community/PDF/Arm%20Mali%20GPU%20Best%20Practices.pdf)). Mali's Forward Pixel Kill makes a separate depth pre-pass almost never worth its vertex and bandwidth cost (same guide).
- **Lightmap UVs**: xatlas (MIT, used by The Witness) is the standard open-source unwrapper/packer ([xatlas](https://github.com/jpcy/xatlas)). three.js now ships ray-traced radiosity and **vertex-baked GI** (no lightmap UVs, 1/4–1/40 of a lightmap's size) ([three.js #35124](https://github.com/mrdoob/three.js/pull/35124)).
- **Dynamic local lighting on low-end phones**: SIGGRAPH 2025 presented a stochastic tile-based method with fixed-cost shadowed local lights "even on low-end mobile GPUs" ([Advances 2025](https://www.advances.realtimerendering.com/s2025/index.html)).

### Android reach

- Google's dashboard (7 days to 2025-11-24, handhelds): **no Vulkan 7.37 %, Vulkan 1.0 3.86 %, 1.1 62.09 %, 1.3 26.01 %, 1.4 0.67 %** ([distribution dashboard](https://developer.android.com/about/dashboards)).
- Google's minimum for native engines is Android 10, Vulkan 1.1 and the 2022 Android Baseline profile; "most, but not all" 1.1 devices meet it ([native engine support](https://developer.android.com/games/develop/vulkan/native-engine-support)).
- SDL_GPU on Vulkan needs 1.0 + `VK_KHR_maintenance1` and a set of features, several of which can be switched off "to run on some older Android devices" ([SDL GPU docs](https://pub.dev/documentation/sdl3/latest/topics/gpu-topic.html)). **SDL_GPU has no OpenGL ES backend.** One public SDL_GPU game implements its own GLES 3.0 path for the SDL_GPU calls it uses ([GT3-revisited RENDERING.md](https://github.com/matt-pulsipher/GT3-revisited/blob/master/docs/dev/RENDERING.md)).

## Audit: VICTUS against this target

Full-repository facts at `ded1487`: ~75 k lines of engine/editor/tools C++ (excluding the 30 k-line generated shader table), ~43 k lines of tests, 28 engine modules, 57 shaders, 343 commits since 2026-11-16. Subsystem-by-subsystem maturity stays in [KNOWN_LIMITATIONS.md](../KNOWN_LIMITATIONS.md); this table is only what blocks the target.

| # | Finding | Evidence | Severity for the target |
|---|---|---|---|
| A1 | **No baked lighting.** No lightmaps, lightmap UVs, shadowmasks or vertex-baked GI. Indirect light comes from the analytic sky, SH from a baked environment, or irradiance-volume probes, all of which are off or coarse on Low. | `rg -i lightmap engine` finds nothing; `RenderSettings::preset(Low)` turns off irradiance volumes, SSAO, local shadows and bloom | **Critical.** Every competitor's photoreal-on-mobile answer is baked GI |
| A2 | **Never run on a phone or a real GPU.** All renders come from lavapipe and CI's virtual Apple GPU. | `PLATFORM_SUPPORT.md`, `KNOWN_LIMITATIONS.md` | Critical, blocked on hardware |
| A3 | **Vulkan-only on Android.** No OpenGL ES fallback, so ~7 % of handhelds cannot start and Vulkan 1.0 devices (~4 %) are at risk. | `docs/ANDROID.md`: "SDL_GPU has no OpenGL ES backend, so Vulkan is required" | High |
| A4 | **FP32 everywhere.** No shader uses `mediump`/`float16_t`. | `rg mediump\|float16 engine/render/shaders` is empty | High (10–25 % measured elsewhere) |
| A5 | **The compact HDR target is never used.** `SceneRendererOptions::compact_hdr` (R11G11B10) exists, but nothing enables it, so phones render into RGBA16F at twice the bandwidth. | only definition in `scene_renderer.hpp:612` and use at `scene_renderer.cpp:553` | High, cheap fix. **Fixed 2026-11-20:** on for phones and Low/Medium (ADR-0126), with stochastic rounding into the packed targets (ADR-0129) |
| A6 | **One quality tier for every phone.** The player picks `mobile_quality` for all mobiles; there is no device-class mapping like UE's device profiles. | `engine/player/src/player.cpp:110-119` | Medium |
| A7 | **Low tier keeps several full-screen round trips.** Tonemap, spatial upscale and FXAA are separate passes; each is a full write and read on a tile-based GPU. | pass list in `scene_renderer.cpp` (`tonemap`, `upscale_sharpen`, `fxaa`) | Medium |
| A8 | **Content looks like programmer art.** Templates and samples use primitives and flat colours; there is no reference scene with scanned materials to judge "photoreal" against. | `docs/images/*` | Medium; quality cannot be judged without it |
| A9 | **Directional SH lightmaps, shadowmasks, distance-field sun shadows, impostors and a mobile temporal upscaler are all missing.** | — | Follow-ups after A1 |
| A10 | **CI was red** (clang-tidy cast, two GPU tests without the skip guard, unbounded `make -j`). | fixed in `ded1487` | Fixed |

What is already right and should be kept: SDL_GPU device creation already relaxes clip-distance, depth-clamp and first-instance features for older Android drivers (`rhi_sdl.cpp`); the render graph uses `DontCare` loads where it can; ASTC/BC7 texture compression, meshoptimizer LODs, occlusion culling, dynamic resolution, a Low environment bake and perceptual image comparison (`victus compare`) all exist. The perceptual-equivalence brief already states the right goal.

## Plan, in order

Each item lands on `main` with tests and a `STATUS.md` entry before the next starts.

1. **Baked lighting v1 (A1).** xatlas lightmap UVs per mesh; a CPU path-traced baker (BVH, sun, sky, point and spot lights, multi-bounce diffuse, albedo from materials, dilation and a denoise filter); a per-scene lightmap atlas; per-instance scale/offset; renderer sampling that replaces ambient for lightmapped surfaces; `victus bake-lighting`; a Studio *Bake Lighting* command. Encodings follow Unity's reasoning: RGBM on desktop, dLDR-style on mobile.
2. **Mobile bandwidth quick wins (A5, A7).** R11G11B10 scene colour on Low/Medium and on mobile; no depth pre-pass where SSAO is off; fold FXAA into tonemap on Low.
3. **Baked lighting v2.** Directional (L1 SH) lightmaps for normal maps, a sun shadowmask so Low can drop realtime cascades beyond a short distance, light probes baked from the same tracer for dynamic objects, and lightmapped templates.
4. **FP16 shading (A4)** in pbr.frag and post passes behind a device capability check, verified against `victus compare` goldens. Done for pbr.frag's BRDF (ADR-0137); post passes still FP32.
5. **Device classes (A6).** GPU-name and capability-based tier selection with project overrides, recorded in the run report.
6. **Photoreal reference scene (A8).** CC0 scanned materials (Poly Haven) in a lightmapped interior and exterior, used as the perceptual-equivalence reference across tiers.
7. **Temporal upscaling for mobile.** Evaluate Arm ASR (MIT) against the existing TAA upscaler on the reference scene.
8. **OpenGL ES 3 fallback (A3).** ADR first: an RHI backend that implements the subset VICTUS uses, versus dropping pre-Vulkan-1.1 devices on purpose.
9. **Hardware proof (A2).** Still blocked on a phone; everything above produces the captures to run as soon as one exists.

## Re-run conditions

Re-check this document when any competitor ships a new mobile renderer feature, when Google publishes a new Vulkan distribution snapshot, and after the first on-device run.
