# Reference scenes (VICTUS sample)

Three deterministic scenes for perceptual rendering checks (ADR-0104): fixed camera, no scripts,
no physics, no particles. A fixed-frame run renders the same pixels every time on the same
driver; one slowly rotating object per scene gives frame sequences a temporal signal.

| Scene | What it exercises |
| --- | --- |
| `scenes/exterior.vscene` | Shadowed low sun, physical sky, height + volumetric fog, a dielectric and a metal sphere row with a roughness ramp (0.05 to 0.95), thin posts and rails (aliasing, shadow detail), stairs and a wall (AO, contact shadows), an emissive bar (bloom), a rotating cube |
| `scenes/effects.vscene` | The surface and effect passes: a raised pool of water with refraction and waves, a multiply and an alpha decal, a seeded CPU spark fountain with trails, a GPU mist emitter, a reflection probe around a chrome sphere |
| `scenes/interior.vscene` | A closed room lit only by local lights: a shadowed point lamp, a shadowed spot casting the box and cylinder onto the back wall, an emissive panel, a fog volume (light shafts), glossy floor, dark corners, a rotating metal cylinder |

```sh
victus run samples/reference_scenes --scene scenes/interior.vscene --quality ultra \
    --frames 30 --capture-dir out/interior --capture-every 15 --width 320 --height 180 \
    --render-report out/interior.path.json
victus compare samples/reference_scenes/golden/lavapipe_ultra_320x180/interior out/interior --heatmap out/heat.png
```

## Goldens

`golden/lavapipe_ultra_320x180/` holds software-renderer correctness images: ultra quality,
320x180, frames 15 and 30, rendered with Mesa lavapipe (llvmpipe, LLVM 21.1.8). They are not a
high-end reference; a capture on capable physical GPU hardware is still to be recorded.

The `victus_reference_scenes` CTest (label `gpu`, needs `VICTUS_REQUIRE_GPU=1`) renders both
scenes, compares them with `--max-flip 0.005 --max-tile-flip 0.01 --min-tile-ssim 0.99
--max-temporal 0.005`, checks the key rendering-path features are active, and logs low-vs-ultra
FLIP without gating on it. On a different rasteriser (another Mesa, a hardware GPU), regenerate:

```sh
SDL_VIDEO_DRIVER=offscreen VICTUS_REQUIRE_GPU=1 VICTUS_UPDATE_GOLDENS=1 ctest --preset dev -R victus_reference_scenes
```

The scenes themselves are generated; edit the script, not the JSON, then regenerate goldens:

```sh
python3 samples/reference_scenes/tools/generate_reference.py samples/reference_scenes
```
