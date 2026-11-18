# Subsystem maturity and known limitations

Assessed 2026-11-18 by reading the code and running the full test suite (Slices 1-78). The labels are deliberately conservative:

- **Production-ready**: no subsystem has this label yet. It requires a shipped or externally used game, validation on real hardware for every supported platform, and stable public APIs. None of these exist yet.
- **Usable**: implemented end to end (runtime, serialization, editor UI where relevant, scripting access), covered by automated tests, and used by a template or sample game. Expect API changes.
- **Experimental**: works and is tested, but has narrow coverage, unmeasured performance, or lacks the editor or workflow support needed for real projects.
- **Incomplete**: part of the system exists; important parts that games need are missing.
- **Missing**: not implemented.

Evidence for every item is in [STATUS.md](STATUS.md); platform coverage is in [PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md).

| Subsystem | Maturity | What works | Main limitations |
|---|---|---|---|
| Core (jobs, memory, JSON, logging, fs, profiler zones) | Usable | Tested under ASan/UBSan/TSan; benchmarks | No allocation call stacks; one profiler per process |
| Platform (SDL3 windows, input, lifecycle) | Usable on desktop Linux | Keyboard, mouse, gamepad, touch, clipboard, suspend/resume events | Windows/macOS windows never used interactively; Android not run on a device |
| RHI on SDL_GPU | Usable on Vulkan, Experimental on Metal | Resources, pipelines, readback, compute, indirect draws | D3D12 unusable (no DXIL); device loss is detected and ends the session cleanly, but the device is not recreated; no GPU timestamps |
| 3D rendering | Usable (desktop, software rasteriser) | Forward+ clustered PBR, CSM + local light shadows, SSAO, IBL, sky, reflection probes, irradiance volumes, bloom, TAA/upscaling, motion vectors, occlusion culling, LODs, decals, fog, quality tiers | Performance never measured on a real GPU; no area lights, light cookies, volumetrics, dynamic GI beyond probe volumes, water |
| 2D rendering | Usable | Sprite batching, sprite sheets, tilemaps with autotiling, pixel-art sampling, 2D lights | No 2D shadows/normal maps; tiles not camera-culled; grid sprite sheets only |
| Materials and shaders | Usable | `.vmat` with instances, `.vshader` surface shaders, material graphs | Fragment stage only; four parameters, no custom textures/uniforms |
| Scene and serialization | Usable | Versioned JSON, unknown components preserved, prefabs with overrides, scene transitions | Nested prefabs flattened; no variants; no additive/streamed loading |
| ECS | Usable | Archetype SoA, parallel queries, command buffers | — |
| Asset pipeline | Usable | Stable UUIDs, `.meta`, cooked cache, textures, glTF (hardened against cgltf advisories), audio, meshoptimizer LODs, BC/ASTC texture compression, thumbnails | No import queue UI or dependency inspector; glTF cameras/lights not imported; packs are not compressed or signed |
| Physics (Jolt 3D, Box2D 2D) | Usable | Bodies, triggers, queries, joints, character controllers, terrain heightfields | Checkpoints keep rigid-body velocities but not character controller velocity; whole heightfield rebuilt on every terrain edit |
| Animation | Usable | glTF clips, GPU skinning, cross-fades, sprite animation | No blend trees, state machines, IK or root motion |
| Navigation (Recast/Detour) | Usable | Auto navmesh from colliders and terrain, agents with avoidance | Single tile, so very large levels need coarser voxels; rebuilds are whole-mesh; no authored off-mesh links |
| Gameplay AI | Usable | Behaviour trees, blackboard, perception, Luau tasks, debugger | No visual tree editor; no environment queries |
| Audio (miniaudio) | Usable | Buses, 3D spatialization, streaming, script API | No effects (reverb, filters), no audio profiling; never listened to on Android |
| In-game UI | Usable | Canvases, anchors, text, images, buttons, layout, navigation | No UI editor canvas, rich text or scroll views |
| Scripting (Luau) | Usable | Sandboxed VM, budgets, hot reload, broad engine API | API is not versioned |
| Visual scripting | Experimental | Graph model, compiler to Luau, editor, trace debugger | No breakpoints/watches; no user functions or comments |
| Particles / VFX | Experimental | Deterministic CPU emitters, soft HDR billboards | CPU only; no GPU particles, ribbons, mesh particles or collision |
| Terrain and foliage | Experimental | Heightfield LOD chunks, sculpt/paint tools, collision, navmesh, scatter | One draw per foliage instance (no instancing), no holes, scripts cannot deform terrain |
| Sequencer | Experimental | `.vseq` tracks, events, editor timeline with record key | No curve editor, multi-key select or video export |
| Networking (ENet) | Experimental | Authoritative server, prediction, interpolation, delta snapshots, encryption, reconnect, listen server | Tested on loopback only; no NAT traversal, host migration, or lag-compensated hits |
| Editor | Usable on Linux | Docked workspace, gizmos, undo/redo, multi-select, command palette, autosave/recovery, asset browser with thumbnails, material/graph/sequencer/terrain tools, profiler, AI and script debuggers, play-in-editor incl. multiplayer | Never run on Windows/macOS by a person; no layout presets; viewport input tested through APIs rather than synthetic mouse events |
| Packaging (`victus export`, `.vpak`) | Usable on host platform | Validated, reproducible packs, notices, manifest | No cross-export, code signing, installers or patching |
| Android | Incomplete | APK builds in CI; touch API; lifecycle | Never installed on a device or emulator; no GLES fallback; no profiling |
| Debugging and profiling | Usable for CPU | Profiler window, Chrome trace export, capture/replay with divergence reports | No GPU timings; no memory history graph |
| Save games | Usable | Slots, checkpoints, script data | No compression, cloud saves or editor browser |
| Plugins / modding SDK | Missing | Unknown components survive load/save | No plugin loading, versioned public API or mod sandbox |
| Localization | Usable | Per-locale `.vstrings` string tables, keyed `UiText`, `loc` script API with placeholders and fallback locale, hot reload; built-in fonts cover Latin, Cyrillic and Greek | No text shaping or right-to-left layout; no plural rules; no translation editor or string extraction; CJK and other scripts need a project font |
| iOS, Web, XR, consoles | Missing | — | — |

## Cross-cutting limitations

- **Hardware:** every rendering result was produced on Mesa lavapipe (software Vulkan) or CI's virtualised Apple GPU. Frame-time numbers in STATUS.md are for that setup and do not predict real GPU performance.
- **Determinism:** capture/replay is verified on one compiler and machine; cross-platform floating-point determinism is not.
- **Device loss:** SDL_GPU 3.4 has no device-lost signal. VICTUS infers loss from repeated failed GPU operations and exits cleanly (exit code 3; the editor autosaves first), but cannot recover and keep running. Only injected failures have been tested, not a real driver reset or GPU removal.
- **Static analysis:** clang-tidy runs on engine, editor, tools and apps sources (see CONTRIBUTING.md); test sources are not linted.
- **Third-party advisories:** cgltf 1.15 has four open advisories with no upstream fix. VICTUS validates accessors and unpacks sparse data itself before handing glTF data to cgltf (see THIRD_PARTY_AND_LICENSES.md); untrusted glTF files should still be treated with care.
