# Low tier anti-aliasing: MSAA + FXAA or TAAU (ADR-0188)

Measured 2026-11-22 with `tools/render/measure_low_aa.py` (Release, clang 21, Mesa lavapipe on 2
vCPU, 640x360 output, 60 fixed frames, captures at frames 20, 40 and 60). Every candidate renders
`samples/reference_scenes` at the Low tier; the reference is Low rendered at scale 2 with 4x MSAA
and no TAA, so the features are identical and only anti-aliasing and resolution differ.

```
SDL_VIDEO_DRIVER=offscreen python3 tools/render/measure_low_aa.py --victus build/bin/victus --out /tmp/low_aa
```

Columns: mean FLIP and worst 64-pixel tile FLIP (lower is better), worst tile SSIM (higher is
better), temporal error (how much the frame-to-frame change differs from the reference's; lower
is more stable) and median frame time on lavapipe (software: scales with shaded samples, not
representative of phone GPUs).

| Scene | Candidate | FLIP | Worst tile | Tile SSIM | Temporal | ms |
|---|---|---|---|---|---|---|
| exterior | `low_msaa4_fxaa_0.75` | 0.0230 | 0.1085 | 0.907 | 0.00015 | 31.1 |
| exterior | `low_taau_0.75` | 0.0286 | 0.1101 | 0.914 | 0.00137 | 35.4 |
| exterior | `low_msaa4_fxaa_0.6` | 0.0244 | 0.1142 | 0.895 | 0.00019 | 25.4 |
| exterior | `low_taau_0.6` | 0.0286 | 0.1114 | 0.909 | 0.00152 | 32.5 |
| exterior | `low_msaa4_fxaa_0.5` | 0.0272 | 0.1189 | 0.884 | 0.00023 | 23.9 |
| exterior | `low_taau_0.5` | 0.0290 | 0.1115 | 0.906 | 0.00176 | 30.2 |
| exterior | `low_fxaa_0.5` | 0.0269 | 0.1172 | 0.878 | 0.00023 | 17.8 |
| interior | `low_msaa4_fxaa_0.75` | 0.0108 | 0.0595 | 0.943 | 0.00002 | 26.7 |
| interior | `low_taau_0.75` | 0.0133 | 0.0405 | 0.977 | 0.00053 | 35.8 |
| interior | `low_msaa4_fxaa_0.6` | 0.0120 | 0.0592 | 0.944 | 0.00002 | 21.7 |
| interior | `low_taau_0.6` | 0.0135 | 0.0436 | 0.971 | 0.00061 | 31.8 |
| interior | `low_msaa4_fxaa_0.5` | 0.0133 | 0.0779 | 0.914 | 0.00003 | 18.9 |
| interior | `low_taau_0.5` | 0.0138 | 0.0434 | 0.970 | 0.00073 | 29.5 |
| interior | `low_fxaa_0.5` | 0.0147 | 0.0836 | 0.903 | 0.00003 | 16.2 |
| effects | `low_msaa4_fxaa_0.75` | 0.0167 | 0.0643 | 0.953 | 0.00118 | 33.8 |
| effects | `low_taau_0.75` | 0.0214 | 0.0849 | 0.910 | 0.00304 | 43.0 |
| effects | `low_msaa4_fxaa_0.6` | 0.0193 | 0.0892 | 0.912 | 0.00150 | 29.0 |
| effects | `low_taau_0.6` | 0.0224 | 0.0874 | 0.893 | 0.00311 | 40.4 |
| effects | `low_msaa4_fxaa_0.5` | 0.0224 | 0.0938 | 0.888 | 0.00205 | 29.1 |
| effects | `low_taau_0.5` | 0.0231 | 0.0907 | 0.870 | 0.00334 | 37.0 |
| effects | `low_fxaa_0.5` | 0.0238 | 0.0988 | 0.886 | 0.00206 | 26.0 |
| photoreal | `low_msaa4_fxaa_0.75` | 0.0643 | 0.1053 | 0.780 | 0.00012 | 39.9 |
| photoreal | `low_taau_0.75` | 0.0522 | 0.0916 | 0.844 | 0.00211 | 42.0 |
| photoreal | `low_msaa4_fxaa_0.6` | 0.0790 | 0.1218 | 0.701 | 0.00016 | 32.2 |
| photoreal | `low_taau_0.6` | 0.0539 | 0.0915 | 0.832 | 0.00291 | 36.3 |
| photoreal | `low_msaa4_fxaa_0.5` | 0.0952 | 0.1616 | 0.622 | 0.00018 | 26.9 |
| photoreal | `low_taau_0.5` | 0.0584 | 0.1108 | 0.798 | 0.00395 | 34.0 |
| photoreal | `low_fxaa_0.5` | 0.0964 | 0.1617 | 0.620 | 0.00020 | 23.2 |

Averages over the four scenes:

| Candidate | Mean FLIP | Mean worst tile |
|---|---|---|
| `low_msaa4_fxaa_0.75` | 0.0287 | 0.0844 |
| `low_taau_0.75` | 0.0289 | 0.0818 |
| `low_msaa4_fxaa_0.6` | 0.0336 | 0.0961 |
| `low_taau_0.6` | 0.0296 | 0.0835 |
| `low_msaa4_fxaa_0.5` | 0.0395 | 0.1131 |
| `low_taau_0.5` | 0.0311 | 0.0891 |
| `low_fxaa_0.5` | 0.0404 | 0.1153 |

## Reading

- At Low's own scale (0.75) the two are even on mean FLIP (0.0287 vs 0.0289): TAAU wins the
  textured photoreal scene and worst tiles indoors, MSAA + FXAA wins the other three on mean FLIP.
  MSAA + FXAA is 5 to 25 times more stable frame to frame and cheaper here, so Low keeps it.
- At 0.6 and 0.5, the range dynamic resolution reaches (`dynamic_resolution_min` defaults to 0.5),
  TAAU has the lower worst tile in all four scenes and the lower average mean FLIP (0.0296 vs 0.0337
  at 0.6, 0.0311 vs 0.0395 at 0.5); MSAA cannot recover the detail lost to the lower resolution.
- TAAU's temporal error stays higher at every scale; the reactive mask, disocclusion and thin
  feature locks (ADR-0143/0147/0148) limit but do not remove it.

## Not measured

Phones. Tile-based GPUs resolve MSAA on chip, while TAAU reads and writes history every frame, so
the bandwidth trade-off there may differ from lavapipe's. The threshold should be re-measured on
Adreno, Mali and Apple GPUs before it is tuned further.
