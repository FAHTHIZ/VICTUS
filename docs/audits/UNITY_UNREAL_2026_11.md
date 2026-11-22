# Unity and Unreal in November 2026: what moved and what it means for VICTUS

Dated **2026-11-22**. VICTUS revision: `main` at `11f4fac`. Requested by the owner: research Unity and Unreal Engine as deeply as possible and keep the documents current.

This audit **extends** two existing documents and does not replace them:

- [COMPETITOR_BENCHMARK.md](COMPETITOR_BENCHMARK.md): the per-workflow table (Godot 4.7, Unity 6.3 LTS, Unreal 5.8), refreshed 2026-11-20.
- [ENGINE_LANDSCAPE_AND_GAP_AUDIT.md](ENGINE_LANDSCAPE_AND_GAP_AUDIT.md): the numbered gap register (`GAP-01` to `GAP-37`), 2026-11-21.

Both compared against **stable** releases only. Since then, both vendors have published previews and roadmaps that change what "competitive" will mean during 2027. This document records those changes from primary sources, marks which are shipping, beta or only announced, and maps each one to the gap register.

Rules are the same as the rest of the repository: competitor claims come from vendor pages and are not reproduced; VICTUS claims come from `docs/STATUS.md`, `docs/KNOWN_LIMITATIONS.md` and the code at the revision above; nothing in VICTUS has been measured on real GPU hardware.

---

## 1. Release status on 2026-11-22

