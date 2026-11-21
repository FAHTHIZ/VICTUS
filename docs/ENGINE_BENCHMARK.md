# VICTUS against the industry's best engines

Researched 2026-11-21. This page records what makes the leading in-house and commercial engines
strong, where VICTUS stands against each strength today, and the order in which the gaps are
being closed. It is deliberately blunt: VICTUS is a young engine that has never been measured on
a real GPU or shipped a game, so "has the feature" never means "matches the shipped quality".
Implementation evidence for every VICTUS claim is in [STATUS.md](STATUS.md); maturity labels are
in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

## What the best engines do well

### Infinity Ward IW 8.0 (Call of Duty: Modern Warfare, 2019)

- A rebuilt PBR material system for photogrammetry content, hybrid tile-based streaming, PBR
  decal rendering, volumetric lighting from every light, a new GPU geometry pipeline, DXR ray
  traced shadows and spectral rendering for thermal and night-vision optics
  ([Activision](https://investor.activision.com/news-releases/news-release-details/call-duty-modern-warfare-introduces-ultimate-multiplayer),
  [Activision blog](https://blog.activision.com/call-of-duty/2019-06/Initial-Intel-Call-of-Duty-Modern-Warfares-game-engine-is-put-through-its-paces)).
- Digital Foundry singled out the volumetric lighting, material quality and multiple blended
  lighting bakes as the reasons it set a new benchmark
  ([Digital Foundry](https://www.digitalfoundry.net/articles/digitalfoundry-2019-cod-modern-warfare-delivers-series-most-advanced-visuals-yet),
  [PC Gamer](https://www.pcgamer.com/call-of-duty-modern-warfare-represents-a-new-benchmark-for-visual-fidelity/)).

### Rockstar RAGE (GTA V, Red Dead Redemption 2)

- Very large streamed worlds, crowd and traffic AI, weather and networking, with NaturalMotion's
  Euphoria motion synthesis on top of Bullet physics for characters that react physically
  ([Wikipedia](https://en.wikipedia.org/wiki/Rockstar_Advanced_Game_Engine)).
- RDR2 renders sky, clouds and fog as one participating medium: a froxel grid near the camera,
  ray marching beyond it, a cloud map and a sky-irradiance probe grid
  ([SIGGRAPH 2019 Advances](https://www.advances.realtimerendering.com/s2019/index.htm),
  [Eurogamer / Digital Foundry](https://www.eurogamer.net/digitalfoundry-2018-red-dead-redemption-2-tech-analysis)).

### id Tech 7 (DOOM Eternal)

- Fully forward clustered rendering with uber-shaders (~500 pipeline states), bindless
  resources, one geometry pool, compute-based draw merging and a job system with no main thread
  ([Simon Coenen's frame study](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy.html),
  [SIGGRAPH 2020 Advances](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf),
  [Doom Wiki](https://doomwiki.org/wiki/Id_Tech_7)).

### Unreal Engine 5

- Virtual shadow maps: a 16k virtual shadow map per light with 128² pages allocated only where
  the depth buffer needs them, cached across frames, and ray-marched soft shadows
  ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine?application_version=5.6)).
- Per-light screen-space Contact Shadows that trace from each pixel towards the light through the
  depth buffer for small-scale contact detail
  ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/contact-shadows-in-unreal-engine)).

### Frostbite, Decima and Bend Studio

- Frostbite's FrameGraph: a per-frame DAG of passes and transient resources with culling,
  memory aliasing and automatic barriers ([GDC Vault](https://www.gdcvault.com/play/1024612/)).
- Decima streams continuously on background threads ([Guerrilla, Horizon Zero Dawn](https://www.slideshare.net/slideshow/decima-engine-visibility-in-horizon-zero-dawn/82371923)).
- Bend Studio's screen-space sun shadows, first shipped in Days Gone, fill in contact detail the
  shadow maps miss ([Bend Studio](https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/)).

## Where VICTUS stands

| Strength | Best-in-class | VICTUS today |
|---|---|---|
| Frame graph | Frostbite FrameGraph | Render graph with dependency derivation, pass culling and transient texture aliasing (comparable design) |
| Forward clustered lighting | id Tech 7 | Forward+ clustered PBR (comparable design; no bindless, no draw merging) |
| Contact shadows | UE5, Days Gone | Sun contact shadows in the SSAO pass (ADR-0183); no per-light contact shadows yet |
| Shadow resolution | UE5 virtual shadow maps | Cascaded maps plus a local-light atlas; no virtual paging or caching |
| Volumetrics | RDR2, IW 8.0 | Froxel fog for sun and local lights, one flat cloud layer; no unified fog and cloud medium |
| Decals | IW 8.0 PBR decals | Projected box decals on opaque geometry in the forward pipeline (ADR-0064), colour only: no normal or roughness blending |
| Streaming | RAGE, Decima, IW 8.0 | World cells (`.vworld`); no texture tile streaming |
| Physical characters | RAGE + Euphoria | Powered ragdolls; no balance controller or motion synthesis |
| Thermal / night vision | IW 8.0 spectral rendering | Missing |
| Global illumination | Lumen, baked bakes | Lightmaps, irradiance volumes, probe sun occlusion; no dynamic GI beyond probes |
| Real-GPU validation | Every shipped engine | None: all evidence is on a software rasteriser |

## Gap order

Ordered by visible quality per unit of work, and by what everything else depends on:

1. Contact shadows (done for the sun: ADR-0183).
2. LOD dithered cross-fade, so LOD switches stop popping.
3. Volumetric fog noise and self-shadowing, then clouds in reflection probes and temporal
   reprojection of the cloud target.
4. Light cookies and rectangular area lights (LTC).
5. Thermal / night-vision post effect driven by a per-material heat value.
6. Virtual shadow-map paging for the sun.
7. Active ragdoll balance (physical animation that follows the animator under perturbation).
8. Hybrid texture tile streaming.

Real-GPU performance measurement stays the most important unknown and is outside what this
sandbox can provide.
