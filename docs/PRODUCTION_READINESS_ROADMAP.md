# VICTUS Production Readiness Roadmap

The single tracker for how far each subsystem is from being usable in a shipped game. It is a
summary, not the evidence: commands and measurements live in [STATUS.md](STATUS.md), per-subsystem
detail in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md), platform evidence in
[PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md), decisions in
[TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md) and the ordered work plan in
[../ROADMAP.md](../ROADMAP.md). Update this file in the same commit as any change that moves a
subsystem, closes a weakness or adds a blocker.

**Overall verdict (2026-11-21): not production-ready.** VICTUS is broad and heavily tested on
Linux with a software rasteriser, but no game has shipped on it, no rendering has run on a
physical GPU on any platform, Android has never run on a device, and GPU device-loss recovery
(player only) has been exercised with injected failures, never a real driver reset. No subsystem is labelled production-ready (definition below).

## Maturity labels

| Label | Meaning |
|---|---|
| Production-ready | Used by a shipped or externally developed game, validated on real hardware for every supported platform, stable public API. Nothing has this label. |
| Usable | End-to-end (runtime, serialization, Studio UI, scripting), automated tests, used by a template or sample. API may change. |
| Experimental | Works and is tested, but narrow coverage, unmeasured performance or missing workflow support. |
| Incomplete | Important parts that games need are missing. |
| Missing | Not implemented. |

## Production blockers

Items that keep the whole engine from production use, regardless of feature work. Ordered by impact.

| # | Blocker | Why it blocks | Owner input needed | State |
|---|---|---|---|---|
| B1 | No physical-GPU validation | Every frame-time and image result comes from Mesa lavapipe or CI's virtual Apple GPU; real driver bugs, performance and thermals are unknown | Windows PC with GPU, Apple Silicon Mac, Android phones | [!] blocked on hardware |
| B2 | Android never run on a device | APK builds in CI only; lifecycle, input, GLES fallback, frame rate and thermals unverified | At least one Vulkan and one GLES-only phone | [!] blocked on hardware |
| B3 | GPU device loss is fatal | SDL_GPU 3.4 has no device-lost signal; loss is inferred from 30 failed operations in a row | A real driver reset/GPU removal to test against | [~] 2026-11-21 (ADR-0157): the player recreates the device and every GPU object and keeps the game running (up to 3 times); verified with injected failures only. The editor still autosaves and exits |
| B4 | No shipped or external project | Workflows are verified by tests, not by an external developer building a game | External testers | [ ] |
| B5 | Windows rendering untested | CI Windows runners have no GPU; D3D12 unusable (no DXIL) | Windows GPU machine | [!] blocked on hardware |
| B6 | Packaging lacks installers, OS code signing, patches, cross-export (pack compression and pack signing done, ADR-0154) | Required for store distribution | Signing certificates for release | [~] |
| B7 | Scripting API was unversioned | Game code could break between engine versions | — | [x] 2026-11-21 (ADR-0155): versioned, frozen snapshots enforced in CI; argument lists/return shapes not covered, no deprecation warnings yet |

## Subsystems

