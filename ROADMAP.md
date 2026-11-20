# VICTUS Roadmap

The plan for what is still missing, in priority order. Finished work is not listed here: it is described in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) (what each subsystem can do today), recorded with commands and results in [docs/STATUS.md](docs/STATUS.md), decided in [docs/TECHNOLOGY_DECISIONS.md](docs/TECHNOLOGY_DECISIONS.md) and summarised in [CHANGELOG.md](CHANGELOG.md). An item is removed from this file when its acceptance criteria are met and the evidence is in STATUS.md.

Legend: **[~]** in progress · **[ ]** not started · **[!]** blocked on something only the owner or hardware can provide.

## Direction

VICTUS is a general-purpose engine for complete 2D and 3D games, simulation and multiplayer. Two goals shape every priority:

1. **Anyone can make a game.** A person who has never made a game must be able to create a project, add a player, enemies, pickups, a goal and a HUD, play it and export it, without writing code. Code (Luau, visual graphs, C++ plugins) is the path to go further, never the entry fee.
2. **Perceptual-equivalence rendering.** A lower-cost device may use different algorithms from a high-end PC, but the final image should keep the same visual intent within tested perceptual tolerances. Reduce the cost of producing the image before reducing its visible quality. Brief: [docs/handoff/VICTUS_PERCEPTUAL_EQUIVALENCE_RENDERING.md](docs/handoff/VICTUS_PERCEPTUAL_EQUIVALENCE_RENDERING.md).

## Current maturity

Broad and tested, **not production-ready**: no game has shipped with VICTUS, rendering has only run on software Vulkan (lavapipe) and CI's virtual Apple GPU, Windows rendering and Android devices are untested, and GPU device loss ends the session instead of recovering. Per-subsystem maturity: [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md); platform evidence: [docs/PLATFORM_SUPPORT.md](docs/PLATFORM_SUPPORT.md).

## Priorities

### 0. Photoreal on low-end hardware (owner direction, 2026-11-20)

The owner's target: images that read as photoreal on hardware as weak as entry-level phones. Research, audit and the ordered plan: [docs/audits/PHOTOREAL_LOW_END_AUDIT.md](docs/audits/PHOTOREAL_LOW_END_AUDIT.md). This track comes before every other priority; new no-code gameplay features wait until its first items land.

