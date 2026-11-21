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
physical GPU on any platform, Android has never run on a device, and GPU device loss ends the
session. No subsystem is labelled production-ready (definition below).

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
| B3 | GPU device loss is fatal | SDL_GPU 3.4 has no device-lost signal or recreate path; the player exits with code 3 | — (upstream SDL or own backend) | [ ] |
| B4 | No shipped or external project | Workflows are verified by tests, not by an external developer building a game | External testers | [ ] |
| B5 | Windows rendering untested | CI Windows runners have no GPU; D3D12 unusable (no DXIL) | Windows GPU machine | [!] blocked on hardware |
| B6 | Packaging lacks signing, compression, installers, cross-export | Required for store distribution | Signing certificates for release | [ ] |
| B7 | Scripting API is unversioned | Game code can break between engine versions | — | [ ] |

## Subsystems

| Subsystem | Maturity | Biggest remaining weaknesses | Next step |
|---|---|---|---|
| Core (jobs, memory, JSON, fs, logging, profiler) | Usable | No allocation call stacks; one profiler per process | Allocation call-stack capture in tracking builds |
| Platform (SDL3) | Usable on Linux desktop | Windows/macOS never used interactively; Android untested on device | Hardware (B1, B2) |
| RHI (SDL_GPU Vulkan/Metal, GLES 3.0 fallback) | Usable on Vulkan; Experimental Metal/GLES | No D3D12 (no DXIL), no GPU timestamps, device loss not recoverable | Device-loss recovery design (B3) |
| Shader pipeline (GLSL → SPIR-V/MSL/ESSL) | Usable | FP16 variant only for the PBR BRDF; surface shaders fragment-only | — |
| 3D renderer | Usable on software Vulkan | Unmeasured on real GPUs; no area lights/cookies, LOD cross-fade, SSR/planar reflections, FFT ocean, dynamic GI beyond probes | Rendering gaps in ROADMAP §2 |
| Temporal AA / upscaling | Usable | No per-material responsive flag for blended meshes; TAAU on Low unmeasured on phones | Responsive flag |
| Baked lighting | Usable | CPU-only bake, analytic sky only, no auto re-bake, no adaptive probe density | Bake performance on large scenes |
| 2D renderer | Usable | No 2D shadows/normal maps; tiles not camera-culled | 2D normal maps |
| Materials / surface shaders / material graphs | Usable | Four parameters, no custom textures/uniforms, fragment-only | Custom texture slots |
| Scene, prefabs, serialization | Usable | Nested prefabs flattened; no variants; formats all at v1 (migrators unexercised) | Prefab variants |
| ECS | Usable | — | — |
| Asset pipeline | Usable | No import queue UI or dependency inspector; glTF cameras/lights not imported; V.A.U.L.T. budgets cover textures/models only | V.A.U.L.T. CPU budgets and mesh/audio residency |
| World streaming | Experimental | No cross-cell references, data layers, server cell ownership, Studio world view | Cross-cell references |
| Physics (Jolt 3D, Box2D 2D) | Usable | No convex decomposition; whole heightfield rebuild per edit; ragdoll get-up missing | Convex decomposition |
| Animation | Usable | No weighted bone masks/layer sync, IK poles/limits, foot locking, GPU morphs | Weighted bone masks |
| Navigation (Recast/Detour) | Usable | Single tile; whole-mesh rebuilds; no off-mesh links | Tiled navmesh |
| Gameplay AI | Usable | No visual tree editor, no environment queries | Visual BT editor |
| Gameplay framework / no-code behaviours | Usable | Attributes/inventories not replicated, no Studio inspector for them; no session/level lifecycle | Session lifecycle |
| Scripting (Luau) | Usable | Unversioned API (B7); no attach to running player; interpreter only | API versioning |
| Visual scripting | Experimental | No breakpoints, user functions or comments | Breakpoints |
| Audio (miniaudio) | Usable | Simple comb reverb; no per-source effects, HRTF, ducking, profiler | Ducking and per-source effects |
| Particles / VFX | Experimental | CPU emitters do not collide; GPU emitters 3D only; no mesh particles | Mesh particles |
| Terrain and foliage | Experimental | Per-instance CPU culling of foliage; no impostors, wind, holes | Impostors for far trees |
| Sequencer | Experimental | No curve editor, multi-key select, video export | Curve editor |
| In-game UI | Usable | No UI editor canvas, rich text, sliders, text input | Sliders and text input |
| Localization | Usable | No shaping/RTL, plurals or translation editor | Plural rules |
| Networking (ENet) | Experimental | Loopback only; no NAT traversal, host migration, lag compensation | Lag-compensated hits |
| Save games / save history | Usable | No compression, cloud saves, restore UI | Generation restore UI |
| Plugins / modding | Usable for trusted code | No sandbox, hot reload, editor loading, signing | Editor plugin loading |
| VICTUS Studio (editor) | Usable on Linux | Never run on Windows/macOS by a person; no dependency inspector, UI canvas, BT editor, curve editor | ROADMAP §1 Studio maturity |
| Debugging, profiling, perceptual compare | Usable for CPU | No GPU timings; compare is LDR-only; goldens are lavapipe-only | — |
| Packaging and export | Usable on host platform | B6 | Compressed `.vpak` |
| Android | Incomplete | B2 | Hardware |
| iOS, Web, XR, consoles | Missing | — | Feasibility studies |
| Build, CI, static analysis | Usable | clang-tidy not a required check (owner setting); reference-scene gate was red on main for six commits before 2026-11-21 (see process weaknesses) | Keep main green |

## Process weaknesses

- **Commits recorded as verified while CI was red.** From d1f895b to 2096152 (2026-11-20/21) every
  push failed `victus_reference_scenes` in the Linux Release lane while STATUS.md listed only the
  suites that passed locally. Rule from now on: a slice that touches rendering runs
  `ctest -R victus_reference_scenes` locally under `VICTUS_REQUIRE_GPU=1`, and the next slice does
  not start while main's CI is red.

## Completed improvements (production-readiness work)

Newest first. Feature history before this file existed is in [../CHANGELOG.md](../CHANGELOG.md).

| Date | Change | Evidence |
|---|---|---|
| 2026-11-21 | FP16 PBR keeps Lambert and environment BRDF in FP32 (ADR-0153): FP16-vs-FP32 error halved on all four reference scenes, fixing the red CI reference gate | STATUS.md 2026-11-21 |
| 2026-11-21 | Created this tracker | — |