| Subsystem | Maturity | Biggest remaining weaknesses | Next step |
|---|---|---|---|
| Core (jobs, memory, JSON, fs, logging, profiler) | Usable | No allocation call stacks; one profiler per process | Allocation call-stack capture in tracking builds |
| Platform (SDL3) | Usable on Linux desktop | Windows/macOS never used interactively; Android untested on device | Hardware (B1, B2) |
| RHI (SDL_GPU Vulkan/Metal, GLES 3.0 fallback) | Usable on Vulkan; Experimental Metal/GLES | No D3D12 (no DXIL), no GPU timestamps, device loss recovered in the player only (injected failures) | Editor device recovery; test a real driver reset (B3) |
| Shader pipeline (GLSL → SPIR-V/MSL/ESSL) | Usable | FP16 variant only for the PBR BRDF; surface shaders fragment-only | — |
| 3D renderer | Usable on software Vulkan | Unmeasured on real GPUs; no opaque SSR on GLES, foliage impostors or dynamic GI beyond probes | Rendering gaps in ROADMAP §2 |
| Temporal AA / upscaling | Usable | Low-tier TAAU threshold measured on lavapipe only, not phones; no auto-generated reactive mask | Phone measurements |
| Baked lighting | Usable | CPU-only bake, analytic sky only, no auto re-bake, no adaptive probe density | Bake performance on large scenes |
| 2D renderer | Usable | No 2D shadows/normal maps; tiles not camera-culled | 2D normal maps |
| Materials / surface shaders / material graphs | Usable | Four parameters, no custom textures/uniforms, fragment-only | Custom texture slots |
| Scene, prefabs, serialization | Usable | Nested prefabs flattened; no variants; formats all at v1 (migrators unexercised) | Prefab variants |
| ECS | Usable | — | — |
| Asset pipeline | Usable | Running background imports (ADR-0189) are not interruptible; references miss paths built at run time; V.A.U.L.T. budgets cover textures/models only | V.A.U.L.T. CPU budgets and mesh/audio residency |
| World streaming | Experimental | No cross-cell references, data layers, server cell ownership, Studio world view | Cross-cell references |
| Physics (Jolt 3D, Box2D 2D) | Usable | No convex decomposition; whole heightfield rebuild per edit; ragdoll root does not follow while simulating; no get-up clips or pose matching | Convex decomposition |
| Animation | Usable | IK limits are one cone per chain (no per-joint/twist); foot placement uses one ray per foot along world up; no sync markers, GPU morphs; layer masks read-only in Studio | Sync markers, then GPU morph targets |
| Navigation (Recast/Detour) | Usable | Single tile; whole-mesh rebuilds; no off-mesh links | Tiled navmesh |
| Gameplay AI | Usable | Visual tree editor done (ADR-0176); no environment queries | Environment queries |
| Gameplay framework / no-code behaviours | Usable | No Studio inspector for attributes and inventories; level travel is not seamless (no loading screens) | Attribute inspector |
| Scripting (Luau) | Usable | Snapshot does not cover signatures or return shapes; no deprecation warnings; no attach to running player; interpreter only | Generated Luau type definitions (signatures in the snapshot, editor autocomplete) |
| Visual scripting | Experimental | No breakpoints, user functions or comments | Breakpoints |
| Audio (miniaudio) | Usable | Simple comb reverb; no per-source effects, HRTF, ducking, profiler | Ducking and per-source effects |
| Particles / VFX | Experimental | CPU emitters do not collide; GPU emitters 3D only; no mesh particles | Mesh particles |
| Terrain and foliage | Experimental | Per-instance CPU culling of foliage; no impostors, wind, holes | Impostors for far trees |
| Sequencer | Experimental | No multi-key select or video export (curve editor and property tracks done, ADR-0190) | Multi-key editing |
| In-game UI | Usable | No UI editor canvas, rich text, sliders, text input | Sliders and text input |
| Localization | Usable | No shaping/RTL, plurals or translation editor | Plural rules |
| Networking (ENet) | Experimental | Loopback only; no NAT traversal, host migration, lag compensation | Lag-compensated hits |
| Save games / save history | Usable | No compression, cloud saves, restore UI | Generation restore UI |
| Plugins / modding | Usable for trusted code | No sandbox, hot reload, editor loading, signing | Editor plugin loading |
| VICTUS Studio (editor) | Usable on Linux | Never run on Windows/macOS by a person; no UI canvas (BT editor ADR-0176 and sequencer curve editor ADR-0190 done) | ROADMAP §1 Studio maturity |
| Debugging, profiling, perceptual compare | Usable for CPU | No GPU timings; compare is LDR-only; goldens are lavapipe-only | — |
| Packaging and export | Usable on host platform | B6: installers, OS code signing, delta patches, cross-export; trusted pack key comes from `export.json` (not tamper-proof without code signing) | Delta patch packs signed with the game's key |
| Android | Incomplete | B2 | Hardware |
| iOS, Web, XR, consoles | Missing | — | Feasibility studies |
| Build, CI, static analysis | Usable | clang-tidy not a required check (owner setting); reference-scene gate was red on main for six commits before 2026-11-21 (see process weaknesses) | Keep main green |

