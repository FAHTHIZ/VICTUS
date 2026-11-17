# Technology Decisions

A candidate is not adopted until exact version, license, platform matrix and maintenance status are verified.

| Area | Current position | Status |
|---|---|---|
| Language | C++20 baseline | Provisional |
| Build | CMake + CTest | Implemented |
| Platform | SDL3 3.4.20 (window, input, gamepad, lifecycle) behind `victus::platform` | Accepted (ADR-0007) |
| Graphics | VICTUS RHI with SDL_GPU (Vulkan/Metal/D3D12) as first backend | Accepted (ADR-0003) |
| Shaders | GLSL 450 → SPIR-V (glslang) + MSL 2.1 (SPIRV-Cross), offline, committed | Accepted (ADR-0003) |
| ECS | Compare Flecs, EnTT, custom SoA | Open |
| Scheduler | Compare Taskflow, enkiTS, custom | Open |
| Physics | Box2D v3.2.0 (2D) and Jolt v5.7.0 (3D) candidates, versions verified 2026-11-16 | Open (next slice) |
| Navigation | Recast/Detour candidate | Open |
| Animation | ozz-animation candidate | Open |
| Editor UI | Dear ImGui 1.93 docking on the VICTUS RHI | Accepted (ADR-0005) |
| Asset interchange | glTF 2.0 first; evaluate OpenUSD | Open |
| Audio | miniaudio candidate | Open |
| Scripting | Luau 0.747 (sandboxed VM, gradual types) behind `victus::script` | Accepted (ADR-0008); visual scripting open |

## ADR-0001 — C++20 dependency-light foundation
Accepted for bootstrap. Standard-library-only core gives CI a reliable baseline. Revisit C++23 only after compiler/platform validation.

## ADR-0002 — Main-branch delivery
Accepted by owner. Commit coherent verified increments directly to main; keep commits small, preserve history and never force-push.

## ADR-0003 — Graphics backend (accepted 2026-11-16)
**Problem.** VICTUS needs real 2D/3D rendering on Windows, Linux, macOS, Android and iOS now, without a multi-year investment in three native backends before any game can render, and without becoming a wrapper around another engine.

**Candidates.** bgfx (BSD-2; own shader language/toolchain and build system, renderer-level abstraction), Diligent Engine (Apache-2.0; large, heavy build), direct Vulkan + Metal + D3D12 (maximum control, largest cost), SDL_GPU from SDL 3.4.x (zlib; explicit modern API: command buffers, render/copy/compute passes, cycling resources, Vulkan/Metal/D3D12 backends, Android/iOS support, small).

**Decision.** Engine renderers program only against the VICTUS RHI (`engine/rhi/include/victus/rhi/rhi.hpp`: generational handles, formats, pipelines, passes, uploads, readback). Its first backend is SDL_GPU (`engine/rhi/src/rhi_sdl.cpp`). Render graph, renderers, materials, shaders and all scene logic are VICTUS-owned. Shaders are authored once in GLSL 450 using SDL's resource-set convention and compiled offline by `tools/shaders/compile_shaders.py` into SPIR-V + MSL embedded in `engine/render/generated/builtin_shaders.cpp`; CI verifies the generated file matches sources by SHA-256 without needing the tools.

**Evidence.** Vulkan via Mesa lavapipe 26 (Vulkan 1.4) headless and through an X11 swapchain under Xvfb: clear/readback, sprite batching, PBR + shadow-map scene, tonemap and HUD overlay verified by pixel probes and visual inspection (see STATUS.md).

**Risks / gaps.** Metal path compiles shaders to MSL but has not been executed on Apple hardware; D3D12 requires DXIL (not yet generated) so Windows currently uses SDL_GPU's Vulkan driver; no GPU timestamp queries in SDL_GPU 3.4 (CPU profiling only); bindless/ray tracing/mesh shaders are not available through SDL_GPU. **Revisit trigger:** a measured bottleneck or required feature that SDL_GPU cannot provide; a native backend would then be added behind the same RHI. DXC (linux release v1.9.2609) is the planned DXIL path.

## ADR-0007 — Platform layer (accepted 2026-11-16)
SDL3 3.4.20 (zlib), fetched by pinned URL + SHA-256 (`cmake/VictusDependencies.cmake`), statically linked; `VICTUS_USE_SYSTEM_SDL` allows distro packages. Engine code sees only `victus::platform` (windows, keyboard/mouse/gamepad/touch input, lifecycle events, clipboard). `VICTUS_WITH_GRAPHICS=OFF` builds the core headless without SDL (servers, simulations).

## ADR-0004 — ECS and scheduler (accepted provisionally 2026-11-16)
**Decision.** VICTUS owns its ECS (`engine/ecs`): archetype storage with one aligned SoA column per component, archetype add/remove edges, generational 32+32-bit entities, runtime-registered components (needed for scripting and editor-defined data), cached queries over archetypes with exclusion filters, deferred command buffers and parallel iteration via the existing VICTUS job system (chunked `parallel_for`). Structural changes during iteration assert. Scenes (`engine/scene`) layer identity (UUID), hierarchy (intrusive sibling lists in a `Hierarchy` component) and serialization on top; serialization is a registry of named component serializers so unknown components round-trip unchanged.

**Why not Flecs/EnTT now.** Both are MIT and excellent; adopting either would expose its API across editor, scripting and networking. A small owned core keeps the replication/serialization/scripting seams under VICTUS control. **Evidence so far:** `victus_bench_scene` (STATUS.md, Slice 3). **Still required:** an identical-workload comparison against Flecs and EnTT; if VICTUS is materially slower on iteration or structural changes, either optimise or adopt behind the `victus::ecs` API. The job system remains custom (Slice 1); Taskflow/enkiTS comparison is still open.

## ADR-0008 — Scene format and asset pipeline (accepted 2026-11-16)
- Scenes: JSON `{"format":"victus.scene","version":1,"entities":[...]}` with entity UUIDs, parent UUIDs and components sorted by name for diff-friendly files. Loading is 3-pass (create, parent, components). A binary cooked scene format is planned for shipped builds.
- Assets: every source file gets a `<file>.meta` sidecar (`victus_meta`, `uuid`, `importer`, `settings`) that is meant to be committed with the project. Cooked results go to `<project>/.victus/cache/<uuid>.vcooked` (header `VCOOK`, format version, key hash of source bytes + settings + importer version, dependency hashes); the cache is disposable. Invalid metas are reported, never silently overwritten; duplicated UUIDs (copied files) are reassigned.
- glTF: cgltf 1.15 (MIT, single header, no dependencies) chosen over tinygltf (pulls in JSON library + stb) and fastgltf (C++20, simdjson dependency, faster but larger). File access is sandboxed to the asset's directory; remote URIs are rejected.
- Layering note: `engine/assets` depends on `engine/render` only for CPU types (`Image`, `MeshData`); a future split of those types into a GPU-free module will let headless cookers drop the render dependency.

## ADR-0005 — Editor UI (accepted 2026-11-16)
**Decision.** The VICTUS editor (`victus_editor`) is built on Dear ImGui v1.93.0-docking (commit `b916186e`, MIT), vendored unmodified in `third_party/imgui`. Only the upstream SDL3 *platform* backend is used; rendering goes through VICTUS's own renderer in `engine/ui` on the VICTUS RHI, so the editor exercises the same GPU path as games (Vulkan verified, Metal/D3D12 via SDL_GPU). The editor is a library (`victus_editor_core`) plus a thin executable so it can be driven headlessly by tests.

**Candidates.** Qt 6 (LGPL-3.0/commercial; strong accessibility, text and native widgets but heavy packaging, dynamic-linking obligations and a second rendering stack); Avalonia/.NET (MIT but needs a managed runtime and a GPU interop layer); a custom retained UI (largest effort, no ecosystem). ImGui wins today on embeddability, licensing, one-process debugging and iteration speed.

**Risks / revisit triggers.** ImGui has limited accessibility (screen readers), IME and rich-text support and the docking branch API moves. Mitigation: editor features live in VICTUS code behind panels, undo commands and the scene/asset APIs, not in ImGui state, so a shell replacement does not touch engine data. Revisit before a 1.0 editor if accessibility or text-heavy tools (script editing, localisation) require it. Runtime game UI is a separate decision (Phase 9); ImGui is not the shipped game UI.

**Fonts.** ImGui's embedded ProggyClean/ProggyForever fonts (MIT, Tristan Grimmer / Disco Hello) are used; no other fonts are bundled.

## ADR-0006 — Original license
Selected: Apache-2.0 for original VICTUS code. Third-party components retain their own licenses/notices. Maintain SBOM.

## ADR-0008 — Gameplay scripting language (accepted 2026-11-16)
**Problem.** Games need fast-iteration gameplay code that hot reloads in the editor, cannot crash or hang the engine, is safe to run from downloaded projects and mods, and works on every target including iOS (no JIT allowed) and consoles.

**Candidates.** Luau 0.747 (MIT, Roblox; Lua 5.1-derived with gradual typing, a native `vector` type, a built-in sandbox and VM interrupts); Lua 5.4 (MIT; smaller, no sandbox or types, standard library must be stripped by hand); LuaJIT (MIT; fastest on x86/ARM64 desktop but its JIT is forbidden on iOS and consoles, interpreter-only there, and upstream releases are irregular); Wren (MIT; small community, no types); AngelScript (zlib; C++-like, weaker sandboxing story); C# via .NET/Mono (MIT; large runtime, AOT complexity on iOS/consoles, GC pauses); WebAssembly (safe and language-agnostic but needs a toolchain per language and an ABI for every engine call).

**Decision.** Embed Luau (VM + compiler only, fetched as the pinned 0.747 release tarball). `engine/script` owns one VM per `ScriptHost`, applies `luaL_sandbox` (read-only globals, per-module sandboxed threads), a tracking allocator with a hard memory limit and an interrupt-based per-call time budget (an infinite loop raises an error instead of freezing the editor). Scripts are `.luau` assets referenced by a serialized `ScriptComponent` (path + property overrides + enabled). The API (`entity`, `scene`, `input`, `time`, `log`) is VICTUS's own and goes through the scene/serializer registry, so scripts see the same component data as the editor and scene files. Faulted instances are disabled until their file is reloaded; errors go to the log/console with file and line.

**Evidence.** `victus_script_tests` (sandboxing, budget, memory limit, lifecycle, hot reload, properties), the three templates are fully scripted and played headlessly by `victus_project_tests` and `victus simulate` in CI; `victus_bench_script` Release p50 on 2 vCPU Xeon: 10 000 scripted entities `update` 3.89 ms (~0.39 µs/entity incl. transform read/write), 1M-iteration arithmetic loop 8.15 ms.

**Risks / revisit triggers.** Luau's native code generator is not used yet (interpreter only; evaluate on desktop once hot spots are measured). Type checking (Luau.Analysis) is not wired into `victus check` yet. A managed or WASM tier may be added later for heavy systems; C++ remains the language for engine systems. Visual scripting is deferred and will compile to the same runtime API.

## ADR-0009 — Physics engines (accepted 2026-11-16)
**Problem.** VICTUS needs production-grade 3D and 2D rigid-body physics (shapes, dynamic/kinematic/static bodies, triggers, queries, contact events) that is deterministic enough for tests, scales across worker threads, runs on mobile CPUs and has a permissive license for commercial games.

**Candidates.** 3D: Jolt Physics v5.7.0 (MIT, Jorrit Rouwe; used by Horizon Forbidden West and Godot 4.4+ as its default 3D backend); PhysX 5 (BSD-3-Clause; very capable but a heavy build, GPU features NVIDIA-only); Bullet 3 (zlib; mature but largely unmaintained); a custom solver (years of work). 2D: Box2D v3.2.0 (MIT, Erin Catto; v3 rewrite with SIMD solver, multithreading via task callbacks, sensor events); Chipmunk2D (MIT; inactive); a custom 2D solver.

