# Camera-responsive LOD audit (Workstream H1/H3)

Scope: how the current renderer picks mesh levels of detail when the camera FOV, viewport size, render
scale or view type changes. This covers the first evidence slice of the camera-responsive work in
[COMPETITIVE_MATURITY_GAP_CLOSURE.md](../handoff/COMPETITIVE_MATURITY_GAP_CLOSURE.md) (Workstream H)
and the [camera-responsive visual fidelity handoff](../handoff/VICTUS_CAMERA_RESPONSIVE_VISUAL_FIDELITY.md).
It describes the code as it is. It does not claim A.E.G.I.S. behaviour, which is a separate system
(ROADMAP 6e).

## Current selection rule

`select_lod` in `engine/render/src/scene_renderer.cpp` (ADR-0049) picks the coarsest level whose
geometric error, projected to render pixels, stays within `RenderSettings::lod_error_pixels`:

\[ k = \frac{s \cdot h_{render}}{2 \tan(\mathrm{fov}_y / 2) \cdot d} \qquad \text{error}_{lod} \cdot k \le \text{budget} \]

- \(h_{render}\) is the render-target height after `render_scale` and dynamic resolution, so the budget
  is in rendered pixels, not output pixels.
- \(\mathrm{fov}_y\) is the main camera's vertical FOV, so a narrow FOV (zoom) increases the projected
  error and keeps finer levels.
- \(d\) is the distance from the eye to the nearest point of the draw's world bounds, and \(s\) the
  largest axis scale of the model matrix.
- The rule is stateless: no hysteresis and no cross-fade. The same camera always gives the same level.

## Verified behaviour (tests/render/gpu_lod_tests.cpp)

| H3 item | Check | Result |
| --- | --- | --- |
| 1. Wide/default/narrow FOV, same position | FOV 100, 60, 30, 10 and 3 degrees: triangles never decrease as FOV narrows, and 3 degrees uses more than 100 | pass |
| 2. Dolly vs zoom with equal coverage | distance 6 at 60 degrees vs distance 18 at the matching FOV: same level | pass |
| 3. Different render-target sizes | 160x96 vs 640x384 at the same camera: the taller target keeps a finer level | pass |
| 5. Dynamic resolution / render scale | `render_scale` 0.25 picks a coarser level; restoring 1.0 recovers the original level exactly | pass |
| 9. Rapid zoom | six alternating 2/120 degree frames, then back to 45: same level as before | pass |
| Shadow LOD bias | `shadow_lod_bias` 2 vs 0: same main triangles, fewer `shadow_triangles`, image within 1.5/255 | pass |
| 6. Texture close-up after a distant view | per-level coloured 64x64 chain on a tiled plane (tests/render/gpu_texture_mip_tests.cpp): close-up samples mip 0, 8 m away samples about level 3, returning or zooming to 1.5 degrees samples mip 0 again; nearest filtering minifies through mips too | pass |
| 7. Character over poses and distances | Vanguard V-01 through the real pipeline (GLB, asset database, scene, AnimationSystem, extraction, skinned draws) in Idle, Run, Jump and Interact at 2.5, 8 and 20 m (tests/render/gpu_character_lod_tests.cpp): LODs draw 10 054, 5 028 and 2 578 of 20 632 triangles; at most 3 pixels per 256x256 frame differ by more than 48/255 from every pixel within one pixel of the LOD 0 image; changed pixels are silhouette edges (visor, insignia and hands survive) | pass |
| 3. Main and secondary views | One SceneRenderer draws a 320x180 FOV 60 main view and a 96x96 FOV 6 scope view of nine receding spheres, standalone and interleaved (main, scope, main) twice (tests/render/gpu_lod_tests.cpp). Main: 9 visible, 4455 tris; scope: 7 visible, 3962 tris (finer levels per object). Interleaved frames reproduce each view's triangles, coarser-draw count, shadow triangles and image (mean diff < 0.25). Selection is stateless per `render()`, so no view inherits another's levels; each call's SceneStats is that view's cost. The engine has no secondary-camera component yet: the editor viewport and asset thumbnails each own a SceneView, and reflection probes select per face | pass |
| 8. Terrain chunk boundaries during zoom | 129-sample rolling heightfield, 16 chunks, grazing camera at FOV 75/35/12/4 with 1 px and a stress 8 px budget (tests/render/gpu_terrain_tests.cpp): no cracks (sky pixels with LOD-frame ground above and below where LOD 0 shows ground); 1 px silhouette notches on the horizon stay within 0.1% of ground pixels at the default budget. Mutation check: removing the skirts makes the 8 px pass report 8 and 31 cracks | pass |
| 8. Alpha-cutout surfaces during zoom | 30% speckle cutout texture on a plane, top-down at 0.5/6/15/40 m (tests/render/gpu_alpha_coverage_tests.cpp). Before: box-filtered alpha mips render 0.28/0.11/0.002/0 coverage, so the surface vanishes. After ADR-0070 (coverage-preserving mips): 0.28/0.11/0.15/0.18 | fixed (ADR-0070); see finding 7 |
| 4. Off-screen shadow contribution | a caster outside the main frustum still darkens the visible ground (`visible == 1`, `shadow_casters == 2`) | pass |