## Process weaknesses

- **Pack v2 (8649027) pushed with a use-after-free in its own test**, red on sanitizers, macOS and
  Windows. Rule: slices touching `engine/core` run the asan preset's core tests before push.
- **Commits recorded as verified while CI was red.** From d1f895b to 2096152 (2026-11-20/21) every
  push failed `victus_reference_scenes` in the Linux Release lane while STATUS.md listed only the
  suites that passed locally. Rule from now on: a slice that touches rendering runs
  `ctest -R victus_reference_scenes` locally under `VICTUS_REQUIRE_GPU=1`, and the next slice does
  not start while main's CI is red.

## Completed improvements (production-readiness work)

Newest first. Feature history before this file existed is in [../CHANGELOG.md](../CHANGELOG.md).

| Date | Change | Evidence |
|---|---|---|
| 2026-11-21 | Weighted bone masks on animation layers (ADR-0159), phase-synced layers (ADR-0160), FABRIK cone joint limits (ADR-0161) and a Layers section in Studio's Animation window (state, weight slider, mask weights) | `victus_animation_tests` weighted-mask case (Spine 0.25 -> 2.0 of 1..5, inheritance, additive scaling, round-trip, errors); editor Animation window test with a weighted layer |
| 2026-11-21 | Foot placement and foot locking (ADR-0163): ground probes, pelvis drop, slope tilt, planted feet that do not slide, one-step humanoid setup, Studio "Feet on the ground" card and IK overlay | `victus_animation_tests` foot placement cases (steps, holes, slopes, locking, setup, serialization, physics probe); editor test "studio feet" |
| 2026-11-21 | Ragdoll get-up (ADR-0158): root moved under the hips and turned for a get-up clip, back/front detection, smoothstep blend from the lying pose to the animation; Luau `entity:ragdoll_get_up` (script API 1.1), Studio "Get Up" | `victus_physics_tests` back and front falls: root within 5 cm of hips on the ground, first frame within 6 cm of the lying pose, final pose exact; script and editor tests |
| 2026-11-21 | GPU device-loss recovery in the player (ADR-0157, B3 partial): new device, GPU context, asset cache contents and scene view rebuilt on the same window; scene, scripts, physics, audio and network survive; `--device-recoveries` (default 3) | `victus_device_loss`: all three injected failure kinds recover in the 2D template, final frame vs clean run FLIP 0.002-0.010, SSIM >= 0.991; 3D template FLIP 0.003, SSIM 0.998 |
| 2026-11-21 | Script time budgets charge thread CPU time, not wall time (ADR-0156): a descheduled or blocked script thread no longer fails healthy scripts (flaky macOS Debug CI `dungeon_crawler` "exceeded its time budget of 250 ms"); busy loops are still stopped | `victus_script_tests` 126/126, new budget case |
| 2026-11-21 | Versioned script API 1.0 (ADR-0155, closes B7): `kScriptApiVersion`, `victus.project` `script_api`, hosts refuse incompatible scripts, `victus check` error, frozen snapshot in `engine/script/api/` checked by `victus_script_tests`, `victus script-api`; Entity members moved to one property table | STATUS.md 2026-11-21 (script API) |
| 2026-11-21 | Fixed red CI after ADR-0154: `pack_tests.cpp` read a `PackEntry*` from a destroyed temporary `Pack` (ASan heap-use-after-free; random codec value on macOS/Windows). Test now keeps the pack alive | `victus_core_tests` 75/75 under ASan+UBSan locally |
| 2026-11-21 | `.vpak` v2 (ADR-0154): per-entry zstd (3D template pack 6.50 MB to 2.43 MB), BLAKE2b-256 digests, Ed25519 signatures enforced by the player, `victus pack keygen/verify/list` | STATUS.md 2026-11-21 |
| 2026-11-21 | FP16 PBR keeps Lambert and environment BRDF in FP32 (ADR-0153): FP16-vs-FP32 error halved on all four reference scenes, fixing the red CI reference gate | STATUS.md 2026-11-21 |
| 2026-11-21 | Created this tracker | — |