**Decision.** Jolt 5.7.0 and Box2D 3.2.0, fetched as pinned release tarballs with SHA-256 verification, built as third-party targets without VICTUS warning flags, behind one owned API (`victus::physics`). Scenes only see VICTUS components (`RigidBody`, `Collider`, `RigidBody2D`, `Collider2D`) that are serialized, reflected into the editor inspector and exposed to Luau; the backends never leak into public headers. `PhysicsWorld` syncs components to bodies (create/update/destroy, kinematic targets, teleports), steps in `Simulation` FixedUpdate, writes dynamic transforms back, and publishes sorted per-tick contact/trigger events that `ScriptHost` dispatches as `on_collision_enter/exit` and `on_trigger_enter/exit`. Both solvers run on the engine `JobSystem` when one is supplied. Build choices: Jolt `CPP_RTTI_ENABLED=ON` (needed to subclass `JobSystemWithBarrier` without undefined typeinfo), AVX/AVX2/AVX-512 off so binaries run on any x86-64 (SSE4.2 baseline; ARM64 uses NEON), `OBJECT_LAYER_BITS=32` with a VICTUS-encoded (layer, mask, motion) object layer so per-collider masks and the 16x16 layer matrix are honoured.

**Evidence.** `victus_physics_tests` (falling bodies come to rest on ground in both dimensions, kinematic platforms carry bodies, triggers, layer masks, raycasts/overlaps, velocities/forces/impulses, teleports, reparenting, determinism of repeated runs, serialization round-trips); `victus_script_tests` physics case; `victus_project_tests` plays the 2D platformer on Box2D (land, run to the wall, jump, collect coins via triggers) and the 3D template (crate stack settles, the kinematic ball knocks it over); `victus_bench_physics` (see STATUS).

**Risks / revisit triggers.** Cross-platform bit-exact determinism is not claimed (Jolt supports it with `CROSS_PLATFORM_DETERMINISTIC`, which costs speed; revisit for lockstep networking). Character controllers (`JPH::CharacterVirtual`), joints, mesh/convex-hull colliders and 2D polygons are not exposed yet. If either upstream stalls, the owned component API allows swapping the backend without touching scenes or scripts.

## ADR-0010 — Audio engine (accepted 2026-11-16)
**Problem.** Games need low-latency playback of sound effects and streamed music, mixing buses with volume/mute, 3D spatialization following scene transforms, and decoding of common formats, on desktop and mobile, with headless tests that can assert real mixed output.

**Candidates.** miniaudio 0.11.26 (single-file C, public domain or MIT-0, David Reid; backends for WASAPI, CoreAudio, ALSA/PulseAudio/JACK, AAudio/OpenSL ES, Web Audio; built-in WAV/FLAC/MP3 decoders, node graph, spatializer); SDL3 audio (already linked, but only device/streams: no decoding, mixing or spatialization); OpenAL Soft (LGPL, dynamic linking constraints); FMOD/Wwise (proprietary, per-title licensing); SoLoud (zlib; less active).

**Decision.** miniaudio 0.11.26 fetched as a pinned release tarball (SHA-256 verified) and compiled from one implementation unit (`engine/audio/third_party/miniaudio_impl.c`) with `MA_NO_ENCODING`, stb_vorbis from miniaudio's `extras/` for Ogg Vorbis, warnings disabled, behind an owned API (`victus::audio`). Scenes see only `AudioSource`/`AudioListener` components (serialized, reflected into the inspector, exposed to Luau). Six fixed buses (Master, Music, Sfx, Ui, Ambient, Voice) are miniaudio sound groups under Master. `AudioEngine::create` falls back to a no-device engine when no output exists (or `VICTUS_NO_AUDIO=1`), and `render()` pulls mixed frames from that engine so tests assert actual sample output. On Apple, `MA_NO_RUNTIME_LINKING` links CoreAudio/AudioToolbox directly (required for iOS App Store builds).

**Evidence.** `victus_audio_tests` (6 cases: null-device mixing of a WAV clip with non-silent output, finishing and looping, bus volume/mute, pause/stop; spatial attenuation by listener distance and panning; one-shot reaping, voice cap and bad-clip reporting; streamed Ogg Vorbis and FLAC decoding; component serialization and system-driven playback; MP3 is decoded by miniaudio but not covered by a test file), script audio case in `victus_script_tests`, template audio assertions in `victus_project_tests`, `victus_bench_audio` (see STATUS).

**Risks / revisit triggers.** No DSP effects (reverb, filters, ducking) or HRTF exposed yet; Opus is not decoded; streaming reads files directly (no packed-archive VFS yet, revisit with `victus export`). Device hot-plug relies on miniaudio's default rerouting.

## ADR-0011 — Game packaging and the `.vpak` archive (accepted 2026-11-16)
**Problem.** Games must ship as a runnable folder that contains no editor-only data (source images, glTF, `.meta` files), never runs importers on players' machines, loads fast on mobile storage, and can be checked for corruption.

