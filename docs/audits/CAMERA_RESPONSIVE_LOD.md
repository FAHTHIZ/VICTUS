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
5. **Texture mips are separate.** Mip selection is the GPU's footprint-based sampling and is not driven
   by the LOD rule. There is no view-prioritised mip streaming (H2); do not claim it.
6. **Profiler GPU time.** Frame-time numbers from the profiler's GPU wait are not GPU execution time
   (H3). These tests report triangle counts and images only.

## Not covered yet

- H3 items 6 (texture close-up after a distant view), 7 (character face/hands over poses), 8 (thin
  railings, foliage, alpha cutout and terrain chunk seams during zoom) and 10 (physical Android devices,
  hardware-blocked).
- Terrain chunk LODs (ADR-0063) use their own selection and are not part of these checks.