| Line | Status | Source |
|---|---|---|
| **Unreal Engine 5.8** | Latest stable release; the comparison baseline in COMPETITOR_BENCHMARK.md. | [5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes) |
| **Unreal Engine 5.9 Preview** | "MegaJam Preview" released 2026-11-09, Preview 2 notes updated 2026-11-19. Epic describes it as "the first Unreal Engine release built primarily with agentic workflows", with a shorter QA pass than usual, and plans the full 5.9 for 2027 with no date. | [5.9 Preview release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes), [Epic forum announcement via Patchbot](https://patchbot.io/games/unreal-engine/articles/2151-59-megajam-preview) |
| **Unreal Engine 6** | Announced 2026-06-22. Early Access targeted for the end of 2027, full release 12-18 months later. Merges UE5 and UEFN; new gameplay framework **Scene Graph** on **Verse**; Actors and Blueprints stay in early UE6 and are eventually deprecated with conversion tools. | [The road to UE6](https://www.unrealengine.com/news/the-road-to-ue-6) |
| **Unity 6.3 LTS** | Current LTS, supported until December 2027. The comparison baseline in COMPETITOR_BENCHMARK.md. | [Unity 6 releases](https://unity.com/releases/unity-6) |
| **Unity 6.6** | Current "Supported" update release. | [New in Unity 6.6](https://docs.unity.com/en-us/engine/6000.7/manual/whats-new/unity66) |
| **Unity 6.7 LTS** | In public beta ("Coming soon" on the releases page); Unity calls it the final release built on Mono. | [Unity 6 releases](https://unity.com/releases/unity-6), [Unity 6.7 Beta manual](https://docs.unity.com/en-us/engine/6000.7/manual), [CoreCLR update, October 2026](https://discussions.unity.com/t/coreclr-scripting-and-net-update-october-2026/1738338) |
| **Unity 7** | Announced at Unite Seoul (2026-07-21). Preview in December 2026, full release targeted for Q1 2027. CoreCLR, .NET 10 and C# 14; URP becomes the only render pipeline. Unity describes it as "a direct continuation of Unity 6" with no breaking changes. | [Unite Seoul 2026 keynote recap](https://unity.com/blog/unite-seoul-keynote-2026-recap), [The path to a single Render Pipeline](https://unity.com/topics/the-path-to-a-single-render-pipeline-in-unity-7) |

**Consequence for the benchmark:** COMPETITOR_BENCHMARK.md stays correct for stable releases. By mid-2027 its baselines will be Unity 6.7 LTS / Unity 7 and Unreal 5.9; the rows in section 3 below are the ones that will move first.

---

## 2. The five shifts that matter

### 2.1 Both editors now have an AI agent interface

- **Unreal 5.9 Preview:** an MCP server starts with the editor; agent toolsets cover Blueprints, editor actions, MVVM, Slate inspection, performance, files, scene capture, console, Texture Graph, widget animation and Play In Editor; a Toolset Registry limits which toolsets agents may use; the Unreal Assistant panel hosts external coding agents (Claude Code, Codex, Cursor, OpenCode) with graduated tool access (Ask, Read, Edit, Auto, Full); a Knowledge Base answers from live engine state or Markdown files. New Python and Blueprint libraries edit Blueprints, Chooser tables, StateTrees and Dataflow graphs from scripts ([5.9 Preview notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)).
- **Unreal 6:** plans to expose "a broad set of engine capabilities through the MCP protocol" so developers can choose their own model ([The road to UE6](https://www.unrealengine.com/news/the-road-to-ue-6)).
- **Unity:** a free **Unity CLI** and **Unity MCP server** for everyone without a subscription; MSBuild support so code compiles outside the Editor "enabling faster agentic workflows"; the CLI validated an asset and pushed content bundles to a running Nintendo Switch 2 in the keynote demo ([Unite Seoul recap](https://unity.com/blog/unite-seoul-keynote-2026-recap)).

**VICTUS today:** VICTUS itself is developed by coding agents (AGENTS.md), and its `victus` CLI already covers project creation, import, run, headless `simulate` and `serve`, `check`, `migrate`, `compare`, `export`, `pack`, `diagnose`, `bake-lighting`, `library`, `character-check`, `script-api` and a debug adapter (`tools/cli/main.cpp`). But there is **no protocol through which an agent can drive a running Studio**: inspect the scene, run a command, enter Play, take a screenshot, read the Problems panel. Editor plugins (ADR-0192) gained commands, but only native C ABI code can call them.

**New gap, GAP-38 (S2):** *No agent/automation interface to the Studio.* Fix direction: an opt-in local MCP server inside the Studio (localhost only, off by default, per-project allow-list in the spirit of Unreal's Toolset Registry), whose tools reuse the existing undoable command layer, the command palette and ADR-0192 editor commands, so every agent edit is one undo step. Start read-only (scene tree, Problems, screenshots, `victus check` output), then add edits. Treat this as a security-sensitive feature: it executes actions on the user's machine.

### 2.2 The web became a real target

- **Unity 6.6:** WebGPU is production-ready with full URP compatibility, compute shaders and indirect rendering ([Unite Seoul recap](https://unity.com/blog/unite-seoul-keynote-2026-recap)); 6.7 adds WebAssembly64 ([single render pipeline page](https://unity.com/topics/the-path-to-a-single-render-pipeline-in-unity-7)).
- **Unreal 5.9 Preview:** an experimental WebAssembly target (wasm64 only, heaps above 2 GB) with WebGPU rendering, progressive-web-app install, per-tier shader downloads, ASTC-first textures on phones, save games that survive reloads, readable startup errors, and Blueprint-only projects packaged for the web in Preview 2 ([5.9 Preview notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)).

**VICTUS today:** no web target (GAP-34). SDL3 supports Emscripten, and SDL_GPU's backend list does not include WebGPU, so a web port needs a new RHI backend or a WebGPU path outside SDL_GPU.

**Register change:** GAP-34 is split. **GAP-34a Web (S2, raised from part of a mixed row):** both leaders now treat a browser as a first-class way to let players try a game, which is exactly the "let people try the showcase" argument already in GAP-34. Fix direction: a feasibility ADR comparing (1) a WebGPU RHI backend behind the existing RHI interface, (2) WebGL2 via the GLES 3.0 path that already passes the GPU suite on Mesa, and (3) waiting for upstream SDL_GPU WebGPU work. Option (2) is likely the shortest path because the GLES fallback exists. **GAP-34b iOS, consoles, XR** keeps the old severity.

### 2.3 Dynamic global illumination reached phones

- **Unity 6.7 LTS:** **Surface Cache GI**, real-time indirect lighting for URP without baking, shown at 60 fps on a Samsung Galaxy S26; combined with SSR and GTAO, all three in URP ([Unite Seoul recap](https://unity.com/blog/unite-seoul-keynote-2026-recap)). It runs on any platform with compute shaders and uses hardware ray tracing where present through the UnifiedRayTracing API ([single render pipeline page](https://unity.com/topics/the-path-to-a-single-render-pipeline-in-unity-7)).
- **Unity 7.0** adds physical light units, auto-exposure and a dynamic sky to URP; 7.x in 2027 adds volumetric fog, shadow caching and ray-traced direct lighting ([same page](https://unity.com/topics/the-path-to-a-single-render-pipeline-in-unity-7)).

**VICTUS today:** baked lightmaps, probes and irradiance volumes; no dynamic GI for moving time of day (GAP-12); no SSR (GAP-16); SSAO, not GTAO. VICTUS already has a physically based atmosphere (`render/atmosphere.hpp`), auto exposure (`scene_renderer.hpp`) and volumetric fog (ADR-0098, ADR-0186), so on those items VICTUS is not behind Unity's 2027 URP plan on paper; it is behind on measured device evidence.

**Register change:** GAP-12 stays S2 but its "Leaders" cell must now cite a phone, not only Lumen: the bar is real-time indirect light at 60 fps on a 2026 flagship phone. The phone-first direction in GAP-12 (time-of-day probe keyframes, low-resolution probe updates) remains right for entry phones (Infinix Hot 10S class), which are far below a Galaxy S26. GAP-16 (SSR) moves from S3 to **S2**: Unity now ships SSR in its mobile-first pipeline.

### 2.4 Translucency sorting is being solved

- **Unreal 5.9 Preview:** Adaptive Voxel-Based Order Independent Transparency (AVBOIT) blends overlapping glass, smoke and particles correctly regardless of draw order, with Nanite and non-Nanite translucency ([5.9 Preview notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)).

**VICTUS today:** blended geometry draws after opaque geometry and decals, ordered far to near per batch, and alpha particles are sorted back to front on the GPU (`scene_renderer.cpp`). Intersecting or interleaved translucent surfaces can still sort wrongly; ADR-0187 fixed their TAA smearing, not their order.

**New gap, GAP-39 (S3):** *Order-dependent transparency.* Fix direction: weighted blended OIT (McGuire and Bavoil 2013) as an opt-in for particles and glass on Medium/High; it is one extra accumulation target and a resolve, cheap enough to measure on the GLES path before deciding. Not a showcase blocker.

### 2.5 Iteration speed and scripting runtimes are the headline in both camps

- **Unity:** Fast Enter Play Mode becomes the default for new projects from 6.6, with vendor figures of 2.7-4.7x faster Play Mode entry; CoreCLR reloads only what changed instead of a whole domain; a Fast Build Profile in 6.7 cuts shader build times by up to 90 %; PSO tracing and warm-up reduce runtime stutter ([Unite Seoul recap](https://unity.com/blog/unite-seoul-keynote-2026-recap)).
- **Unreal 6:** "cook times are expected to come down, iteration loops to get tighter"; Verse runs game code in transactions that can be rolled back and resimulated ([The road to UE6](https://www.unrealengine.com/news/the-road-to-ue-6)).

**VICTUS today:** Luau scripts and assets hot-reload; Play-in-editor uses a scene snapshot; ADR-0180's shared cooked cache cut a second 3D project from 24.4 s to 1.3 s. There is **no recorded Play-entry time, no shader-build time and no PSO/pipeline warm-up measurement** in STATUS.md.

**New gap, GAP-40 (S2):** *Iteration times are not measured.* Fix direction: add three numbers to `docs/PERFORMANCE_STRATEGY.md` and track them per release: Studio cold open of each template, Play entry on the 3D template, and first-run pipeline compilation time (cold and warm cache). Without them VICTUS cannot claim, or notice losing, its one structural advantage (a small engine that starts fast).

---

## 3. Feature-level changes by area

Rows list only what is new since COMPETITOR_BENCHMARK.md (2026-11-20). "Status" is the vendor's own label.

| Area | Unreal (5.9 Preview / UE6 plan) | Unity (6.6, 6.7 LTS beta, 7 plan) | VICTUS (recorded) | Register |
|---|---|---|---|---|
| Rendering | AVBOIT; Nanite culling split by instance complexity; Stochastic PCSS on directional VSMs; far shadows at their own update rate; experimental GPU wind for Nanite foliage ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | Single pipeline (URP) in 7.0; Built-in RP removed in 7.0 (supported until end 2028 in 6.7 LTS); HDRP deprecated in 7.0; AgX tonemapper; DLSS 4.5 and other upscalers; inline ray tracing in user shaders ([single pipeline](https://unity.com/topics/the-path-to-a-single-render-pipeline-in-unity-7)) | One renderer with quality tiers; TAAU; contact shadows (ADR-0183); dithered LOD cross-fade (ADR-0184); foliage on retained instance groups (ADR-0164) | GAP-39 new; GAP-16 raised; GAP-15 done |
| Mobile | Single-pass deferred lighting for all shading models on mobile; web mobile tiers use TAA instead of FXAA ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | On-tile post-processing on Metal and Vulkan; Tile-Only mode for predictable power; Neural Texture Compression on-device (vendor: over 50 % less texture memory) ([Seoul](https://unity.com/blog/unite-seoul-keynote-2026-recap)) | Low tier TAAU below 0.65 scale, measured on lavapipe only (ADR-0188); never run on a phone (GAP-02/03) | GAP-02/03 unchanged and still S1 |
| 2D | — | Physics Core 2D on Box2D v3, multithreaded; RenderSprite APIs draw thousands of sprites without GameObjects; four new 2D profilers ([Seoul](https://unity.com/blog/unite-seoul-keynote-2026-recap)); updated Box2D backend with faster Tilemap in 6.7 ([6.7 beta post](https://discussions.unity.com/t/6-7-lts-beta-sweepstakes-your-chance-to-win-a-gpu-or-unity-swag/1736728)) | Box2D v3.2.0 already (cmake/VictusDependencies.cmake); sprite batching; tiles not camera-culled; no 2D profiler view | GAP-19 unchanged; add a 2D section to the profiler as part of GAP-04 |
| Animation | IK Retargeter Proximity IK, Pelvis Volume and Limb Twist; Control Rig as an asset; UAF notifies; AI animation generation (AnimGen, AnimGenOffline in Sequencer) ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | — | Retargeting, IK, foot placement (ADR-0163), twist drivers (ADR-0182) | GAP-25: twist done; tooling still missing |
| AI / gameplay | StateTrees editable from Blueprint and Python; Mass Characters plugin; progressive navmesh building with a per-tick tile budget ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)); Verse transactional gameplay in UE6 ([UE6](https://www.unrealengine.com/news/the-road-to-ue-6)) | Graph Toolkit real-time debugging and a state-machine API in 6.7 ([Inven Global, Unite Seoul](https://www.invenglobal.com/articles/24003/unity-engine-7-changing-the-development-paradigm-and-the-roadmap-ahead)) | Behaviour Tree Editor with drag reparenting (ADR-0176, ADR-0191); tiled navmesh with incremental tile rebuilds (ADR-0168); simulation LOD (ADR-0165) | GAP-32 partly done (editor yes, EQS no) |
| UI and text | Widget components output depth and velocity; list/tile view orientation ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | UI Toolkit grid layout, z-index, gaps, gradients and integrated authoring; built-in Localization with Smart Strings ([6.7 beta post](https://discussions.unity.com/t/6-7-lts-beta-sweepstakes-your-chance-to-win-a-gpu-or-unity-swag/1736728)); text shaping with right-to-left support ([Inven Global](https://www.invenglobal.com/articles/24003/unity-engine-7-changing-the-development-paradigm-and-the-roadmap-ahead)) | UI Editor canvas (ADR-0193); `.vstrings` localization; **no text shaping, no RTL, no plural rules** (KNOWN_LIMITATIONS) | **GAP-41 new (S2)** |
| Networking | No networking items in the 5.9 Preview notes | Prediction for GameObject netcode ("GhostObjects") released ([Unity Discussions roadmap tag](https://discussions.unity.com/tag/roadmap/159607)); server-authoritative Cloud Behaviors in 6.7 ([Inven Global](https://www.invenglobal.com/articles/24003/unity-engine-7-changing-the-development-paradigm-and-the-roadmap-ahead)) | Authoritative server, prediction, encryption; loopback only | GAP-37 unchanged |
| Platforms | Experimental Wasm/WebGPU target ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | WebGPU production-ready in 6.6; WebAssembly64 in 6.7; DXC for DX12 in 6.6, Vulkan-on-Android and console DXC planned ([Seoul](https://unity.com/blog/unite-seoul-keynote-2026-recap)) | Vulkan, Metal (experimental), GLES 3.0; D3D12 unusable without DXIL (GAP-07) | GAP-34 split; GAP-07: DXC is now the industry route |
| Tools and automation | MCP server, Unreal Assistant, Knowledge Base, Unreal Canvas (experimental) | Free Unity CLI and MCP server; MSBuild; web dashboard and RBAC in closed beta ([Seoul](https://unity.com/blog/unite-seoul-keynote-2026-recap)) | `victus` CLI; Studio plugins (ADR-0192); no agent protocol | **GAP-38 new (S2)** |
| Documentation | Epic had AI review all 72 "Lighting the Environment" pages against 5.9 source: 671 inaccurate-text fixes, 162 additions, 76 removals ([5.9](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)) | — | Specs written 2026-11-16 to 2026-11-19 are 140-450 commits behind main (section 5) | **GAP-42 new (S3)** |

---

## 4. Gap register deltas

Apply these to [ENGINE_LANDSCAPE_AND_GAP_AUDIT.md](ENGINE_LANDSCAPE_AND_GAP_AUDIT.md) section 4 (done in the same commit as this audit).

| ID | Change | Reason |
|---|---|---|
| GAP-15 | open → **done** | Dithered LOD cross-fade shipped (ADR-0184). |
| GAP-16 | S3 → **S2** | Unity ships SSR in its mobile-first URP in 6.7. |
| GAP-32 | open → **partly** | Visual Behaviour Tree Editor (ADR-0176) with drag reparenting (ADR-0191); environment queries still missing. |
| GAP-34 | split into 34a (Web, S2) and 34b (iOS/console/XR) | Both leaders now treat the browser as a primary trial channel. |
| GAP-36 | open → **partly** | In-Studio Asset Store with licence-checked installs (ADR-0174); still no docs site, tutorials or community. |
| GAP-38 | **new, S2** | No agent/automation interface to the Studio. |
| GAP-39 | **new, S3** | Order-dependent transparency. |
| GAP-40 | **new, S2** | Iteration times (Studio open, Play entry, pipeline compile) not measured. |
| GAP-41 | **new, S2** | No text shaping, right-to-left layout or plural rules. |
| GAP-42 | **new, S3** | Specification documents drift behind the code; no automated check. |

---

## 5. What VICTUS should take, and what it should not

**Take:**

1. **Agent access through the undo system (GAP-38).** Both vendors route agents through the same editor actions people use. VICTUS's undoable command layer and ADR-0192 commands are the right seam; an MCP front end is small compared with what already exists.
2. **A browser build of the showcase (GAP-34a)** through the GLES path first. Both leaders now use the web to let people try games without installing anything.
3. **Measured iteration (GAP-40).** Unity's whole 2026 message is iteration speed. VICTUS should publish its own numbers before claiming anything.
4. **Text shaping (GAP-41).** HarfBuzz (MIT) plus SheenBidi or ICU's bidi for right-to-left; required before any release outside Latin, Cyrillic and Greek scripts, which matters for the owner's home market (Bengali needs shaping).

**Do not take:**

1. **Unreal's renderer ceiling.** Nanite, Lumen, AVBOIT at full quality and Virtual Shadow Maps target consoles and PCs. The phone-first rule in GAP-06 and GAP-18 still holds.
2. **Generative content inside the editor** (Unreal Canvas, Unity's AI generation). It depends on paid external services and raises licence questions the Asset Store work (ADR-0174) deliberately avoided. Revisit only after GAP-01 (a shipped game).
3. **A second gameplay language.** Unreal is moving to Verse and Unity to CoreCLR; both are multi-year migrations by large teams. Luau plus visual graphs is enough for VICTUS's audience.

**Observe:** Epic shipped 5.9 Preview "built primarily with agentic workflows" and still marked the web renderer a prototype to be refactored, gave it a shorter QA pass and published a known-issues list ([Epic forum announcement via Patchbot](https://patchbot.io/games/unreal-engine/articles/2151-59-megajam-preview)). That is the same trade-off VICTUS makes every day: agents produce breadth quickly; proof on hardware and shipped games still decides trust (GAP-01, GAP-02).

---

## 6. Documentation drift (GAP-42 evidence)

Commits on `main` since each document was last changed, measured at `11f4fac`:

| Document | Last changed | Commits behind |
|---|---|---|
| docs/PLATFORM_STRATEGY.md | 2026-11-16 | 447 |
| docs/RENDERING_SPEC.md | 2026-11-16 | 445 |
| docs/PRODUCTION_PIPELINE_SPEC.md | 2026-11-16 | 443 |
| docs/NETWORKING_SPEC.md | 2026-11-16 | 385 |
| docs/REFERENCE_PROJECTS.md | 2026-11-17 | 335 |
| docs/EDITOR_SPEC.md | 2026-11-18 | 220 |
| docs/DIAGNOSTICS.md | 2026-11-18 | 213 |
| docs/RELEASE_PROCESS.md | 2026-11-18 | 207 |
| docs/PERFORMANCE_STRATEGY.md | 2026-11-18 | 207 |
| docs/ARCHITECTURE.md | 2026-11-19 | 179 |
| docs/GETTING_STARTED.md | 2026-11-19 | 137 |

"Commits behind" is not the same as "wrong": a strategy document can stay valid. Each one is being re-read against the code; corrections land as separate `docs:` commits. Fix direction for the long term: a `tools/lint` check that every `ADR-NNNN` in TECHNOLOGY_DECISIONS.md with a user-visible change appears in CHANGELOG.md, and that every `engine/<module>` appears in the ARCHITECTURE module map (the changelog was eight entries behind and the module map was missing `traffic` and `library` on 2026-11-22).

## 7. Re-run conditions

Re-run this audit when any of these happen: Unity 6.7 LTS final release; Unity 7 Preview (December 2026) or release (Q1 2027 target); Unreal 5.9 full release (2027); Unreal 6 Early Access (end of 2027 target); or when a VICTUS change closes one of GAP-38 to GAP-42.