**Candidates.** Loose folders (simple, but slow to open thousands of files on mobile, easy to tamper with and ship sources by mistake); zip via miniz/libzip (standard tooling, but zip's central directory and per-entry extras add parsing surface and compression is per-entry deflate only); PhysicsFS (zlib; full VFS but another dependency and C-style global state); an owned archive format.

**Decision.** An owned, deliberately small format (`victus/core/pack.hpp`): 40-byte header, 16-byte-aligned payloads, a table of contents with sanitized relative paths, per-entry FNV-1a 64 checksums verified on every read and a TOC checksum verified on open; offsets and sizes are bounds-checked against the file, duplicate or escaping paths are rejected. Entries are written in sorted order, so the same project exports byte-identical packs. A codec field is reserved (only "stored" today). `fs::mount(root, pack)` overlays a pack on a folder so `read_binary/read_text/exists/list_files` transparently read pack entries; everything above core (projects, scenes, scripts, audio through a miniaudio VFS, cooked assets) works unchanged. `AssetDatabase::open_packaged` reads an asset index written at export instead of scanning and never imports or writes. `victus export` validates scenes and scripts before writing, copies the player, embeds third-party license texts collected at configure time from the pinned dependency sources, and writes an `export.json` manifest.

**Evidence.** `victus_core_tests` pack and mount cases (round trip, reproducible output, corrupted payload/header/TOC/truncation detection, path escapes, overlay precedence and listing), `victus_export_tests` (export contents, packaged texture/model/scene/script/audio loading from a mounted pack with nothing written next to it, re-export, refusal to write into unrelated or in-project folders, broken-script and missing-reference failures), `victus_cli_smoke` exports all three templates and runs each exported game headless with a screenshot.

**Risks / revisit triggers.** No compression yet (cooked textures are uncompressed RGBA8 mip chains; GPU texture compression and LZ4/zstd are the next size wins). Packs are read with buffered reads, not memory-mapped. FNV-1a detects corruption, not tampering; signing is a later item. Export targets the host platform only.

## ADR-0012 — Scalable rendering quality pipeline (accepted 2026-11-16)
**Problem.** The 3D renderer must look close to modern high-end engines on desktop while the same content runs on phones. Every expensive effect therefore needs a cheaper fallback and a switch, chosen per project and overridable per run, without content changes.

**Candidates.** Deferred shading (G-buffer bandwidth is the main cost on tile-based mobile GPUs); forward with a depth prepass (cheap on tilers, MSAA/FXAA friendly, gives SSAO its depth); clustered forward (better for hundreds of lights, more complexity than today's 16-light limit needs); ray-traced AO/shadows (not available on most mobile hardware).

**Decision.** Forward HDR PBR with an optional depth prepass that feeds SSAO (Alchemy/SAO, optional half resolution + depth-aware blur), cascaded shadow maps (1–4 cascades in a 2×2 atlas, practical split scheme, texel snapping, dithered cascade transitions, optional soft PCF), dual-filter bloom (13-tap Karis downsample, tent upsample, 3–6 levels), GPU auto exposure (64×64 log-luminance → 1×1 R32F ping-pong with separate brighten/darken adaptation speeds), selectable tonemappers (ACES fitted, Khronos PBR Neutral, Reinhard, none) with exposure compensation, saturation, contrast and vignette, then FXAA 3.11. `RenderSettings::preset(QualityTier)` defines Low/Medium/High/Ultra (Low: 0.75 render scale, 1 hard cascade, no SSAO/bloom, 4 lights; Ultra: 4 cascades, full-res 16-sample SSAO, 6 bloom levels); `sanitized()` clamps hand-written values. Per-scene look lives in the reflected `Environment` component; the tier lives in `victus.project` (`rendering.quality`, `rendering.render_scale`) and can be overridden with `--quality`. Each pass is a render-graph node, so disabled effects cost nothing.

**Evidence.** `victus_gpu_tests` "gpu quality" cases (8): pass counts per tier, distant casters shadowed only with CSM, SSAO darkening a crease but not open floor, bloom halo around an emissive, auto exposure normalising dark and bright scenes and adapting over time, tonemapper/grading differences, FXAA reducing hard step edges, render scale; Environment component end-to-end case; `victus_render_tests` preset/sanitize cases; `victus_cli_smoke` runs each template at every tier. Timings in STATUS are llvmpipe CPU numbers only.

**Risks / revisit triggers.** No temporal AA/upscaling, motion blur or depth of field yet; point lights are capped at 16 (move to clustered forward when scenes need more); no reflection probes or GI; tiers are not yet validated on physical mobile GPUs. Revisit the prepass on tilers if measurements show it costs more than it saves.

## ADR-0013 — Automatic GPU instancing (accepted 2026-11-16)
**Problem.** Scenes repeat the same mesh and material many times (foliage, props, crowds). One draw call per object makes CPU submission the bottleneck long before the GPU is busy, which is worst on mobile drivers.

**Candidates.** Explicit instancing components authored by users; automatic batching of identical draws with per-instance vertex data; GPU-driven culling with indirect draws and storage buffers (best scaling, but needs compute culling and multi-draw-indirect, which are not uniformly available on SDL_GPU targets and older mobile GPUs).

**Decision.** Automatic instancing inside `SceneRenderer`: every mesh draw is instanced (a single object is a batch of one), with model and inverse-transpose normal matrices (112 bytes) in a per-frame vertex buffer at slot 1 using instance step rate. Visible draws are sorted by pipeline, base texture, a mesh+material key, then distance; runs with the same mesh and exactly equal material (checked field by field, not just by hash) become one `draw_indexed` with N instances. Shadow cascades batch by mesh only. Each pass binds its own contiguous instance range by offset (no reliance on base-instance support). The instance buffer grows geometrically and is uploaded with `discard` so in-flight frames are renamed instead of stalling. `RenderSettings::instancing` turns batching off for A/B comparison. Indirect, GPU-driven culling stays a later option.

**Evidence.** `victus_gpu_tests` "gpu instancing" cases (identical image with batching on/off within 1 LSB, batch counts in main/prepass/shadow passes, materials and meshes never merged, buffer growth across frames). `victus_bench_render`: 4 096 cubes, Low tier, 320×180 on llvmpipe: 21.8 ms instanced vs 258.8 ms unbatched (p50).

**Risks / revisit triggers.** Front-to-back order is only kept within a batch, so heavy overdraw scenes on the Low tier (no prepass) may lose some early-z benefit; instances are rebuilt every frame (no persistent static batches yet); skinned meshes will need their own path.

## ADR-0014 — Character controller and physics joints (accepted 2026-11-16)
**Problem.** Games need a player character that walks on slopes, climbs stairs, stands on moving platforms and pushes objects without the jitter and tunnelling of a plain rigid body, plus doors, levers, pendulums, ropes and breakable connections. Both must stay behind owned `victus::physics` types so the backend can change.

**Candidates.** A dynamic rigid-body capsule (`JPH::Character`): simple, but friction and stepping need hacks and it is pushed around by the solver. Jolt `CharacterVirtual`: collision-query based, moved by the game each step, with built-in stair walking, floor sticking and contact callbacks. An owned kinematic sweep controller: full control but duplicates work Jolt already ships and tests. For joints: expose Jolt constraint types directly vs a small owned `Joint` component that maps to them.

**Decision.** `CharacterController` component backed by `JPH::CharacterVirtual` (Jolt 5.7.0) with a capsule whose bottom sits at the entity origin and an inner rigid body (`mInnerBodyShape`) so raycasts, triggers and other bodies see the character. Characters are updated before each rigid-body step: ground velocity update, ground-relative desired velocity with exponential air control, gravity only along the ground normal while supported on a walkable slope, then `ExtendedUpdate` with stair step-up and stick-to-floor set to `step_height`. `push_force` sets the maximum force the character exerts on dynamic bodies (`mMaxStrength`). `Joint` component (fixed, point, hinge, slider, distance) references the connected entity by UUID (nil = world); body 1 is the connected body so angles and slider positions measure this entity. Limits, velocity motors (force/torque limited) and a break force (constraint impulse / dt checked after every step, then `SetEnabled(false)`) are supported; a `GroupFilter` gives every body its own group and skips pairs that share a non-colliding joint. Constraints are rebuilt only when bodies or non-motor fields change, and are removed before their bodies.

**Evidence.** `victus_physics_tests` 16 character/joint cases (see STATUS Slice 11), script and template tests, and `victus_bench_physics`: 256 characters step in 0.514 ms p50 on one 2.9 GHz Xeon thread. Sources: Jolt `CharacterVirtual` reference and architecture/constraints documentation (RESEARCH_REGISTER).

**Risks / revisit triggers.** Character capsules only (no crouch shape swap or custom shapes yet); characters are updated serially; 2D (Box2D) has no character controller or joints yet; no cone, swing-twist, 6-DOF or ragdoll joints and no position motors. Revisit the serial update if scenes need thousands of characters.

## ADR-0015 — Skeletal animation and GPU skinning (accepted 2026-11-16)
**Problem.** Characters need imported skeletal animation that plays, loops and cross-fades, is scriptable, renders with correct shadows and depth, and scales to hundreds of characters on mobile-class CPUs and GPUs.

**Candidates.** A third-party runtime (ozz-animation: excellent SoA sampling, but an extra offline format and dependency); CPU skinning (simple, but per-vertex work and buffer uploads every frame scale badly); GPU skinning with a per-draw joint palette; animating joints as ordinary scene entities vs a hidden pose buffer.

**Decision.** An owned `victus::animation` module that samples glTF 2.0 channels directly from the cooked model (STEP, LINEAR with normalized shortest-path quaternions, CUBICSPLINE, clamped ends). Joints stay ordinary scene entities bound by node name under the animator's root, so the editor, scripts, physics attachments and the existing transform propagation all see animated bones; the system writes local transforms in `Phase::Update` and evaluates animators in parallel with `parallel_for`. Skinning runs on the GPU: a separate `SkinVertex` stream (4 × u16 joints + 4 × f32 weights) and skinned vertex-shader variants for the main, pre-pass and shadow passes read a joint palette from a storage buffer; palettes are computed relative to the mesh entity so the skinned node transform is ignored as glTF requires; skinned draws of the same mesh and material still instance, each instance indexing its own palette range. The importer keeps the four strongest influences from up to two JOINTS/WEIGHTS sets and renormalizes them.

**Evidence.** `victus_animation_tests`, asset round-trip tests and pixel-probed `gpu skinning` cases (STATUS Slice 12); `victus_bench_animation`: 1 024 characters × 32 joints update in 1.481 ms on one thread and 0.827 ms with jobs (p50, 2 vCPU 2.9 GHz Xeon, 0 allocations). Source: glTF 2.0 specification skins and animations sections (RESEARCH_REGISTER).

**Risks / revisit triggers.** Entity-per-joint costs ECS memory for very large crowds; revisit with a compact pose buffer plus GPU-side palette generation if crowds reach thousands of animated characters. No dual-quaternion skinning (volume loss at twisting joints), blend trees, state machines, root motion, IK, retargeting or morph targets yet.

## ADR-0016 — Particles and VFX (accepted 2026-11-16)
**Problem.** Games need fire, sparks, smoke, dust and pickup effects that look good in the HDR pipeline (glow through bloom, no hard edges where sprites cut into geometry), are scriptable and deterministic, run on mobile-class GPUs, and work in both 2D and 3D scenes.

**Candidates.** GPU-simulated particles (compute shaders + indirect draws: scales to millions, but needs compute in the RHI, makes scripting/readback and determinism harder, and mobile GPUs pay for it in bandwidth); CPU simulation with instanced billboards; a third-party VFX runtime (e.g. Effekseer: rich authoring, but a large dependency with its own renderer integration).

**Decision.** An owned headless `victus::particles` module: a serialized `ParticleEmitter` component (rate, bursts per cycle, duration/loop, cap, point/sphere/cone/box shapes, speed, gravity, drag, size/colour over life with HDR intensity, spin, alpha/additive blend, optional texture, softness, 2D layer, seed) with hidden runtime state. Each emitter owns a PCG32 stream (seeded from `seed` or its entity UUID) and is simulated independently in `Phase::PostUpdate` (after scripts and physics move emitters), in parallel through `parallel_for`, so results are bit-identical serial or threaded. Each update produces 48-byte billboards that share `render::ParticleInstance`'s layout, so the runtime submits them without conversion. The renderer draws them as instanced camera-facing quads (six vertices from `gl_VertexIndex`, one instance per particle) in an HDR pass after the opaque forward pass, without a depth attachment: the fragment shader reads the scene depth texture, discards fragments behind geometry and fades near it (soft particles). Batches are frustum-culled by their bounds and drawn back to front; alpha batches sort their particles back to front, additive batches do not need to. 2D scenes draw the same billboards through `SpriteBatch`, which gained an additive pipeline.

**Evidence.** `victus_particles_tests` (10 cases incl. serial vs job-system byte equality), 4 pixel-probed `gpu particles` cases, template and script tests (STATUS Slice 13). `victus_bench_particles`: 64 emitters × 256 particles (16 384 live, steady state) update in 0.236 ms on one thread and 0.129 ms with jobs (p50, 2 vCPU 2.9 GHz Xeon, 0 allocations). Source: Lorach, "Soft Particles" (NVIDIA, 2007) (RESEARCH_REGISTER).

**Risks / revisit triggers.** CPU cost and upload bandwidth grow linearly; revisit with GPU simulation when scenes need more than ~100k live particles. No collision, trails/ribbons, flipbooks, curves editor, sub-emitters, lighting of particles or shadow casting yet; additive 2D colours are clamped to LDR.

## ADR-0017 — 2D character mover, one-way platforms and 2D joints (accepted 2026-11-16)
**Problem.** 2D games need a platformer-grade character (crisp, non-bouncy movement, ground detection, slopes, one-way platforms you jump through and drop off, pushing crates) and joints (chains, bridges, swinging props, wheels, motors), scriptable through the same Luau API as the 3D `CharacterController`/`Joint` (ADR-0014).

**Candidates.** A dynamic `RigidBody2D` driven by velocity plus raycast grounding (what the 2D template used: simple, but contact friction/restitution make it bouncy and sticky on edges, and one-way platforms need custom contact handling anyway); a kinematic body with hand-written sweeps; Box2D 3.2's geometric mover API (`b2World_CollideMover` + `b2SolvePlanes` + a sweep + `b2ClipVector`, as in its `geometric_mover.cpp` sample).

**Decision.** `CharacterController2D` (capsule `size`, `max_slope`, `snap_distance`, `gravity_scale`, `push_force`, `air_control`, layer/mask) uses the Box2D mover: each fixed step integrates a velocity model matching the 3D controller (desired velocity plus the ground body's point velocity on the ground (riding kinematic and dynamic platforms), `air_control` blend in the air, gravity), then up to 5 iterations of collide (5 mm skin) → plane solve → shape cast, clipping velocity against the solved planes; a 2 cm downward probe decides `grounded` against `max_slope`, and the character snaps down within `snap_distance` while walking. A kinematic inner capsule follows the mover through `b2Body_SetTargetTransform`, so raycasts, triggers and other bodies see it; dynamic bodies in the way get an impulse capped at `push_force × dt`. `Collider2D.one_way` platforms are filtered in two places: the mover rejects a one-way plane unless the bottom of the capsule's lower circle is above the platform's closest point (with `drop_through(seconds)` suppressing all one-way planes for a short window), and dynamic bodies use Box2D pre-solve/continuous callbacks (contact normal within ~45° of up and not already penetrating more than 8 cm). `Joint2D` (weld, revolute, prismatic, distance, wheel) maps 1:1 to Box2D 3.2 joints with limits (degrees or metres), springs (Hz/damping ratio), motors, `break_force` (checked against the constraint force/torque each step) and `collide_connected`; a nil `connected` attaches to a static anchor body at the world position of `anchor`. Joints are rebuilt when either body is rebuilt or the component changes, like the 3D joints.

**Evidence.** `victus_physics_tests` "2D character …"/"2D joint …" (13 cases), `victus_script_tests` "script: 2D character, one-way drop-through and 2D joint motor", the 2D template test (jump up through the one-way ledge, land, drop back through, lantern joint active) (STATUS Slice 14). Release `victus_bench_physics` (p50, 2 vCPU 2.9 GHz Xeon, one 1/60 s step): 64 2D characters 0.154 ms, 256 0.862 ms; 200 swinging chain links 0.226 ms, 1 000 links 1.19 ms. Sources: Box2D v3.2.0 samples and `collision.h`/`world.h` mover API (RESEARCH_REGISTER).

**Risks / revisit triggers.** The mover has no explicit step-up (ledges are climbed only as far as the capsule's rounding slides over them) and no rotating-platform angular carry; one-way filtering for dynamic bodies is per contact and can let fast thin bodies through. Revisit with ground-velocity inheritance and a Box2D upgrade if the mover API changes.

## ADR-0018 — In-game UI (accepted 2026-11-16)
**Problem.** Games need HUDs and menus that scale with the screen, are authored as scene data, scriptable from Luau and drawn in both 2D and 3D views.

**Candidates.** Dear ImGui widgets directly (immediate mode, editor-styled, not scene data); RmlUi (HTML/CSS, large surface); commercial middleware (Noesis, Coherent: licensing); owned retained components with a small layout core.

**Decision.** Owned retained ECS components in `engine/gameui` (canvas, rect, panel, image, text, button, layout) with headless layout and interaction so tests and `victus simulate` run without a GPU. Canvas scaling follows Unity's `CanvasScaler` match mode (weighted average of log2 width/height ratios). Rendering converts draw items to Dear ImGui 1.93 draw-list geometry in a private context (anti-aliased rounded shapes, dynamic font baking) through the existing RHI UI pipeline; ImGui widgets are not used. Default font: Inter 4 Medium/Bold, SIL OFL 1.1, embedded.

**Evidence.** STATUS Slice 15 tests; `victus_bench_gameui` 520 elements update 0.048 ms, draw list 0.024 ms (p50).

**Risks / revisit triggers.** No rich text, shaping (complex scripts) or accessibility; revisit text with HarfBuzz when localization lands, and add a visual editor.

## ADR-0019 — Navigation (accepted 2026-11-16)
**Problem.** Gameplay AI needs walkable-surface generation, path queries and agents that follow paths and avoid each other, authored with no manual navmesh step and scriptable from Luau.

**Candidates.** Recast & Detour 1.6.0 (zlib; industry standard, used by Unity's legacy navmesh lineage, Unreal, Godot 3, O3DE); owned grid A* (simple, poor on 3D levels, no crowd); Godot's `NavigationServer` (engine-coupled); commercial middleware (licensing).

**Decision.** Recast & Detour 1.6.0 (`Recast`, `Detour`, `DetourCrowd` only) as a private dependency of `engine/navigation`. The navmesh is a single tile built on demand from every static, non-trigger 3D `Collider` (box/sphere/capsule/cylinder triangulated in world space, outward winding); kinematic colliders contribute a quantized upright bounding cylinder so Y-rotation does not cause rebuilds; dynamic bodies, triggers and agents are ignored. A geometry signature (FNV-1a over collider shapes, world matrices and `NavMeshSurface` settings) is checked every 0.25 s and triggers rebuilds; agents keep their destinations across rebuilds. Agents are DetourCrowd agents (anticipate turns, visibility/topology optimization, obstacle avoidance and separation); `CharacterController` agents feed the crowd velocity to `PhysicsWorld::move_character` and re-sync the corridor to the physics position each frame, other agents are placed on the surface plus `base_offset`. Recast parameters follow the RecastDemo solo-mesh defaults (edge length 12 m, simplification 1.3, region min/merge 8²/20², 6 verts/poly).

**Evidence.** STATUS Slice 16 tests; `victus_bench_navigation` (Release, p50): build of a 60 × 60 m level with 100 obstacles 44 ms, path query across it 0.028 ms, crowd update 16 agents 0.056 ms and 128 agents 0.87 ms.

**Risks / revisit triggers.** Single-tile builds are synchronous (tens of ms on large levels): move to tiled navmeshes with background rebuilds of dirty tiles (DetourTileCache) for open worlds or frequent kinematic changes. No off-mesh links (jumps, ladders), area costs or 2D navigation yet.

## ADR-0020 — Networking (accepted 2026-11-16)
**Problem.** Games need two or more players against an authoritative dedicated server with replication, prediction and interpolation, testable headless and under bad network conditions, without a platform-service dependency. Networking is a high-risk foundation, so alternatives were compared.

**Candidates.** ENet 1.3.18 (MIT; small C library, reliable/unreliable channels over UDP, fragmentation, RTT, used widely; no encryption); Valve GameNetworkingSockets v1.6.1 (BSD-3; encryption, P2P/NAT traversal, but needs OpenSSL or libsodium plus protobuf, heavy to build on every platform); yojimbo v1.15.2 (BSD-3; connect tokens and encrypted packets, bundles libsodium, netcode and reliable, opinionated message model); an owned UDP protocol (full control, large reliability/fragmentation surface to get right).

**Decision.** ENet 1.3.18 behind VICTUS's own `net::Transport` interface (ENet and in-memory loopback backends, plus a link-condition emulator wrapping either), statically linked and private to `engine/net`, so the transport can be swapped (e.g. for GameNetworkingSockets when encryption is needed) without touching sessions or scripts. Above it VICTUS owns the protocol: a versioned handshake carrying a game fingerprint, a server-authoritative session that runs scripts and physics, spawns a player from the scene's `player_template` per client and routes each client's input only to its player, 30 Hz unreliable snapshots of `NetworkIdentity` entities (transform, velocity, script values) and reliable script messages; clients predict their owned player and are corrected past a threshold, and interpolate others 100 ms behind (extrapolating at most 0.1 s, then easing back).

**Evidence.** STATUS Slice 17: `victus_net_tests` (10 cases, 410 assertions) over memory and real UDP transports including emulated latency/jitter/loss; CLI smoke runs a real server with two UDP clients; measured snapshot 216 bytes for 10 entities.

**Risks / revisit triggers.** ENet traffic is unencrypted and unauthenticated: add encryption/auth (GameNetworkingSockets or yojimbo transport, or DTLS) before public servers. Full snapshots without delta compression cost bandwidth with many entities. No physics rollback or lag compensation yet.

## ADR-0021 — Input actions (accepted 2026-11-16)
**Problem.** Scripts hard-coded key names, so games could not be played with a gamepad or rebound, and network input only carried keys. Input is user-facing in every game, so options were compared.

**Candidates.** Raw SDL3 events and SDL gamepad calls exposed directly to scripts (no abstraction, every game re-implements mapping and dead zones); a third-party action library such as gainput (MIT; actions, devices and recording, but its own device layer duplicates SDL and the repository was archived in September 2021, unmaintained); an owned action map in `engine/gameplay` over the existing `InputSource` interface, with SDL3 only as the device backend.

**Decision.** Owned `gameplay::InputMap`: named actions with button bindings or positive/negative axis bindings and a dead zone, bindings written as readable strings (`Space`, `Mouse0`, `Pad.South`, `Pad.LeftX+`) using SDL3's standard gamepad layout names, stored in the `input` section of `victus.project` with defaults, validated on load and by `victus check`. Devices stay behind `InputSource`, so the same map is evaluated over SDL input, headless `ManualInput` and the network's `RemoteInput`; protocol v2 sends merged gamepad state (buttons bitmask plus six quantized axes). No new dependency.

**Evidence.** STATUS Slice 18: input-map, Luau, template, project and network tests; CLI smoke with `--hold Pad.*`.

**Risks / revisit triggers.** Gamepads are merged into one for single-player and network input; local multiplayer needs per-player device assignment. Touch and rumble are not covered. Revisit if games need input contexts/stacks or combos.

## ADR-0022 — Asset hot reload (accepted 2026-11-16)
**Problem.** Iterating on art required restarting the game or pressing Reimport. Reloading must never crash a running game or editor, must work the same on Windows, macOS and Linux, and must say clearly what cannot be reloaded.

**Candidates.** OS change notifications written per platform (inotify, FSEvents/kqueue, ReadDirectoryChangesW: immediate, three code paths, editors that save via rename/temp files produce bursts to debounce); efsw (MIT; wraps those backends with a generic polling fallback, one more dependency); polling file size + modification time from VICTUS's own code (one portable code path, latency bounded by the interval, cost of one directory walk per interval).

**Decision.** Polling in `runtime::AssetHotReloader` (0.5 s in the player, 1 s in the editor), with the asset database's existing content hashes deciding what really needs re-importing, so touched-but-identical files cost nothing. Re-imported textures/models are evicted from `GpuAssetCache` (SDL GPU frees released textures/buffers only once it is safe) and the animation cache; audio clips are reloaded by `AudioEngine::reload_clip`; scripts keep their own watcher. Failed imports keep the last good cooked file (the database falls back to it) and are reported. Scenes, fonts, shaders and code are reported or documented as restart-only. No new dependency; OS notifications (or efsw) remain an option if project sizes make polling expensive.

**Evidence.** STATUS Slice 20: GPU, audio and editor tests; live `victus run` edit of the template robot.

**Risks / revisit triggers.** Large asset trees make each scan slower (benchmark before adopting notifications). Modification-time resolution on some file systems is coarse (size changes and the next scan still catch it). Model node-graph changes need re-instantiation to show fully.

## ADR-0023 — Prefabs (accepted 2026-11-16)
**Problem.** Games need reusable entity hierarchies that can be placed many times, tweaked per placement, updated centrally, and spawned from scripts, without breaking references between entities or scene merges.

**Candidates.** Unity-style prefabs (https://docs.unity3d.com/Manual/Prefabs.html): instances keep links to source objects and store property-level modifications, plus nested prefabs and variants; very flexible, large implementation and UI surface. Godot `PackedScene` (https://docs.godotengine.org/en/stable/classes/class_packedscene.html): a serialized scene instantiated as a subtree, with "editable children"; simple, but every scene is a prefab and overrides are coarse. VICTUS-native: a `.vprefab` file in the scene snapshot format, instance root with `PrefabInstance`, parts tagged with `PrefabPart`, component-level overrides, deterministic derived uuids.

**Decision.** The VICTUS-native design: it reuses the existing versioned component serializers (no second format), keeps instances valid scene data (an unpacked or prefab-less scene still loads), and derived uuids make instantiation deterministic for diffs, networking and tests. Component-level overrides are the first step; per-property overrides, variants and nesting are recorded as remaining work in ROADMAP Phase 9.

**Evidence.** STATUS Slice 21: scene, script and editor tests; template runs in the editor-less player and an exported build.

**Risks / revisit triggers.** Component-level overrides discard upstream changes to an overridden component; nested prefabs are flattened (lose their own link). Revisit when variants/nesting are implemented.

## ADR-0024 — Save games and checkpoints (accepted 2026-11-16)
**Problem.** Games need durable progress/profile data that survives crashes and game updates, and players expect quick save/load of the whole game state, without VICTUS games each inventing a file format.

**Candidates.** Unreal-style save objects (https://dev.epicgames.com/documentation/en-us/unreal-engine/saving-and-loading-your-game-in-unreal-engine): typed `USaveGame` objects written to named slots per user; strong typing, but engine-reflected classes and no built-in whole-world capture. Godot's documented pattern (https://docs.godotengine.org/en/stable/tutorials/io/saving_games.html): persistable nodes return dictionaries from `save()` and are recreated on load, as JSON lines or binary `store_var`; flexible, but every game writes its own loader and recovery. VICTUS-native: JSON slots with checksums, backups and versioned migrations, plus checkpoints built on the existing scene snapshot format and per-script `save_state`/`load_state` hooks.

**Decision.** The VICTUS-native design. JSON keeps saves inspectable and reuses the engine's parser and limits; the checksum + `.bak` + atomic rename protect against torn writes; `data_version` with ordered migrations makes save compatibility an explicit, testable contract. Checkpoints reuse `Scene::save/load` (component serializers already versioned), so every serializable component is captured without per-game code, while scripts own their private state through hooks (Godot's `save()` idea). Restores happen at a frame boundary to keep systems consistent.

**Evidence.** STATUS Slice 22: store, migration, checkpoint and physics restore tests; template quick save/load tests; exported game writing to the per-user folder.

**Risks / revisit triggers.** Large worlds make JSON checkpoints slow and big (consider binary or compressed payloads, measured first). Runtime state outside components (physics velocities, playback positions) is lost on restore. Multiplayer needs server-side snapshots instead of client checkpoints.

## ADR-0025 — Delta snapshots against acknowledged baselines (accepted 2026-11-16)
**Problem.** Slice 17 snapshots sent every changed entity in full (12-byte float position + 8-byte rotation) and repeated unchanged ones every second to recover from loss, wasting bandwidth and still leaving up to a second of staleness after a lost packet.

**Candidates.** (1) Quake 3 style (https://fabiensanglard.net/quake3/network.php): the server compares the current state with the last snapshot the client acknowledged, sends only changed fields, and falls back to a zeroed dummy baseline (a full update) when no acknowledged snapshot is available; loss just means an older baseline. (2) Per-entity reliable updates (send each change reliably once): no redundant resends, but head-of-line blocking delays fresh transforms behind stale ones. (3) Gaffer On Games snapshot compression (https://gafferongames.com/post/snapshot_compression/): baseline deltas plus bounded-range quantization and smallest-three quaternions with bit packing.

**Decision.** Candidate 1 with byte-aligned zigzag varints: quantize to 1 mm and the existing 16-bit rotation components so both ends hold identical views; the server keeps 32 views per client, the client 64, acks ride on the existing 60 Hz input packets. Bit packing and smallest-three (candidate 3) are deferred until measurements show the varint format is the bottleneck. The full encoding stays behind `ServerOptions::delta_snapshots` for diagnosis and comparison.

**Evidence.** STATUS Slice 23: 977.5 → 485.6 bytes per snapshot in the test scene; exact decoding for a client on a 20%-loss link; header-only idle snapshots.

**Risks / revisit triggers.** A client whose acks are all lost for more than ~1 s gets full (zero-baseline) snapshots until one arrives. Memory is O(clients × views × entities) on the server; very large worlds need sparse views or interest management. Positions beyond ±1000 km clamp.

## ADR-0026 — Input-replay regression captures (accepted 2026-11-16)
**Problem.** Gameplay regressions (script, physics or engine changes that alter how a game plays) need a repeatable test that a developer can create by simply playing, and that CI can run headless.

**Candidates.** (1) Input recording with state checksums: rely on a deterministic simulation given the same initial state and inputs, record only inputs and verify periodic digests (the determinism and per-frame checksum idea in https://gafferongames.com/post/deterministic_lockstep/). (2) State recording: store full scene snapshots every frame and compare; robust to nondeterminism but large and it cannot replay changed code to find regressions. (3) Golden screenshots: catch visual changes but are GPU/driver dependent and say nothing about gameplay state.

**Decision.** Candidate 1. VICTUS already steps simulation with explicit frame times and single-threaded Jolt/Box2D in headless runs; the only nondeterministic gameplay input found was Luau's per-VM `math.random` seed, now fixed per run through `math.randomseed` (https://luau.org/library: the sequence "depends only on seed"). Recording feeds the simulation through the same `ReplayInput` used for playback, so pressed/released edges match by construction. Digests hash names and transforms rounded to 0.1 mm so replays are compared semantically, not by runtime UUIDs.

**Evidence.** STATUS Slice 24: four templates, player-to-headless and Debug-to-Release replays matching; divergence detected at the expected frame.

**Risks / revisit triggers.** Bit-exact floating point is not guaranteed across compilers, OSes or architectures (the same article warns about debug vs release and different machines); captures should be re-recorded per platform if CI shows that. Systems that read wall-clock time or threads with racy order would break determinism. Digests do not see script-only state.

## ADR-0027 — Entity-level capture divergence reports (accepted 2026-11-17)
**Problem.** A capture replay that only says "frame 120: digest X, got Y" tells a developer that gameplay changed but not what changed; finding the cause meant re-running and diffing by hand.

**Candidates.** (1) Store the hashed state lines with each digest and diff them on mismatch (like Factorio's desync reports, which keep both game states so they can be compared, https://wiki.factorio.com/Desynchronization). (2) Store a hash per entity: small, but the report can only name entities, not what or how much changed. (3) Store nothing and re-record a reference run on demand: needs the old build, which is usually gone after the regression lands.

**Decision.** Candidate 1, capped at 4096 entities per digest (`Capture::detail_limit`, 0 disables). The lines are the exact digest input, so the report and the digest can never disagree, and they are human-readable for manual diffing. Factorio's experience (https://factorio.com/blog/post/fff-47) is that at the first diverging tick the difference is usually tiny, so reporting the first differing digest with up to three named entities is what matters.

**Evidence.** STATUS Slice 25: unit, editor and CLI checks name the moved entity and component; 300-frame captures are 20 KB (58 entities) and 184 KB (580+ entities).

**Risks / revisit triggers.** Captures of large scenes grow with entities × digests; if this matters, store per-entity hashes beyond the cap or compress. Divergence in state not covered by the lines (script variables, velocities) still shows up only later as a transform change.

## ADR-0028 — Encrypted, authenticated multiplayer sessions (accepted 2026-11-17)
**Problem.** Multiplayer traffic was plaintext: anyone on the path could read it, inject input or impersonate a server, and servers could not restrict who joins. ENet provides no security.

**Candidates.** Libraries: (1) Monocypher 4.0.2 — X25519, BLAKE2b, XChaCha20-Poly1305, two C files under CC0-1.0/BSD-2-Clause, no dependencies, under 2000 lines (https://monocypher.org/). (2) libsodium — the same primitives and more, ISC licence, portable and widely packaged (https://doc.libsodium.org/), but a much larger build (autotools/MSVC projects, no upstream CMake) to vendor on three platforms. (3) DTLS via OpenSSL or mbedTLS — standard, but certificate infrastructure, large dependencies and a datagram layer that would sit awkwardly under ENet's reliability. Protocol: (a) a Noise pattern (https://noiseprotocol.org/noise.html), NX = client anonymous, server transmits its static key (`-> e  <- e, ee, s, es`); (b) netcode.io-style connect tokens from a web backend (needs an online service, conflicts with "no mandatory vendor"); (c) DTLS handshakes.

**Decision.** Monocypher with an NX-style handshake implemented as a `Transport` decorator, so every transport (UDP, memory, link emulation) and both sessions get it without changing the session protocol. Server authentication comes from DH with the static key (`es`) confirmed by an AEAD tag over the transcript; clients may pin the key the server prints, otherwise they trust and report it. Optional server passwords travel only inside the encrypted channel. Encryption is the default for `victus serve` and clients; `--insecure` opts out on both sides.

**Evidence.** STATUS Slice 26: 7 new tests (tampering, replay, pinning, passwords, plaintext peers, a full lossy two-client session, ENet loopback) and a real UDP run with password and pinned key.

**Risks / revisit triggers.** Not a byte-compatible Noise implementation (simplified key schedule, server static key sent in clear); revisit if third-party clients need interoperability. Trust-on-first-use without `--server-key` lets an active attacker learn passwords. No rekeying or player identities yet. Review Monocypher's changelog and advisories before updating the pinned version.

## ADR-0029 — Listen server: the host plays in its own authoritative session (accepted 2026-11-17)
**Problem.** Playing together required a separate `victus serve` process plus a client per player; a player could not simply host and play, which is how most small co-op sessions start.

**Candidates.** (1) In-process host character: the server spawns a server-owned copy of the player template driven by the machine's own input (the routing fallback), with no prediction (Unreal and Source "listen server" model; https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking). (2) In-process server plus a local client over a loopback transport, each with its own scene copy — uniform code paths, but two worlds in memory, double simulation, and the host suffers interpolation delay and corrections for no reason. (3) Spawn a `victus serve` child process and connect to it — no engine changes, but two processes to manage and the same loopback costs as (2).

**Decision.** (1). `ServerOptions::host_player/host_name`; the host's character is tracked like any player (kind `kPlayer`, owner `kServerId`), appears first in rosters and `clients()` as id 0 and is `NetServer::local_player()`; scripts see the same values as offline (`net.authority()` and `is_local()` true). `victus run`/`victus_player --host` wire it up with the same encryption, key file and password options as `victus serve`.

**Evidence.** STATUS Slice 27: a session test with a host and a remote client (independent movement, replication to the client, rosters, the host surviving a client leaving) and a real UDP run of `victus run --host` with a headless client, rendered as "Hosting 2 players".

**Risks / revisit triggers.** The host has zero latency to the authority (unfair for competitive games; use `victus serve`). The session ends when the host quits (no host migration). The server ticks inside the host's frame loop, so a slow host slows everyone; revisit with a fixed-rate server tick if hosts drop frames.

## ADR-0030 — Reconnect with server-issued tokens and held characters (accepted 2026-11-17)
**Problem.** A dropped connection (Wi-Fi blip, network switch, a frozen laptop) removed the player's character and score; the client could only fail and start over as a new player.

**Candidates.** (1) Server-issued random token in the welcome, presented in a later hello; the server holds the character for a grace period (Unreal's inactive PlayerState model, https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/GameFramework/AGameMode). (2) Match by network address — no protocol change, but addresses change on exactly the network switches reconnect exists for, and NAT can make two players share one. (3) Match by player name — trivial to spoof and names are not unique. (4) Transport-level session resumption (keep the session alive under a new address, like QUIC connection migration) — invisible to the session layer, but ENet has no migration and the encrypted layer would need resumable keys.

**Decision.** (1), protocol version 4. Tokens come from the OS random generator and travel only after the handshake (encrypted by default). Held characters stay in the world idle, leave the roster and count toward `max_clients`; a hello whose token matches a still-connected client retires that stale connection first. Clients retry every `reconnect_interval` (1 s) for `reconnect_window` (20 s); after a rejoin they drop snapshot baselines, prediction history and clock sync and prune entities the server did not re-announce. Secure-layer refusals (wrong password or key) are final; handshake timeouts during a rejoin are retried.

**Evidence.** STATUS Slice 28: two session tests (server noticed / did not notice the drop, same id and character afterwards, score kept, no duplicates on any machine; grace expiry gives a fresh player and prunes the old one; a client that never returns fails; goodbyes release at once) and a real encrypted UDP run where a client frozen for 16 s rejoined as the same client.

**Risks / revisit triggers.** Anyone holding the token can take the character over (it is a bearer secret; with `--insecure` it travels in clear). Held characters are frozen in place and can be hit or pushed while their owner is away; games can react to `client_dropped`. No persistence across server restarts. Revisit when player identities arrive (bind tokens to accounts).

## ADR-0031 — State probes in capture digests (accepted 2026-11-17)
**Problem.** Capture digests covered entity names and transforms only. A script whose score, timer or state machine diverged, or a body whose velocity changed before it had moved 0.1 mm, passed the replay check until (and unless) a transform drifted, and then the report named the symptom, not the cause.

**Candidates.** (1) Registered probes that add readable `@path|kind|value` lines next to the transform lines (Factorio's "heuristic CRC" of selected crucial state plus human-readable tags in its state dumps, https://www.factorio.com/blog/post/fff-55 and https://factorio.com/blog/post/fff-47). (2) Hash the serialized scene (every component) — complete, but editor-only and cosmetic fields would fail replays, and the report could only say which bytes differ. (3) Per-subsystem checksums recorded after each system (Photon Quantum's `recordChecksum` steps and per-component checksum dumps, https://doc.photonengine.com/quantum/v1/community-wiki/knowledge-base/debugging_desyncs) — pinpoints the system, but says nothing about which value, and needs one hash per system per digest. (4) Scripts opt in through `save_state()` — reuses existing code but most scripts do not implement it.

**Decision.** (1). `gameplay::register_state_probe(name, fn)`; the script host probes started instances' own primitive fields (`_`-prefixed fields are private), physics probes velocities, spin and grounded state; values are rounded to 1e-4 and sorted with the other lines. Capture format version 2 marks probe-aware digests; version-1 captures keep transform-only digests so recorded regression suites stay valid.

**Evidence.** STATUS Slice 29: a script-only divergence that moves nothing fails with "Keeper: script.score 11 expected 10" (and is missed by a version-1 capture of the same run); all four templates record and replay clean with probes; version-1 captures from the previous build still verify.

**Risks / revisit triggers.** Any newly probed value can make older version-2 captures of a changed game fail; deliberate. Scripts that keep caches or debug counters in public fields make replays fail on them — rename to `_field`. Revisit (bump the format) when adding probes for joints, animation or UI, and if probe cost shows up in profiles of large scenes.

## ADR-0032 — Editor-hosted play sessions and replication inspection (accepted 2026-11-17)
**Problem.** Multiplayer games could only be debugged from terminals (`victus serve`, `victus run --connect`) with no view of who owns what, which entities are replicated to whom, or what bandwidth they cost.

**Candidates.** (1) The editor's Play-mode scene becomes a listen server (the existing `NetServer` with a host player) and the editor reads counters the session keeps per entity and per client (Unreal's Play-In-Editor "Play As Listen Server" net mode and the configurable player count, https://dev.epicgames.com/documentation/en-us/unreal-engine/play-in-editor-multiplayer-options-in-unreal-engine ; Unity Netcode for GameObjects' Network Profiler with per-frame byte totals and per-object activity, https://docs-multiplayer.unity3d.com/tools/current/profiler/). (2) Run a separate `victus serve` process and attach to it over a debug socket — isolates crashes but needs a second protocol and process management. (3) Inspect packet captures — no ownership or relevance information.

**Decision.** (1). Counters live in the session (cheap integer adds in the snapshot and variable loops) and are exposed as plain structs (`ReplicationInfo`, `ClientDiagnostics`) so other tools (CLI, future profilers) can use them. Editor hosting always uses real encrypted UDP so external clients can join with the shown key; recording stays offline so captures remain deterministic.

**Evidence.** STATUS Slice 30: an in-process client joins the editor-hosted session over UDP with the pinned key and appears in the clients and replication tables with the right owner and traffic.

**Risks / revisit triggers.** The editor process now also carries server load; heavy sessions should use `victus serve`. Revisit when adding multi-client play mode (several clients in one editor) and when sessions exceed a few thousand replicated entities (table virtualisation, sampling).

## ADR-0033 — In-editor clients as full client machines over loopback UDP (accepted 2026-11-17)
**Problem.** Testing a multiplayer game from the editor needed several terminals; prediction, interpolation and per-player HUDs could not be checked side by side with the host.

**Candidates.** (1) Each in-editor player is a complete client (own scene copy, scripts, physics) talking to the editor's listen server over real UDP loopback with the session's encryption (Unreal's Play-In-Editor "Number of Players" with "Run Under One Process", https://dev.epicgames.com/documentation/en-us/unreal-engine/play-in-editor-multiplayer-options-in-unreal-engine). (2) Clients on `MemoryNetwork` — deterministic and faster, but the editor's server is on UDP for external players, and a second transport would hide UDP/encryption problems. (3) Separate player windows/instances launched by the editor (as in Unity Multiplayer Play Mode, whose up to three Virtual Players open in separate windows, https://docs-multiplayer.unity3d.com/mppm/current/about/) — isolation and real windows, but process management, an extra editor instance per player and no shared inspector.

**Decision.** (1), capped at 3 clients. The viewed peer gets the keyboard and the panels; the rest idle. Nothing is shared between peers except assets on disk and the job system.

**Evidence.** STATUS Slice 31: two in-editor clients join with key and password; driving one moves only its character on the server, its prediction and the other client's remote copy.

**Risks / revisit triggers.** Simulation cost scales with peers; heavy projects should test with `victus run --connect`. Revisit for split-screen viewing, bots driving idle peers, or more than 3 clients.

## ADR-0034 — Material assets as small JSON files referencing textures by UUID (accepted 2026-11-17)
**Problem.** Mesh renderers only had a colour, metallic and roughness; a game could not put a texture on a primitive, tile a floor texture or share one look across many objects without importing a glTF per surface.

**Candidates.** (1) A `.vmat` JSON asset (PBR factors, four texture slots by asset UUID, UV scale/offset, filter, alpha cutoff, unlit, double sided) used from source like prefabs and referenced from `MeshRenderer`. (2) Inline material fields on `MeshRenderer` — no sharing, every entity edited separately. (3) A cooked binary material format — consistent with textures/models, but materials are tiny, hand-editable and change often; cooking adds a step without a size or speed benefit. (4) A material graph — the long-term direction (ROADMAP Phase 11), but it needs a shader compiler at import time first.

**Decision.** (1). Texture references are UUIDs so renames keep working; type errors in the file are reported (not silently defaulted) while missing keys keep defaults. The material replaces the mesh's own material; `MeshRenderer.base_color` stays a per-entity tint. The GPU asset cache memoizes results including failures, so a broken file is read once; broken or missing materials fall back to the component's own look and count as missing assets. UV scale/offset follow KHR_texture_transform's order (scale, then translate). Files are saved with 6 significant digits so hand-typed values stay readable.

**Evidence.** STATUS Slice 33: tiling 8× multiplies on-screen checker transitions; hot reload swaps an edited material without a restart; broken JSON and unknown UUIDs keep drawing; export and `victus check` reject bad references.

**Risks / revisit triggers.** No material inheritance/instances yet; shader variants are fixed to the built-in PBR shader. Revisit when project shaders or a material graph land (the file gains a `shader` field and parameters), or if material count makes per-file loading slow.

## ADR-0035 — Project surface shaders compiled at load time with glslang and SPIRV-Cross (accepted 2026-11-17)
**Problem.** Every mesh used the one built-in PBR shader; games could not author animated, stylised or procedural surfaces (energy fields, dissolves, holograms, water tints) without changing engine code.

**Candidates.** (1) Surface functions in GLSL spliced into the built-in PBR fragment shader and compiled when first used, with glslang and SPIRV-Cross linked into the engine (Godot's spatial shaders, where `fragment()` writes `ALBEDO`, `ROUGHNESS`, `EMISSION` etc. and the built-in renderer does the lighting, with a global `TIME`, https://docs.godotengine.org/en/stable/tutorials/shaders/shader_reference/spatial_shader.html). (2) Whole custom vertex+fragment shaders — full freedom, but every shader must re-implement shadows, SSAO, fog and lighting and breaks whenever the renderer changes. (3) Compile at import time with the `glslangValidator` CLI — needs the Vulkan SDK on every user's machine. (4) A node-based material graph — friendlier, but it needs (1)'s compiler path first.

**Decision.** (1). The `Surface`/`SurfaceInput` structs in `pbr.frag` are a stable contract; the surface runs after texture sampling and before lighting. Shaders cannot add resources — they get four per-material `vec4` parameters (`Material` UBO `custom[4]`) and time, which keeps the pipeline layout identical to the built-in shader so SDL_GPU resource counts never change. Compilation happens in `GpuAssetCache` (results and failures cached; errors fall back to built-in PBR), `victus check`/export compile every shader so broken shaders never ship, and hot reload recompiles. glslang 16.6.0 (released 2026-09-10, https://github.com/KhronosGroup/glslang/releases/tag/16.6.0) and SPIRV-Cross vulkan-sdk-1.4.363.0 are fetched by CMake with pinned hashes; glslang is built without HLSL, optimizer, tools or tests. MSL is generated only on Metal devices.

**Evidence.** STATUS Slice 36: params and time change on-screen pixels; a compile error falls back and recovers through hot reload; error messages carry the `.vshader` line number; the 3D template's glowing cube works in `victus run`, the editor and an exported game.

**Risks / revisit triggers.** glslang adds build time and binary size; first use compiles on the render thread. No D3D12 DXIL path (Vulkan is used on Windows). Revisit for a shader cache on disk/in packs, async compilation, vertex/sprite/post-process hooks, extra textures per shader, or a material graph that emits surface code.
## ADR-0037 — Packed vertex colour in Vertex3D; fog and coverage in the forward PBR shader (accepted 2026-11-17)
**Problem.** Procedural and stylised games (AURORIA first) colour meshes per vertex, need distance fog for atmosphere and a weather coverage layer (snow) masked per vertex; VICTUS had none of these.

**Candidates.** (1) One packed RGBA8 colour in every `Vertex3D` (4 bytes, default white so existing meshes are unchanged) plus uniform-driven fog and coverage in `pbr.frag`. (2) A separate optional colour vertex stream — no cost for meshes without colour but doubles pipeline variants. (3) Bake colours into textures — loses per-vertex authoring and needs UVs.

**Decision.** (1). RGB is stored sRGB-encoded (glTF COLOR_0 linear values are encoded on import) and decoded in the vertex shader; alpha is linear and either opacity or, per material, a coverage mask. Fog is applied in the forward pass after lighting; coverage blends albedo/roughness on faces whose normal.y exceeds a threshold.

**Evidence.** STATUS Slice 35.1 GPU probes.

**Risks / revisit triggers.** 4 extra bytes per vertex for all meshes; revisit if vertex bandwidth matters on mobile. Height fog and sky fog are not done.

## ADR-0036 — Material instances as sparse .vmat files with a parent reference (accepted 2026-11-17)
**Problem.** Variations of one look (team colours, damaged versions, shader parameter tweaks) required copying a whole material; a later change to the shared look had to be repeated in every copy.

**Candidates.** (1) A `"parent"` UUID in the same `.vmat` format, the file storing only the keys it overrides, resolved at load time (Unreal's Material Instance Constants inherit everything from a parent and change only exposed parameters without recompiling, https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-materials-in-unreal-engine). (2) A separate `.vmatinst` asset type — clearer in listings, but duplicates parsing, references and editor code for the same fields. (3) Parameters explicitly exposed by the parent (Unreal's model) — finer control, but every VICTUS material field is already a plain value, so the extra declaration step brings no benefit yet.

**Decision.** (1). Every field can be overridden; presence of a key is the override (so `null` textures override to none). Resolution happens in `AssetDatabase::resolve_material`, which the renderer cache, export and `check` share; chains are capped at 16 levels and cycles are errors. Because surface shaders read their values from material parameters, instances also vary shader looks without new shaders.

**Evidence.** STATUS Slice 37: three-level inheritance, propagation of parent edits through hot reload, rejected cycles/missing parents, and the editor's inherited/override markers.

**Risks / revisit triggers.** Resolution reads each file in the chain per cache miss (cheap for small chains). Revisit if parents need to lock fields, if run-time instances from scripts are needed, or if load times grow with deep chains (cache resolved parents).

### ADR-0037: Scene transitions are validated at the call and applied at the end of the update
- Status: Accepted (2026-11-17, Slice 38).
- Context: games need menus, levels and restarts; before this slice only the startup scene (or a checkpoint) could be loaded.
- Decision: `scene.load(path, params?)` reads and fully parses the target through `HostOptions::scene_loader` (default: `<project root>/<path>` through mounted packs) and returns `nil, error` when it is missing, outside the project or invalid; a valid request replaces the scene at the end of the current update (the newest of `scene.load`, `scene.reload` and `save.restore` wins). Every script instance is dropped and starts again; the single Luau VM and module-level state survive, and JSON-serialisable `params` reach the next scene via `scene.params()`. Hosts reset physics and audio through `on_scene_loaded`. Checkpoints store the scene path and params. Export/check reject literal `scene.load("...")` targets that are not project scenes.
- Consequences: transitions are deterministic (captures replay them) and work identically in `victus run`, exported games, `victus simulate` and editor play mode (which changes only its play copy). No additive or asynchronous loading yet: large scenes load synchronously within one frame.

### ADR-0038: Sprite animation uses grid sprite sheets (.vsheet) and a SpriteAnimator that writes Sprite UVs
- Status: Accepted (2026-11-17, Slice 39).
- Context: 2D games need animated characters and props; sprites could only show a whole texture.
- Candidates: (1) a JSON `.vsheet` asset that slices one texture into a grid and names clips, played by a `SpriteAnimator` component that sets the Sprite's texture and a new `uv` region (Godot's `Sprite2D` grid plus `SpriteFrames` clips, https://docs.godotengine.org/en/stable/classes/class_spriteframes.html); (2) one texture per frame — simple, but many textures break sprite batching; (3) packed atlases with per-frame rectangles — best texture usage, but needs a packer or external tool format.
- Decision: (1). Frames are numbered row by row; clips are explicit frame lists or `from`/`to` ranges with fps and loop. `AnimationSystem::update_sprites` runs inside `update()` (and with dt 0 in the editor for preview); a clip change restarts it; non-looping clips hold the last frame and report `finished`. Missing or invalid sheets leave the Sprite unchanged and are reported by `victus check`/`export`. Sheets hot-reload by clearing the system's cache.
- Consequences: every frame of a character shares one texture, so the batcher keeps one draw. Linear filtering can bleed neighbouring cells at frame edges; nearest filtering/insets and packed atlases remain open.

### ADR-0039: Texture filtering is an import setting read at draw time
- Status: Accepted (2026-11-17, Slice 40).
- Context: pixel-art sprites and sprite sheets need point sampling; linear filtering blurred them and bled neighbouring sheet cells. Materials already had a per-material `filter`, sprites had none.
- Candidates: (1) a `"filter"` key in the texture's import settings, read by the GPU asset cache when sprites and materials are drawn (Godot exposes texture filtering on the canvas item using the texture, Unity's Texture Import Settings carry a Filter Mode per texture); (2) a filter field on every Sprite component — flexible but repetitive for whole sheets; (3) bake a sampler into the cooked texture — the RHI binds samplers separately from textures, so this would still need a lookup.
- Decision: (1), with the sprite batch splitting batches by filter and materials using nearest when either the material or its base colour texture asks for it. The setting is not baked into the pixels; changing it re-imports (settings are part of the import key) and evicts the cache.
- Consequences: one switch per texture fixes pixel art and sheet bleeding everywhere it is used. Wrap mode stays fixed (sprites clamp, materials repeat).

### ADR-0040: External simulations run as project-declared helper processes; scripts build procedural meshes by id
- Status: Accepted (2026-11-17, Slice 41).
- Context: some games keep logic in an external program (a simulation server, an AI service, a tool written in another language) that speaks JSON lines, and some generate geometry at run time; VICTUS scripts are sandboxed and had no process, mesh-building or module APIs.
- Candidates: (1) projects declare named helpers in `victus.project` `bridges`; scripts can only open those names, so a script cannot run arbitrary commands; (2) a generic `os.execute`-style API — breaks the sandbox; (3) embed the .NET runtime — heavy and specific to one game. For meshes: (a) a runtime `ProceduralMeshComponent` holding an immutable shared mesh with a unique id that the GPU cache uploads once and collects when unused (Unity `Mesh.SetVertexBufferData` copies packed vertex data in a declared layout, https://docs.unity3d.com/ScriptReference/Mesh.SetVertexBufferData.html; Godot `ArrayMesh.add_surface_from_arrays`, https://docs.godotengine.org/en/stable/classes/class_arraymesh.html); (b) write temporary glTF assets — slow and pollutes the project.
- Decision: (1) and (a). Script layouts describe packed vertices (`f32x2/3`, `s8x4`, `u8x4`, `skip:N`) so bridge buffers decode without per-vertex Luau work; mirroring `scale` converts left-handed data and flips winding. `require` shares module tables across scripts. Time spent waiting on a bridge does not count towards the script time budget.
- Consequences: games with external simulations ship the helper next to the game and declare it; procedural meshes are runtime-only (not saved, no colliders yet).

### ADR-0041: Tilemaps store sparse cells and build merged static boxes for 2D physics
- Status: Accepted (2026-11-17, Slice 42).
- Context: 2D games need grid levels that are painted in the editor or generated by scripts, drawn efficiently, and collide with the 2D character mover and bodies; VICTUS had sprites and Box2D colliders but no grid representation.
- Candidates: (1) a `Tilemap` component with a sparse cell map (any size, negative coordinates, scripts paint freely) plus `.vtileset` assets describing the texture grid, per-tile collision and terrain rules (Godot TileSet atlases, physics layers and terrain sets, https://docs.godotengine.org/en/stable/tutorials/2d/using_tilesets.html); (2) dense fixed-size chunks — faster iteration but resizing and sparse levels cost memory; (3) one entity per tile — simple but floods the scene and physics. For collision: (a) merge solid cells into rectangles on one static body (Unity merges tile shapes with a Composite Collider, https://docs.unity3d.com/Manual/tilemaps/work-with-tilemaps/tilemap-collider-2d-reference.html); (b) one Box2D body per tile; (c) chain shapes around outlines — fewer shapes but harder with one-way tiles and holes.
- Decision: (1) and (a). Physics gains a generic runtime-only `StaticShapes2DComponent` so the physics module does not depend on tilemaps; tile sets autotile with a 16-entry side-neighbour mask; one-way tiles never merge vertically so only their top surface supports.
- Consequences: painting rebuilds one body per map (cheap for typical levels; very large maps rebuild whole). Slopes and per-tile polygons need a later extension of the box list; drawing is per cell without culling.

### ADR-0042: Image-based lighting bakes on the GPU with fragment passes and uses SH9 for diffuse
- Status: Accepted (2026-11-17, Slice 44).
- Context: high-end look needs environment lighting and reflections from real HDR skies; the target includes entry-level Android GPUs where SDL_GPU compute support, bandwidth and memory are limited.
- Candidates: prefilter (1) offline in the cooker, (2) on the CPU at load, (3) on the GPU at load with fragment passes, (4) GPU compute. Diffuse from (a) an irradiance cubemap or (b) 9 spherical-harmonic coefficients (Ramamoorthi & Hanrahan 2001, https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf). Specular follows the split-sum approximation with GGX importance sampling (Karis 2013, https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf) and filtered importance sampling (Křivánek & Colbert, GPU Gems 3 ch. 20, https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling).
- Decision: (3) + (b). The GPU bake runs on every SDL_GPU backend (no compute requirement), takes one frame of work, and its size/sample count scale per quality tier (256/64, Low 128/32); SH9 is computed on the CPU at cook time and costs nine multiply-adds per pixel instead of a second cube fetch. Cubes are RGBA16F; the cooked texture stores half-float equirect pixels (licence: stb, public domain/MIT, already vendored; no new dependency).
- Consequences: changing an environment needs a re-bake (cheap); dynamic time-of-day (next slice) re-bakes a small cube. Revisit (1) if load time on phones matters more than package size, and add a BRDF LUT if the analytic split-sum approximation shows artefacts.

### ADR-0043: Physical sky is evaluated on the CPU into a small equirect and baked through IBL
- Status: Accepted (2026-11-17, Slice 45).
- Context: time of day must change sky, ambient and reflections together, and stay cheap on low-end phones.
- Candidates: (1) per-pixel ray marching in the sky shader (Nishita) — cost every frame and does not light the scene; (2) precomputed LUTs (Bruneton & Neyret 2008, https://inria.hal.science/inria-00288758; Hillaire 2020, https://sebh.github.io/publications/egsr2020.pdf) — best quality incl. multiple scattering but needs compute or several render passes per change; (3) single-scattering evaluation of a 128x64 equirect on the CPU when the sun moves, fed into the existing GPU prefilter + SH9 path.
- Decision: (3). It reuses ADR-0042, costs nothing per frame, works on every backend, and keeps sky, IBL and sun colour consistent. Coefficients from Hillaire 2020. No new dependency.
- Consequences: no multiple scattering (compensated by `sky_gain`); re-bakes cost ~21 ms (5 ms on Low) and happen only on ~0.5° sun moves. Revisit with (2) when compute paths exist and for planet-scale views.

### ADR-0044: Reflection probes are captured through readback into the existing IBL bake
- Status: Accepted (Slice 46)
- Context: local reflections need scene captures prefiltered like the global environment.
- Options: (a) render cube faces on the GPU and blit into cube layers, then prefilter in place; (b) render six faces, read back, convert to equirect and reuse `EnvironmentMap::create` (ADR-0042). Per-draw nearest probe versus per-pixel blending of several probes.
- Decision: (b), because it reuses the verified prefilter/SH path with no new GPU resource types; the stall is bounded by a capture budget (default 1 per frame) and captures only happen on change. Each draw uses the smallest containing axis-aligned box (one probe binding, keeps batching), with Lagarde and Zanuttini 2012 box projection for specular and the probe SH9 for diffuse.
- Consequences: no seams between probes are hidden yet (no blending/fade); a GPU-only capture path can replace (b) later without changing the component or shader interface.

### ADR-0045: TAA reprojects from depth; dynamic resolution uses CPU frame time
- Status: Accepted (Slice 47)
- Context: TAA needs per-pixel history positions. Full motion vectors need an extra render target written by every opaque pipeline (built-in, skinned, surface shaders) plus previous-frame model matrices and skin palettes.
- Options: (a) per-object motion vector MRT; (b) camera-only reprojection from depth with neighbourhood variance clipping (Karis 2014, Salvi 2016) bounding errors on moving objects; (c) FXAA/SMAA only.
- Decision: (b) now. It adds one fullscreen pass and two history textures, changes no material pipeline, and is correct for the dominant case (camera motion over static geometry); clipping removes ghosts from moving objects at the cost of some softening. The shader interface (reprojection per pixel) lets (a) be added later by replacing the depth-derived position with a velocity sample. TAA is enabled at High/Ultra; Low/Medium keep FXAA because the extra full-resolution RGBA16F read/write is a large share of bandwidth on entry-level phones.
- Dynamic resolution measures CPU frame time (SDL_GPU exposes no GPU timestamps), scales by sqrt(target/measured) with a cooldown, and recovers in small steps after sustained headroom to avoid oscillation. Fixed-step, recorded and replayed runs keep a constant scale so captures stay deterministic.

### ADR-0046: Clustered forward lighting with CPU light assignment
- Status: Accepted (Slice 48)
- Context: the renderer is forward (needed for transparency, cheap MSAA and tile-based mobile GPUs) and limited to 16 lights in a uniform buffer.
- Options: (a) deferred shading; (b) tiled forward (2D tiles, depth bounds from a pre-pass); (c) clustered forward (Olsson et al. 2012) with 3D clusters, no depth pre-pass dependency; light assignment on the CPU or in a compute shader.
- Decision: (c) with a 16x9x24 grid, logarithmic slices and CPU assignment. Clusters do not need the depth pre-pass (which the Low tier skips), work for transparent surfaces, and keep a single material path. CPU assignment is a few hundred microseconds for hundreds of lights, avoids compute shaders (unavailable or slow on some mobile drivers) and is unit-testable for conservativeness. Lights and the cluster list live in fragment storage buffers, which SDL_GPU supports on Vulkan, Metal and D3D12.
- Consequences: very high light counts (thousands, many per cluster) should move assignment to compute; the per-cluster cap (128) drops the farthest lights in extreme overlaps and is reported in stats.

### ADR-0047: Cooker-side BC and ASTC variants with runtime selection
- Status: Accepted (Slice 49)
- Context: textures need GPU block compression for memory and bandwidth, especially on phones. SDL_GPU supports BC1-7 and ASTC LDR but no ETC2/EAC formats, and compressed textures cannot have mips generated at run time.
- Options:
  - Encoders: (a) Basis Universal / KTX2 supercompression with transcoding at load; (b) direct encoders: bc7enc_rdo (MIT/Unlicense) for BC1/BC3/BC7 and Arm astcenc (Apache-2.0) for ASTC; (c) GPU compute encoders.
  - Variant handling: one format per platform at export time, or several variants per cooked file with selection at load.
- Decision: (b), with variants inside the cooked file, selected at load by `supports_format`, falling back to RGBA8.
  - Direct encoders target each GPU format natively, so there is no intermediate-format quality loss and no runtime transcoder.
  - Both libraries are small, permissively licensed and portable C++.
  - Keeping several variants in one cooked file lets the editor, desktop player and phone share a cache. ETC2 is skipped because SDL_GPU cannot create it; ASTC LDR is the common compressed format of Vulkan-capable mobile GPUs (Mali, Adreno, PowerVR, Apple).
  - Compression is opt-in per asset for now, so existing projects and pixel-exact tests are unchanged.
- Consequences:
  - Cook time and cached size grow with each variant. Exports should strip the variants a platform cannot use (follow-up).
  - Revisit (a) if download size dominates over load time.
  - HDR textures stay uncompressed until BC6H/ASTC HDR are added.

### ADR-0048: Android app as an SDLActivity shell around the native player
- Status: Accepted (Slice 50)
- Context: VICTUS must run on entry-level Android phones. The engine is C++ on SDL3 and SDL_GPU, and exported games are `game.vpak` packs opened by file path. SDL_GPU on Android has only a Vulkan backend (no OpenGL ES).
- Options:
  - App shell: (a) SDL's `SDLActivity` Java glue, with the engine in `libmain.so`; (b) `GameActivity` (AGDK) with our own window, input and audio glue; (c) `NativeActivity` with no Java.
  - Build: (a) Gradle with AGP `externalNativeBuild` driving the root CMake project; (b) a standalone CMake + `aapt2`/`d8`/`apksigner` script.
  - Pack access: read the pack straight from the APK through `AAssetManager`, or install it once to internal storage.
- Decision: SDLActivity with Gradle/AGP 9.5.1, and install-once to internal storage.
  - SDLActivity already implements the window, touch, gamepads, audio, IME and lifecycle events the platform layer consumes, and it ships with the SDL release we pin.
  - The Java glue is extracted from that same hash-checked tarball, so desktop and Android cannot drift. SDL is built shared on Android so its JNI exports survive linking.
  - AGP is the supported path for signing, Play bundles and Android Studio, and it builds the unchanged root CMake project.
  - Installing the pack keeps `pack::Pack` path-based, with one code path on every platform. The copy is streamed, atomic and skipped when the size and header (TOC hash) match.
  - Vulkan 1.1 is declared as a required manifest feature, so stores filter out devices SDL_GPU cannot drive.
  - The player picks `mobile_quality` (default medium) and `mobile_target_fps` (default 30, dynamic resolution) on phones.
- Consequences:
  - The pack takes its size twice on disk, and the first launch pays one copy. Revisit with `AAsset_openFileDescriptor`-based reads of the stored (uncompressed) asset if download size or first-launch time matters.
  - Only arm64-v8a is built. Devices without Vulkan 1.1 are unsupported until SDL_GPU gains a GLES backend.
  - No device or emulator run is verified yet. CI verifies the APK structure, exports and 16 KB alignment, and host tests cover the install and lifecycle logic.
  - GameActivity remains an option if SDL's Java layer becomes limiting (Game Mode, Performance Hint, predictive back).

### ADR-0049: Mesh optimisation and discrete LODs with meshoptimizer
- Status: Accepted (Slice 54)
- Context: imported meshes keep the glTF exporter's triangle and vertex order, and distant objects draw every triangle. Phones are vertex-bound well before desktop GPUs are.
- Options:
  - Library: (a) meshoptimizer (MIT, small portable C++, vertex cache/overdraw/fetch optimisers and an attribute-aware quadric simplifier); (b) our own quadric edge collapse; (c) offline DCC-authored LODs only.
  - Representation: (a) discrete LOD index lists sharing one vertex buffer; (b) per-LOD vertex buffers; (c) clustered (meshlet/Nanite-style) continuous LOD.
- Decision: library (a), representation (a).
  - The importer runs `generate_lods` (default 3 levels, each half the previous triangle budget, error at most 5% of the mesh extent; positions, normals, UVs and, for skinned meshes, the dominant joint are weighted) and then `optimize_mesh` (vertex cache, overdraw within 5% extra misses, fetch remap). Settings live in the model `.meta` (`optimize_meshes`, `lods`, `lod_error`) and are only written when changed.
  - Every level is simplified from the full mesh, so errors do not accumulate, and stores its geometric error in mesh units. The renderer projects that error with the camera's vertical pixel scale, the object's largest axis scale and the distance to its bounds, and picks the coarsest level within `lod_error_pixels` (1 px by default, 2 Low, 1.5 Medium, 0.5 Ultra).
  - All levels share the vertex (and skin) buffers and are concatenated into one index buffer, so a LOD switch is a different `first_index`/`index_count` and instancing still works (batches split by level).
  - Collision, navigation and CPU readers keep using LOD 0 (`MeshData::indices`).
- Consequences:
  - Index memory grows by about the sum of the level sizes (under 1x); the cooked format moves to v6 and the model importer to v5, so caches re-import once.
  - Switches pop (no cross-fade or dithering yet); the 1 px default keeps this below visibility on tests.
  - GPU-driven culling and indirect draws are a separate step (SDL_GPU has indirect draws, but no draw count buffer), as are occlusion culling and clustered LOD.

### ADR-0050: Optional packed HDR colour (R11G11B10F) for bandwidth-bound GPUs
- Status: Accepted (Slice 55)
- Context: entry-level phones (e.g. Mali-G52 MC2 on a shared LPDDR4X bus) are limited by framebuffer bandwidth, not shader ALU. The HDR scene colour, bloom chain and TAA history were always RGBA16_FLOAT (8 bytes per pixel), and nothing in that chain uses alpha or negative values: blending uses source alpha, FXAA reads luma from the tonemapped LDR target, and tonemap, bloom and TAA read only RGB.
- Options: (a) keep RGBA16F everywhere; (b) R11G11B10_FLOAT always; (c) R11G11B10_FLOAT opt-in per renderer.
- Decision: (c). `SceneRendererOptions::compact_hdr` selects R11G11B10_FLOAT for those targets and their pipelines when the device can sample and render to it, else RGBA16F.
  - It halves colour traffic per pass on tiled GPUs. Its 6/6/5-bit mantissas sit well below tonemapped 8-bit output in practice, which is why most console engines use the same format.
  - Opt-in keeps desktop output and pixel-exact tests unchanged. Environment maps and reflection captures stay RGBA16F because they are filtered and reused many times.
- Consequences: the format is fixed when the renderer is created (pipelines are built then). Games enable it on phones; AURORIA does so in its phone profile (D-114).

### ADR-0051: GPU occlusion culling with a Hi-Z pyramid and indirect draws
- Status: Accepted (Slice 55)
- Context: the CPU frustum-culls and instances draws, but everything inside the frustum is shaded, even when a wall hides it. SDL_GPU 3.4 has compute pipelines and indirect draws, but no draw-count buffer (no multi-draw-indirect-count) and, unless the device feature is requested, no non-zero `first_instance` in indirect commands.
- Options:
  - Occluders: (a) the depth pre-pass of the current frame; (b) last frame's depth reprojected (two-phase); (c) CPU software rasterised occluders; (d) hardware occlusion queries.
  - Pyramid storage: (a) a storage buffer with every level packed; (b) a mipped R32F storage texture.
  - Output: (a) one indirect command per CPU batch with a GPU-written instance count; (b) a fully GPU-built draw list (needs draw-count buffers).
- Decision: occluders (a), storage (a), output (a).
  - When the pre-pass runs (SSAO or `depth_prepass`), `hiz_depth.comp` writes the farthest depth of each 8x8 tile, and `hiz_reduce.comp` halves it to 1x1 (one compute pass per level, so SDL orders the writes). `occlusion_cull.comp` projects each opaque main-pass instance's world AABB with the same (TAA-jittered) matrix, picks the coarsest level where the rectangle spans at most 2 tiles per axis and keeps the instance if its nearest depth is not behind the farthest Hi-Z depth (plus a depth-format tolerance). Boxes crossing the camera plane and partly off-screen rectangles are kept; fully off-screen ones are dropped.
  - Survivors are appended with an atomic counter to the batch's indirect command, packed from the batch's own instance offset, so commands use `first_instance = 0` and the draw binds the instance stream at that offset.
  - A buffer pyramid needs no extra texture formats or storage-texture usage, which keeps the same path on Vulkan, Metal and D3D12, and phones.
  - The pre-pass, shadows and transparent draws are unchanged. Only the main (shaded) pass is culled, so the depth buffer, and therefore the image, is identical with culling on or off.
  - RHI additions: `ComputePipelineDesc`/`create_compute_pipeline`, `ComputePass` (pipelines, read-only storage buffers, samplers, dispatch), read-write buffers per compute pass, compute uniforms, `RenderPass::draw_indexed_indirect`, `Device::read_buffer`, `BufferUsageComputeRead`, `DeviceCapabilities::compute`, dispatch and indirect-draw stats. The shader generator reflects compute workgroup sizes and read-write counts, and only recompiles shaders whose hash changed.
- Consequences:
  - Occluders are only pre-pass geometry (opaque, not alpha-tested or surface-shaded). Without a pre-pass (Low tier without SSAO) there is no culling.
  - Fragment shading is saved, but vertex work for hidden objects remains in the pre-pass. A two-phase, previous-frame Hi-Z variant could also cull the pre-pass and shadows later.
  - CPU batching still runs; there is no GPU frustum culling or GPU LOD selection, and indirect draws are one call per batch (no draw-count buffers in SDL_GPU).

### ADR-0052: Local light shadows in one perspective shadow atlas
- Status: Accepted (Slice 56)
- Context: spot and point lights (clustered since Slice 48) lit through walls. Scenes have many local lights, phones have little bandwidth, and SDL_GPU exposes no cube-array depth rendering or geometry shaders.
- Options:
  - Storage: (a) one 2D depth atlas with a fixed tile grid; (b) cube depth textures per point light plus 2D maps per spot; (c) dual-paraboloid maps for point lights.
  - Selection: (a) per-light opt-in plus a per-frame budget ranked by screen coverage; (b) every light.
- Decision: storage (a), selection (a).
  - `RenderSettings::local_shadow_atlas` / `local_shadow_tile` define a grid of at most 8x8 tiles (High 2048/512, Medium 1024/256, Ultra 4096/512, off on Low). Spot lights take one perspective tile (field of view twice the outer angle, up to 85 degrees). Point lights take six 90-degree tiles, picked in the shader by the major axis of the light-to-surface vector, so no cube textures are needed.
  - Lights opt in (`PointLight/SpotLight::cast_shadows`; scenes map `LightComponent::shadows`). Candidates in the view are ranked by range divided by the distance to their sphere, and tiles go out in that order; lights that do not fit stay unshadowed.
  - The world-to-tile matrices (atlas bias included) are appended to the clustered light buffer with the same 64-byte stride, so the PBR shader gains only one sampler (`u_local_shadow`) and no new buffer. Each light stores its first tile and its shadow texel angle; sampling uses a normal offset of about 1.5 texels at the surface's distance, the raster depth bias of the sun shadow pipeline, and the sun's 3x3 PCF setting, clamped inside the tile.
  - Casters are culled per tile on the CPU and batched like cascades; the shadow pipelines are shared with the sun.
- Consequences:
  - Every shadowed light is re-rendered every frame (no caching of static lights yet), and the atlas resolution is per tile, not per light.
  - Cube-face seams are possible at grazing angles because PCF clamps to each tile. Translucent and alpha-tested casters are not supported in local shadows yet (same as the sun pass).
  - Budget changes switch lights between shadowed and unshadowed without a fade.

### ADR-0053: Flipbook frames and stretched billboards for particles
- Status: Accepted (Slice 56)
- Context: games feed `submit_particles` from their own particle logic (AURORIA evaluates its stateless smoke, fire, spark, rain and snow formulas on the CPU). Smoke and flame textures are 4x4 flipbooks, and sparks and rain read as streaks along their motion; billboards could only show a whole texture as a rotated square.
- Options: (a) one texture per flipbook frame (a batch per frame); (b) per-material flipbook settings; (c) per-instance frame, grid and height/width in the reserved `ParticleInstance::params` fields.
- Decision: (c). `params.y` is the frame (left to right, top to bottom), `params.z` the grid (columns x 16 + rows; 0 = whole texture) and `params.w` the height/width ratio along the rotated up axis (0 or 1 = square). The particle vertex shader applies them; the instance layout (48 bytes) is unchanged.
- Consequences: one batch per effect regardless of frame; zeroed reserved fields keep existing emitters exactly as before. No frame blending (frames switch); grids up to 15 x 15.

## ADR template
Date/status; problem/constraints; candidates; exact versions and primary sources; license/platform matrix; prototype/benchmark method/results; decision/rejected options; risks/rollback/revisit trigger.

## ADR-0038 — Blended materials as a sorted sub-pass of the forward pass (accepted 2026-11-17)
- Context: water, glass, decals and glows need alpha and additive blending; the forward renderer only had opaque meshes and separate particle passes.
- Decision: `Material::blend` (`.vmat` `"blend": "opaque" | "alpha" | "additive"`, inheritable by instances). Blended draws skip the depth pre-pass and shadow casting, are sorted far to near by bounds centre, and are drawn at the end of the forward pass (same lighting, fog and coverage uniforms) with depth test on and depth write off. Pipelines are created on first use per blend × skinned × double-sided, and surface shaders get the same variants. Surface shaders also read the interpolated vertex colour (`SurfaceInput::vertex_color`), so meshes can carry per-vertex data for them.
- Consequences: straight (non-premultiplied) alpha; per-object, not per-triangle, sorting — intersecting blended meshes can still order wrongly; no order-independent transparency, refraction or screen-space reflections; blended draws do not appear in SSAO or the depth used by soft particles.