All checks run on Mesa lavapipe (llvmpipe) in CI. They are CPU-side selection and image checks, not
GPU timing.

## Findings and gaps

1. **Shadow views use the main camera's level, with an optional bias.** Directional cascades, local
   (spot/point) shadow tiles and the depth pre-pass use the level picked for the main view; this matches
   Godot and Unreal defaults and keeps the shadow silhouette consistent with the lit surface.
   `RenderSettings::shadow_lod_bias` (0..3, Low 1) draws casters that many levels coarser
   (ADR-0049 addendum). Tested in "gpu lods: shadow_lod_bias draws coarser casters without changing the
   main view": main-view triangles unchanged, shadow triangles lower, mean image difference under 1.5/255,
   instancing kept. Independent per-cascade selection from shadow texel density is not implemented.
2. **Reflection probe captures already select per view.** Each capture face renders through the same
   `SceneRenderer::render` with its own 90-degree camera and face resolution, so its level follows that
   view.
3. **No transition smoothing.** Levels switch instantly when the projected error crosses the budget.
   There is no hysteresis band, dithered cross-fade or geomorph. Popping is bounded by the pixel budget
   (1 px by default) but not hidden. Follow-up together with A.E.G.I.S. (6e).
4. **No per-view budget.** `lod_error_pixels` is global to the renderer settings. A secondary view such
   as a minimap or split screen cannot ask for a looser budget than the main view except through a
   separate `SceneRenderer` or settings change.
5. **Texture mips are separate from mesh LOD.** The GPU picks the mip from the texel footprint, so
   FOV zoom and render scale change the sampled level the same way they change mesh levels. Mip chains
   exist on every cooked path: standalone textures (`TextureImportSettings::mipmaps`, default on), model
   textures (always), and compressed variants (used only when their level count equals the RGBA8 chain;
   otherwise the RGBA8 chain uploads). `GpuContext::create_texture` builds a chain by default; built-in
   1x1 defaults, UI atlases and sprite-batch internals have none by design. All textures stay resident:
   there is no mip streaming, eviction or view-prioritised residency, so do not claim it (H2).
6. **Profiler GPU time.** Frame-time numbers from the profiler's GPU wait are not GPU execution time
   (H3). These tests report triangle counts and images only.
7. **Alpha-tested foliage vanished with distance (fixed).** Box-filtered alpha mips averaged sparse
   cutout texels below the cutoff. Coverage-preserving mips (ADR-0070) fix this for imported
   textures and glTF `MASK` materials. One-texel features still erode by about 60% at mid
   distances under bilinear alpha testing (both mip chains render 0.11 at 6 m); hashed alpha testing
   is the recorded follow-up.

## Separate cost counters (H2)

Geometry, shadows and shading are reported apart, in `SceneStats`, the `--profile` JSON (gated by
tests/perf/budgets.json) and the editor viewport overlay:

| Cost | Counters |
|---|---|
| Geometry | `triangles`, `lod_draws`, `visible`, `main_batches` |
| Shadow maps | `shadow_triangles`, `shadow_batches`, `local_shadow_draws` |
| Shading and transparency | `alpha_tested_draws`, `surface_shader_draws`, `transparent_draws`, `transparent_triangles` |
| Texture memory | `gpu.texture_bytes`, `gpu.live_textures` |

These are submission counts, not measured fill. Measured per-pixel overdraw comes from the
overdraw debug view (ADR-0071): `RenderSettings::debug_view = DebugView::Overdraw` redraws the
visible opaque, alpha-tested and blended batches without depth testing into a per-pixel counter,
shows it as a heat ramp (editor: View > Overdraw View) and `SceneRenderer::read_overdraw()` returns
covered pixels, total layers, maximum and mean. Evidence (`gpu_overdraw_tests.cpp`, 128x96): three
stacked 4 m planes seen from 8-10 m give a maximum of 3, 1764 covered pixels and a mean of 2.39
layers; back faces count only on double-sided materials. It is depth complexity, not shaded
fragments (the pre-pass rejects most hidden opaque layers), and particles and decals are not
counted. Anisotropic filtering has no setting yet.

## Not covered yet

- H3 item 8: thin opaque geometry (railings modelled as meshes) below one pixel wide; mesh LOD keeps
  them up to the 1 px budget, but no sub-pixel coverage test exists. Item 10 (physical Android
  devices) is hardware-blocked.
- Terrain chunk LODs (ADR-0063) go through the same `select_lod` rule as other meshes, so the FOV, viewport and render-scale checks apply to them as well.