- [x] **Baked lighting v1.** xatlas lightmap UVs, a CPU path-traced baker (sun, sky, point/spot lights, multi-bounce diffuse), a per-scene lightmap atlas, renderer sampling, `victus bake-lighting`, Studio *Bake Lighting*. **Accept when:** a template scene bakes from the CLI and Studio, renders lightmapped on every tier, and a GPU test shows bounce light and contact darkening that the unbaked scene lacks. Done: baker, `.vlightmap` format and renderer path (ADR-0122); Static Lighting flag, per-scene bake file, `victus bake-lighting`, Studio Assets > Bake Lighting, export packing (ADR-0125).
- [x] **Mobile bandwidth quick wins.** R11G11B10 scene colour on Low/Medium and mobile, no depth pre-pass without SSAO, FXAA folded into tonemap on Low. Done (ADR-0126): packed HDR by default on phones and Low/Medium, `victus run --hdr`, `hdr_precision` in the render path report; the depth pre-pass was already SSAO/occlusion-only; the FXAA fold was rejected (more ALU and HDR reads than it saves). Stores into the packed targets round stochastically, so they are unbiased and TAA history does not drift (ADR-0129).
- [x] **Baked lighting v2.** Directional (L1 SH) lightmaps, sun shadowmask, baked light probes for dynamic objects, lightmapped templates. Directional lightmaps done (ADR-0130: dominant direction + rebalancing, Unity's encoding; normal maps shade baked light); baked sun shadowmask past the realtime shadow distance done (ADR-0131); baked light probes for dynamic objects done (ADR-0133: SH9 grid traced with the lightmap, invalid probes inside geometry dilated from neighbours, `.vlightmap` v3); lightmapped templates done (ADR-0134: `victus new` and Studio bake the 3D templates on creation; moving entities are never marked Static Lighting); congruent lightmapped copies share one unwrap and batch again (ADR-0135: per-instance atlas uv offset, Unity's lightmapScaleOffset); probe sun occlusion done (ADR-0136: baked sun visibility per probe shadows dynamic objects past the realtime shadow distance, Unity's probe occlusion; `.vlightmap` v4).
- [x] **FP16 shading** behind a capability check, held to `victus compare` goldens. Done (ADR-0137): `pbr.frag#fp16` (explicit `float16_t` BRDF, Filament's 0.089 roughness clamp) used where Vulkan `shaderFloat16` is enabled, on by default on phones and Low/Medium, `victus run --fp16`, `shading_precision` in the render path report; the reference scenes with FP16 forced on are gated against the FP32 goldens. Metal and the speed-up on real phones are still to verify.
- [x] **Device classes.** Tier chosen per GPU and capabilities, with project overrides. Done (ADR-0127): `rendering.quality` / `rendering.mobile_quality` = `"auto"` (the phone default) picks the tier from the GPU family (Mali, Adreno, PowerVR, Xclipse, Apple, Intel integrated, software), capped by RAM and compute support; explicit tiers still win.
- [ ] **Photoreal reference scene** with CC0 scanned materials, the reference for perceptual equivalence across tiers.
- [ ] **Mobile temporal upscaling.** Arm ASR (MIT) evaluated against the built-in TAA upscaler.
- [ ] **OpenGL ES 3 fallback** (ADR first) for the ~7 % of Android handhelds without Vulkan.

### 1. Creator experience (no-code game making)

- [~] **VICTUS Studio maturity** (brief: [docs/handoff/STUDIO_UX_AND_WORKSPACE_MATURITY.md](docs/handoff/STUDIO_UX_AND_WORKSPACE_MATURITY.md)). Done so far: design tokens, typography and icons, start screen with validation and samples, layouts, Problems panel, Export Game, diagnostics, Hierarchy/Inspector/Console/Assets polish, drag-and-drop import, interface size, Getting Started checklist and lessons, Help panel, undo history, open panels and layout remembered between sessions, user-named layouts. Remaining: a dependency inspector and import queue, a visual UI editor canvas, a visual behaviour-tree editor, a curve editor for the sequencer, plugin loading in the editor. **Accept when:** a first-time user completes the Getting Started path, builds and exports a game in a moderated test without help.

### 2. Engine depth

- [~] **A.E.G.I.S. — adaptive geometry and significance budgets** (briefs: [geometry](docs/handoff/VICTUS_AEGIS_ADAPTIVE_GEOMETRY.md), [camera-responsive fidelity](docs/handoff/VICTUS_CAMERA_RESPONSIVE_VISUAL_FIDELITY.md)). One coordinator assigns significance and bounded budgets to rendering detail, animation evaluation, non-authoritative simulation, foliage and per-client replication, with visual, simulation and replication LOD kept as separate outputs. Visibility never stops authoritative gameplay. **Accept when:** stable, view-correct geometry and image detail; approved subsystems stay inside declared budgets without changing authoritative outcomes; Studio shows per-entity decisions; hardware measurements are published.
- [~] **V.A.U.L.T. — runtime resource manager.** Done: asynchronous texture/model loading with upload budgets (ADR-0087), GPU residency budgets with LRU eviction (ADR-0088), texture-mip streaming by footprint (ADR-0089, ADR-0092). Remaining: CPU budgets per resource class, prefetch hints, mesh/animation/audio residency, packaged-archive range reads, a Studio residency view. **Accept when:** a stress project streams mixed assets without frame-blocking I/O, stays inside CPU/GPU budgets under sustained travel and behaves the same packaged as in the editor.
- [~] **World streaming.** Done: `.vworld` cells around streaming sources (ADR-0090). Remaining: cross-cell references, data layers, incremental cell save state, server-side cell ownership and interest, a Studio world view. **Accept when:** a large-world reference project moves through cells without stalls, inside a memory ceiling, preserving cross-cell links and save state, with a headless server and clients.
- [~] **A.R.C. — physics-driven characters.** Done: articulated ragdolls with kinematic/powered/passive modes and pose tracking (ADR-0091), Studio ragdoll bodies. Remaining: contextual hit reactions, balance and stepping, bracing, get-up, simulation tiers by importance. **Accept when:** a reference character moves between animation, partial and full active ragdoll, reacts to disturbances, recovers balance where plausible and returns to control without snapping, authored and debugged in Studio.
- [~] **Starter character fidelity (Orion and Nova).** Done: sculpted SDF mannequins replace the Vanguard robots (ADR-0121). Remaining: baked normal and ambient-occlusion maps from the high-resolution field (the GLBs carry no textures yet), a closer face pass for Nova, per-cascade shadow LOD so a character's shadow does not cost three times its main-view triangles (receiver-aware cascade culling, ADR-0124, cut template-3d from 34.5k to 23.2k shadow triangles against 11.6k main; per-cascade shadow LOD, ADR-0132, cut it to 19.1k with no visible change). README screenshots refreshed (edb3409) and the Robot Arena sample moved to Orion and Nova (879fd8e), so no old robot ships. **Accept when:** both characters hold up in a close-up render review and the template-3d shadow triangles drop without a visible shadow change.
- [~] **Animation depth.** Remaining: weighted bone masks and layer sync, chain IK poles and joint limits, foot locking and contact IK, humanoid retarget profiles and a bone-map editor, root motion on chosen axes, GPU morph targets; optional motion matching measured against the controller baseline.
- [~] **Destruction.** Done: Voronoi fracture, impulse breaking and support graphs (ADR-0093). Remaining: cooked fracture assets, clustered breakup, debris budgets and pooling, save/restore and replicated break events, Studio fracture preview.
- [~] **Audio depth.** Done: voice budgets and virtual voices (ADR-0083), geometry occlusion, reverb zones and underwater filtering (ADR-0096). Remaining: per-source effects, ducking, HRTF evaluation, streaming through V.A.U.L.T., a Studio voice profiler.
- [ ] **Rendering gaps.** Area lights and light cookies, LOD cross-fade, planar/screen-space reflections of scene objects on water, an FFT ocean, 2D normal maps and shadows, impostors and wind for foliage, GPU timestamps once SDL_GPU exposes them.
- [ ] **Gameplay framework gaps.** Game session and level lifecycle, replicated attributes and inventories.

### 3. Reliability and production

- [~] **Untrusted-input robustness.** Mutation tests cover 30 formats. Remaining: coverage-guided fuzzing in a nightly lane for the highest-risk parsers (glTF, `.vpak`, network packets).
- [~] **Reliability and recovery.** Done: durable verified writes with generations (ADR-0082), format migrators (ADR-0100), editor safe mode (ADR-0101). Remaining: save-game and cooked-asset migrations, an editor UI for restoring generations, GPU device recreation if SDL_GPU can support it.
- [ ] **Packaging.** Cross-export, code signing, installers and patches; compressed and signed `.vpak` archives.
- [ ] **Collaboration.** Source-control-friendly scene diffs and merges, conflict handling.
- [ ] **Modding.** Sandboxed, permissioned plugins and signed mod packages; plugin hot reload.
- [~] **Static analysis gate.** clang-tidy analyses every engine/editor/tools/apps source with zero diagnostics. Remaining: making it a required status check is a branch-protection setting only the owner can change.

### 4. Platforms and proof

- [!] **Hardware validation.** Needs a Windows PC with a GPU, an Apple Silicon Mac and at least one Android phone. **Accept when:** the editor and every template run on each, with frame-time captures in STATUS.md and the platform matrix updated.
- [ ] **Reference projects.** Finish the 2D (dungeon crawler, platformer) and 3D (arena) games with title flow and audio mix; add a simulation project with large populations, simulation LOD and headless mode; build all of them from clean checkouts in CI.
- [ ] **iOS, Web, XR.** Feasibility studies first; consoles only with platform-holder access.
- [ ] **Competitive maturity** (brief: [docs/handoff/COMPETITIVE_MATURITY_GAP_CLOSURE.md](docs/handoff/COMPETITIVE_MATURITY_GAP_CLOSURE.md); current comparison: [docs/audits/COMPETITOR_BENCHMARK.md](docs/audits/COMPETITOR_BENCHMARK.md)): evidence-based comparison with Godot, Unity and Unreal by workflow, side-by-side measurements once hardware exists.
- [ ] **External usability testing** and stable release criteria for 1.0.

## Working rules

Dependency order across workstreams: V.A.U.L.T. before world streaming; animation depth before advanced A.R.C.; A.E.G.I.S. consumes V.A.U.L.T. and world-streaming signals; destruction and audio use V.A.U.L.T. and A.E.G.I.S. budgets. Avoid parallel implementations of the same ownership boundary.

A task is done only when the code is on `main`, tests pass, errors and fallbacks are handled, platform and license limits are documented, performance claims have evidence, and the commit SHA with the verification commands is in docs/STATUS.md. See [AGENTS.md](AGENTS.md) for the full engineering contract.
