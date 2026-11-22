# Engine landscape and gap audit

Dated **2026-11-21**. VICTUS revision: `main` at `2c38903`. Requested by the owner: study what a game engine is and why it matters, compare VICTUS with the leading engines, and flag every place where VICTUS is behind or not yet good enough, ordered so the gaps can be closed.

This document extends, and does not replace, [COMPETITOR_BENCHMARK.md](COMPETITOR_BENCHMARK.md) (the workflow-by-workflow table for Godot, Unity and Unreal, 2026-11-18/20). That table stays the per-workflow classification. This audit adds:

1. the fundamentals (what an engine has to provide and why studios depend on one);
2. a wider field (O3DE, Bevy, Defold, Flax, Stride, CRYENGINE and in-house engines, not only the big three);
3. a numbered register of **every gap found** (`GAP-xx`), with the leaders' documented capability, VICTUS's recorded state, severity and the fix direction;
4. the gaps that matter for the owner's showcase direction of 2026-11-21: a living, simulated, stylised-realism city in the spirit of *The Matrix Awakens*, running on an entry-level phone (Infinix Hot 10S class).

Rules used here, the same as the rest of the repo: competitor claims come from vendor documentation and are not reproduced; VICTUS claims come from `docs/STATUS.md`, `docs/KNOWN_LIMITATIONS.md` and the code at the revision above; nothing has been measured on real GPU hardware (only Mesa lavapipe, Mesa GLES and CI's virtual Apple GPU), so every performance row is **unmeasured** unless stated.

---

## 1. What a game engine is, and why it matters

A game engine is not a renderer. Jason Gregory's *Game Engine Architecture* (3rd ed., 2018) splits it into a runtime (platform layer, core systems, resource manager, game loop and real-time simulation, rendering, animation, collision and physics, audio, gameplay foundations, networking) **and** the tools and asset pipeline that feed it ([table of contents](https://www.oreilly.com/library/view/game-engine-architecture/9781351974271/05-9781351974271_contents.xhtml)). Engines differ by genre because each genre stresses a different part of that stack.

Why studios depend on them:

- **Cost.** One shared runtime, editor and pipeline amortised over many games. *The Matrix Awakens* city was built by a small core team because procedural rules generated it, from road sizes to sidewalk debris ([Epic](https://www.unrealengine.com/en-US/spotlights/behind-the-scenes-on-the-matrix-awakens-an-unreal-engine-5-experience)); the team rebuilt the city weekly, 52 times ([IGN](https://www.ign.com/articles/matrix-awakens-unreal-engine-5-teaser)). Tools, not runtime features, made that possible.
- **Platforms.** Shipping to PC, phones, consoles and the web is mostly platform plumbing, certification and device testing that an engine absorbs once.
- **Talent.** Hiring follows engines. In GDC's 2025 survey of over 3,000 developers, Unity and Unreal were tied at 32 % each as primary engine, 13 % used proprietary engines and 4 % Godot ([GDC 2025 summary](https://www.gamesmarket.global/industry-survey-latest-trends-and-insights-from-the-gdc-2025-state-of-the-game-industry-report-6c5cef296d946f9d85f6692b09bef4e1/)). By released PC games, Unity holds about 49 %, Unreal about 20 % and Godot 7.1 % (2025), up from 0.9 % in 2020 ([GameDiscoverCo](https://newsletter.gamediscover.co/p/hows-pc-game-engine-usage-changing)).
- **Proof.** An engine is trusted because games shipped on it. That trust is what VICTUS has least of (GAP-01).

What this means for VICTUS: a feature counts as competitive only when it is reachable through tools (editor, pipeline, export) and proven on target hardware. A runtime capability with no tool or device evidence is half a feature.

## 2. The field (current stable releases, 2026-11-21)

| Engine | Licence / model | Positioning (documented) | Notes relevant to VICTUS |
|---|---|---|---|
| **Unreal Engine 5.8** (5.9 Preview since 2026-11-09; UE6 Early Access targeted end of 2027, see [UNITY_UNREAL_2026_11](UNITY_UNREAL_2026_11.md)) | Source-available, royalty | High-end 3D. MegaLights production-ready in 5.8; Lumen Lite in beta (about twice as fast as full Lumen); mesh-based terrain with Nanite and virtual texturing; manual editing of PCG output ([5.8 notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes)). Mass Entity for data-oriented simulation of crowds and traffic ([Mass overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine?application_version=5.0)); World Partition data layers with per-layer HLODs ([data layers](https://dev.epicgames.com/documentation/unreal-engine/world-partition-data-layers-in-unreal-engine)). Android defaults to OpenGL ES 3.2, Vulkan for selected devices ([rendering features reference](https://dev.epicgames.com/documentation/unreal-engine/rendering-features-reference?lang=en-US)). | The reference for the city showcase (City Sample: Mass AI crowds and traffic, density and visualisation toggles, [City Sample](https://dev.epicgames.com/documentation/en-us/unreal-engine/city-sample-project-unreal-engine-demonstration)). Its top-end features target consoles and PCs, not entry phones. |
| **Unity 6.3 LTS** (6.6 update stream; 6.7 LTS in beta; Unity 7 Preview December 2026, release targeted Q1 2027) | Proprietary, seats | Broadest platform list; URP for mobile. GPU Resident Drawer turns GameObjects into BatchRendererGroup instancing automatically ([manual](https://docs.unity3d.com/6/Documentation/Manual/urp/reduce-rendering-work-on-cpu.html)); Adaptive Probe Volumes place probes automatically and light per pixel with streaming ([APV](https://docs.unity3d.com/6/Documentation/Manual/urp/probevolumes-concept.html)); STP software upscaler (compute; not on GLES) ([STP](https://docs.unity.com/en-us/engine/6000.6/manual/cameras/urp/change-resolution-scale-urp/stp-upscaler)); Entities (DOTS) archetype/chunk ECS ([DOTS tips](https://learn.unity.com/article/c-and-dots-specific-performance-tips)). | Closest model for "beautiful on weak phones": URP + baked lighting + automatic instancing. |
| **Godot 4.7** | MIT | Forward+, Mobile and Compatibility (OpenGL) renderers ([features](https://docs.godotengine.org/en/latest/about/list_of_features.html)); 4.7 adds HDR output, `AreaLight3D` rectangular area lights, live text-shader preview, an Asset Store and standalone Android export ([4.7 release](https://godotengine.org/releases/4.7/)). | The open-source benchmark; its growth (0.9 % to 7.1 % of releases) shows a free engine can win users with tools and community. |
| **O3DE 25.05** | Apache-2.0 / MIT | Atom renderer on DX12/Vulkan/Metal with multi-GPU and ray-tracing work; Track View overhaul ([25.05 notes](https://docs.o3de.org/docs/release-notes/archive/2505-0-release-notes/)). | Shows that breadth without users does not create adoption. |
| **Bevy 0.18** | MIT/Apache-2.0 | Rust ECS-first engine; procedural atmosphere with occlusion, experimental Solari ray-traced renderer, variable fonts, gamepad UI navigation ([0.18](https://bevy.org/news/bevy-0-18/)). No editor yet. | Closest architectural peer (ECS, code-first); VICTUS already has an editor Bevy lacks. |
| **Defold 1.13** | Defold licence (free) | Tiny, fast 2D-first engine: empty Android release build 2.56 MB vs Unity 31.75 MB and Godot 25.29 MB (vendor figures); "renders tens of thousands of game objects" ([product page](https://defold.com/product/)). | Sets the bar for build size and startup on low-end phones (GAP-31). |
| **Flax 1.13** | Source-available, royalty | C#/C++ engine with a complete editor; 1.13 is current ([releases](https://github.com/FlaxEngine/FlaxEngine/releases)). | Small team, complete editor; a realistic target for VICTUS Studio. |
| **Stride 4.4** | MIT | C#/.NET 10 engine; 4.3 brought Bepu physics, Vulkan compute, custom assets ([4.3 notes](https://doc.stride3d.net/en/release-notes/4.3.html)); 4.4.3 is current ([4.4.3](https://github.com/stride3d/stride/releases/tag/releases/4.4.3)). | |
| **CRYENGINE 5.7 LTS** | Source on request | Long-term-support release; source via private repository on request ([support](https://www.cryengine.com/support/view/general)). | Shows the risk of a technically strong engine with stalled tools and community. |
| **In-house engines** (id Tech, Decima, Frostbite, RE Engine and others) | Proprietary | 13 % of surveyed developers (GDC 2025). Built for a studio's genres and platforms. | VICTUS today is effectively an in-house engine with one game (Special Ops) as its consumer. |

## 3. Where VICTUS actually stands

Strengths with evidence (do not lose these while closing gaps):

- **Breadth with tests.** About 131 k lines of engine C++, 19 k editor, 49 k test lines; 160+ ADRs; test suites also run under ASan+UBSan and TSan presets ([STATUS](../STATUS.md)).
- **Low-end-first rendering discipline.** Per-tier quality with perceptual goldens (`victus compare`), packed HDR on phones, FP16 BRDF, baked directional lightmaps, shadowmask and probes, an FSR2-style temporal upscaler, a GLES 3.0 fallback that passes the GPU suite on Mesa (ADR-0122-0148).
- **Determinism and replay.** Capture/replay with divergence reports; reproducible cooks; signed `.vpak` v2 (ADR-0154).
- **A real consumer.** The Special Ops game (Project-) runs a full mission on VICTUS; it found real engine/content issues (for example cutscene cameras needing streaming sources, fixed in content with the existing StreamingSource API, game commit `fd695da`).

The honest summary: VICTUS is **broad but unproven**. It has no shipped game, has never rendered on a real GPU, has never been installed on a phone, and its tools are behind every engine in the table. The gaps below are ordered by how much they block the owner's goals: (1) a showcase that hooks people, (2) on an entry-level phone, (3) an engine others could use.

## 4. Gap register

Severity: **S1** blocks the showcase or any credibility claim · **S2** major competitive gap · **S3** important but can wait · **S4** nice to have.
Status: **open** · **partly** (some of it exists) · **blocked** (needs hardware, access or an owner decision).
"Leaders" cites what the competitors document; "VICTUS" cites recorded state.

### A. Proof, hardware and measurement

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-01 | **No shipped game / external user** | Thousands of shipped titles (GameDiscoverCo data) | None; Special Ops is internal and unfinished | S1 | open | Finish one short showcase and release it; collect external feedback |
| GAP-02 | **Never rendered on a real GPU** | Device-class render paths (UE mobile docs) | lavapipe, Mesa GLES, CI virtual Apple GPU only | S1 | blocked (hardware) | Record captures on the owner's Hot 10S; keep `docs/PLATFORM_SUPPORT.md` explicit |
| GAP-03 | **Android APK never installed by VICTUS's own process** | Standalone Android export in Godot 4.7; Defold deploys without Android Studio | APK builds in CI; the game's APKs were sideloaded by the owner, but no frame-time capture exists | S1 | blocked (device) | Device-run checklist + on-device stats overlay export (ready before the device run) |
| GAP-04 | **No GPU timings** | Godot, Unity, Unreal all document GPU profiling | SDL_GPU has no timestamp queries | S1 | open | ADR: backend extension for Vulkan/GLES timestamp queries behind the RHI, or fence-based per-pass CPU timing as a fallback |
| GAP-05 | **No empty-project size / startup baseline** | Defold publishes 2.56 MB empty Android build; Unity 31.75 MB; Godot 25.29 MB | Not measured | S2 | open | Measure empty-template APK size and cold start; add to `docs/PERFORMANCE_STRATEGY.md` |

### B. Architecture ceilings

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-06 | **RHI capability ceiling from SDL_GPU**: no bindless, no hardware ray tracing, no mesh shaders, no timestamps | UE5 (Nanite, HW RT, SM6), Unity HDRP, O3DE Atom (RT) | SDL GPU maintainers state they will not add bindless, and that hardware ray tracing is unlikely soon ([SDL bug-report guide](https://wiki.libsdl.org/SDL3/HowToReportBugs/raw)) | S2 (S4 for phones) | open | Keep SDL_GPU for phones (where these features barely exist); document a seam for a native Vulkan backend only when a feature that needs it is chosen. Do not chase RT on entry phones |
| GAP-07 | **D3D12 unusable** (no DXIL) | All three ship D3D12 | Vulkan on Windows only | S3 | open | DXIL via shadercross/DXC once a Windows GPU is available for testing |
| GAP-08 | **CPU-driven batching every frame** (instance lists rebuilt per frame; foliage culled and LOD-selected per instance on the CPU) | Unity GPU Resident Drawer (persistent BRG instancing); Unreal ISM/HISM and Nanite | `scene_renderer.cpp` rebuilds batches per frame; foliage per-instance CPU culling (KNOWN_LIMITATIONS) | **S1 for the city** | partly (ADR-0162 retained groups: renderer CPU 6.83 → 0.43 ms per frame for 16,384 copies on lavapipe; ADR-0164 foliage on cached groups: extraction 1.67 → 0.07 ms per frame for 25,872 copies, cluster-granular distance edge; world statics and compute culling remain) | Persistent GPU instance buffers per static batch, dirty-range updates, compute frustum/HZB culling with indirect draws (compute exists on Vulkan; GLES path keeps CPU culling) |
| GAP-09 | **ECS has no scheduled phases or simulation LOD** | Unreal Mass: fragments, stateless processors grouped into tick phases ([Mass overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine?application_version=5.0)); Unity Entities | Archetype SoA ECS with parallel queries and command buffers; ADR-0165 adds after/before ordering, per-system intervals and simulation LOD tiers (distance to sources, 1/2/4/8-frame intervals, staggered, exact accumulated time) consumed by AI trees, animators and built-in movers; no parallel phase graph, physics and scripts do not follow LOD | **S1 for the city** | partly | A.E.G.I.S. significance feeds a processor scheduler: phases, dependencies, LOD-tiered tick rates, off-screen agents keep simulating at low rate |

### C. Rendering

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-10 | **No crowd rendering at scale** (vertex animation textures / GPU-animated impostors) | Common technique: animation baked into textures and played per instance on the GPU ([three-vat](https://github.com/MikeFernandez-Pro/three-vat)); City Sample crowds via Mass | GPU skinning per entity; morph targets on CPU | **S1 for the city** | open | Bake clips to a bone-matrix texture ("rig-encoded" VAT), instanced draw with per-instance clip/time; LOD to impostor cards far away |
| GAP-11 | **No HLOD / impostors** for distant buildings and trees | World Partition HLOD per data layer (UE docs) | Mesh LODs only; no far-field proxies | **S1 for the city** | open | Per-cell merged proxy mesh + baked atlas generated at cook time; octahedral impostors for trees |
| GAP-12 | **No dynamic GI** beyond probe volumes | Lumen, Lumen Lite, Godot SDFGI/VoxelGI, Unity APV (baked, per pixel) | Baked lightmaps/probes; irradiance volumes; nothing for moving time of day | S2 | partly | For phones: time-of-day probe sets (bake N keyframes, blend), DDGI-style low-res probe update on Medium/High only |
| GAP-13 | **No area lights or light cookies** | Godot 4.7 `AreaLight3D`; Unreal rect lights | Missing (ROADMAP rendering gaps) | S3 | open | LTC rectangle lights on High; baked-only area lights for lightmaps |
| GAP-14 | **Emissive-driven night lighting** (windows and lamps without real lights) | City lit only by sun, sky and emissives in Matrix Awakens ([Variety](https://variety.com/2021/digital/news/matrix-awakens-epic-games-unreal-engine-5-demo-1235129348/)) | Emissive materials exist; no baked emissive contribution, no cheap fake-light decals | S2 for the city | open | Bake emissive into lightmaps; projected light-pool decals for street lamps; window interior-mapping shader |
| GAP-15 | **No LOD cross-fade** | Unity LOD cross-fade, Unreal dithered LOD transitions | Dithered cross-fade over `lod_fade_frames` (default 8) with hysteresis (ADR-0184); opaque draws with a motion id only, both levels drawn during the fade, no geomorph | S3 | done | Geomorphing for terrain-like meshes if popping is reported on device |
| GAP-16 | **Water: no scene reflections, no FFT ocean** | SSR/planar in all three; Unity adds SSR to mobile-first URP in 6.7 LTS ([UNITY_UNREAL_2026_11](UNITY_UNREAL_2026_11.md)) | Sky/environment reflections only | S2 (raised 2026-11-22) | open | Screen-space reflections on Medium/High; FFT ocean later |
| GAP-17 | **Custom shaders limited**: four parameters, no custom textures | Shader Graph, UE materials, Godot shaders (vertex+fragment) | `.vshader` `surface()` per pixel and `vertex()` in forward and shadow passes (ADR-0170); named parameters with hints and up to four custom textures in both stages (ADR-0171) | S2 | partial | More than four parameter slots; vertex functions in prepass/motion vectors; bounds growth |
| GAP-18 | **No virtualised geometry** | Nanite | Missing | S4 for phones | open | Not pursued for entry phones; HLOD + impostors (GAP-11) is the phone answer |
| GAP-19 | **2D: no shadows or normal maps; tiles not camera-culled** | Godot 2D lights with normal maps and shadows | Missing | S3 | open | As ROADMAP |

### D. Large worlds and procedural content

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-20 | **No procedural world generation framework** | Unreal PCG (manual editing of output in 5.8); Houdini city rules in Matrix Awakens | Scatter for foliage only; Special Ops generates its valley with game-side Python | **S1 for the city** | open | `victus` procedural graph: road graph → blocks → lots → building kits → props, deterministic seeds, output to `.vworld` cells; Studio preview |
| GAP-21 | **World streaming lacks data layers, cross-cell references, HLOD and a world view** | World Partition data layers drive quests and variations at runtime | Cells around streaming sources (ADR-0090) | S2 | partly | Data layers first (needed for day/night and event variants), then HLOD (GAP-11) |
| GAP-22 | **No traffic/lane graph** | ZoneGraph + Mass traffic in City Sample | Lane graph traffic with signals and IDM agents (ADR-0167); Recast navmesh single tile for pedestrians, crowd agents measured at 128 agents 0.87-1.16 ms (STATUS slice 16) | **S1 for the city** | partial | Lane graph, split-phase signals and IDM vehicle agents done (ADR-0167, `engine/traffic`, `TrafficNetwork` component, 2,000 agents 0.42 ms/frame). Remaining: lane changes, protected turns, pedestrians and crossings, routing to destinations, Studio road editing, lane graph from the procedural city (GAP-20) |
| GAP-23 | **Navmesh single tile** | Tiled navmesh in UE/Unity/Godot | Tiled navmesh (ADR-0168): automatic above 1 M voxel columns or by `tile_size`, tiles cooked in parallel, a change re-cooks only the tiles it overlaps (200 m level: 36 tiles, 256 ms full, 19 ms one-crate rebuild vs 688 ms single tile) | S2 | partial | Off-mesh links; build tiles per streaming cell |

### E. Animation, physics, audio

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-24 | **No vehicle physics** | Godot vehicles; Chaos vehicles | Wheeled vehicles on Jolt `VehicleConstraint` (ADR-0166): `Vehicle`/`VehicleWheel` components, engine, automatic gearbox, per-axle limited-slip differentials, anti-roll bars, keyboard/gamepad driving, Luau `set_vehicle_input`/`vehicle_state`, Studio Make Vehicle card | **S1 for the city** | partial | Remaining: tracked vehicles and motorcycles (Jolt has both controllers), vehicle audio, networked prediction, traffic on the lane graph (GAP-22) |
| GAP-25 | **Animation tooling** (no Control Rig, bone-map editor, foot IK, root motion axes, GPU morphs) | Control Rig, IK Retargeter, MetaHuman Animator | Runtime layers/masks/IK exist; foot placement and foot locking done (ADR-0163); tools missing (ADR-0159-0161 added weighted masks and joint limits) | S2 | partly | GPU morphs and sync markers next; editor tools after |
| GAP-26 | **No soft bodies / cloth** | Chaos cloth; Godot soft bodies | Missing | S3 | open | Jolt soft bodies for flags/awnings (cheap city detail) |
| GAP-27 | **Audio: no HRTF, ducking, per-source effects; never heard on a phone** | MetaSounds; Godot buses/effects | Buses, spatialisation, occlusion, reverb zones | S2 | partly | City ambience needs many quiet emitters: virtual-voice budget exists; add ducking and per-zone ambience beds (ties to VICTUS Library and contextual audio) |

### F. Tools and workflow

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-28 | **Editor not extensible** (no editor plugins) | Godot editor plugins, Unity packages, UE editor modules | Runtime C ABI plugins; the Studio loads editor-target plugins with undoable commands and a Plugins panel (ADR-0192); no plugin panels/inspectors | S2 | partial | As COMPETITOR_BENCHMARK |
| GAP-29 | **No UI editor canvas, rich text, sliders, text input** | UMG, UI Toolkit, Godot Control nodes; Flax rich text wrap | Runtime UI with sliders, toggles, text fields, progress bars and `<b>`/`<color>` rich text (ADR-0169); Studio UI Editor canvas with anchor presets, preview sizes, drag move/resize, grid snap and undo (ADR-0193) | S2 | done | Multi-select and alignment tools, style editor (tracked in KNOWN_LIMITATIONS) |
| GAP-30 | **No FBX import; glTF cameras/lights ignored** | Unreal Interchange, Unity FBX, Godot FBX2glTF | glTF with `KHR_lights_punctual` lights and cameras imported, instantiated and exported (ADR-0172); FBX (binary/ASCII) and OBJ/MTL import through ufbx with meshes, materials, textures, skins, blend shapes, baked animation, lights and cameras (ADR-0173) | S3 | done | separate roughness/metalness texture packing, bump-to-normal conversion, animated light/camera properties (documented limits) |
| GAP-31 | **Build size and startup unknown** (see GAP-05) | Defold 2.56 MB empty build | Unknown; the Special Ops build carries a ~601 MB content pack (Project- BUILD_AND_TEST_LOG), so engine size is hidden | S2 | open | Measure; strip unused modules per project like Defold's App Manifest |
| GAP-32 | **No visual behaviour-tree editor, no environment queries** | StateTree, EQS, Behavior Trees; StateTrees scriptable from Python in UE 5.9 Preview | Behaviour Tree Editor with palette, live debugging and save-reload (ADR-0176), drag-and-drop reparenting (ADR-0191); no environment queries, no multi-select | S3 | partly | Environment queries (scored candidate points) for cover and spawn selection |
| GAP-33 | **No collaboration (scene merge)** | Text scene formats + merge tools | Versioned JSON; no merge tool | S3 | open | |

### G. Platforms, ecosystem, release

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-34a | **No Web target** | Unity WebGPU production-ready in 6.6, WebAssembly64 in 6.7; Unreal 5.9 Preview experimental wasm64 + WebGPU target; Godot web export | Missing; SDL_GPU has no WebGPU backend | S2 | open | Feasibility ADR: WebGL2 through the existing GLES 3.0 path vs a WebGPU RHI backend; the browser is the cheapest way to let people try a showcase ([UNITY_UNREAL_2026_11](UNITY_UNREAL_2026_11.md) section 2.2) |
| GAP-34b | **No iOS, consoles, XR** | All three ship iOS | Missing | S2 | open / blocked (console access, Apple hardware) | iOS after the Android device run (GAP-03) |
| GAP-35 | **No release ever tagged, no LTS** | Unity two-year LTS; UE hotfixes; Godot maintenance branches | Process documented, never run | S2 | open | Tag 0.1 with the showcase |
| GAP-36 | **No community, docs site, asset store, tutorials** | Godot Asset Store (4.7), Fab, Unity Asset Store | In-Studio Asset Store over open providers with licence checks and credits (ADR-0174); otherwise repo docs only | S2 | partly | Public docs site generated from `docs/`; a showcase devlog |
| GAP-37 | **Networking on loopback only** | Production netcode in all three | Authoritative server, prediction, encryption, loopback tests | S3 | open | Not needed for the showcase |

### H. Added 2026-11-22 from the Unity/Unreal update

Evidence and fix directions are in [UNITY_UNREAL_2026_11.md](UNITY_UNREAL_2026_11.md).

| ID | Gap | Leaders | VICTUS today | Sev. | Status | Fix direction |
|---|---|---|---|---|---|---|
| GAP-38 | **No agent/automation interface to the Studio** | Unreal 5.9 Preview: MCP server with the editor, toolset registry, Unreal Assistant; Unity: free CLI and MCP server | `victus` CLI for headless work; editor commands only through native plugins (ADR-0192) | S2 | open | Opt-in localhost MCP server in the Studio over the undoable command layer; read-only tools first |
| GAP-39 | **Order-dependent transparency** | Unreal 5.9 Preview AVBOIT | Blended batches far to near; alpha particles GPU-sorted | S3 | open | Weighted blended OIT as an opt-in on Medium/High, measured on GLES first |
| GAP-40 | **Iteration times not measured** | Unity Fast Enter Play Mode default from 6.6, Fast Build Profile in 6.7 | Shared cooked cache measured (ADR-0180); no Studio-open, Play-entry or pipeline-compile numbers | S2 | open | Add the three numbers to PERFORMANCE_STRATEGY.md and track them per release |
| GAP-41 | **No text shaping, right-to-left layout or plural rules** | Unity 6.7 text shaping with RTL, built-in Localization and Smart Strings | `.vstrings` tables with placeholders; Latin, Cyrillic and Greek fonts | S2 | open | HarfBuzz + a bidi library behind `gameui`; Bengali and Arabic test strings |
| GAP-42 | **Specification documents drift behind the code** | Epic AI-audited 72 lighting pages against 5.9 source | Specs 140-450 commits behind main on 2026-11-22 | S3 | partly (manual refresh under way) | `tools/lint` check: ADRs in CHANGELOG, modules in the ARCHITECTURE map |

## 5. The showcase-critical path

For the owner's chosen showcase (a living stylised city, simulated, on an entry phone), the **S1 gaps form one dependency chain**. Closing them is the work plan:

1. **GAP-08 persistent GPU instancing + GPU culling.** Without it, thousands of buildings, props, cars and people cost CPU per instance per frame on a phone with a weak CPU.
2. **GAP-09 simulation scheduler with LOD.** Thousands of agents need phases and tiered tick rates; off-screen agents keep living at a low rate (the Matrix Awakens "whether you are looking or not" property, [Epic](https://www.unrealengine.com/en-US/blog/introducing-the-matrix-awakens-an-unreal-engine-5-experience)).
3. **GAP-10 crowd rendering (rig-encoded VAT + impostors).**
4. **GAP-20/22 procedural city + lane graph.** Generates the city, the traffic network and the streaming cells in one deterministic pass.
5. **GAP-24 wheeled vehicles** (Jolt) for the player car; traffic uses cheap kinematic lane following.
6. **GAP-11 HLOD/impostors** for the skyline.
7. **GAP-14 emissive night lighting** for the day/night toggle.
8. **GAP-04 GPU timings + GAP-02/03 device captures**, so every claim in the showcase is measured on the Hot 10S.

Each step lands as an engine feature with tests and a sample, not as game-side code (the AGENTS.md rule against hard-coded showcase behaviour).

## 6. Coordination with other work

- Another session is active on `main` (ADR-0155 to 0161 and `docs/SHOWCASE_GAME_BLUEPRINT.md` landed on 2026-11-21). This audit adds files and appends to ROADMAP/STATUS only; it does not edit that session's ADRs or code.
- `docs/SHOWCASE_GAME_BLUEPRINT.md` proposes *The Hollow Current* (third-person exploration adventure). On 2026-11-21 the owner chose a simulation-heavy *Matrix Awakens*-style showcase instead, with stylised realism (not photorealism) and assets made in Blender. Both blueprints share the S1 engine work above except GAP-20/22/24; the choice of game is the owner's and is recorded here, not decided by this audit.
- Priority 0 in ROADMAP ("photoreal on low-end hardware") predates the owner's 2026-11-21 move to stylised realism. Its finished items (baked lighting, packed HDR, FP16, upscaler) all still serve the new direction; no item needs to be reverted.

## 7. Re-run conditions

Refresh when a competitor ships a major or LTS release, when VICTUS records its first real-device capture, or when an S1 gap closes. Change a row only with evidence linked from `docs/STATUS.md`.
