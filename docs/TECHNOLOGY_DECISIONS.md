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

## ADR template
Date/status; problem/constraints; candidates; exact versions and primary sources; license/platform matrix; prototype/benchmark method/results; decision/rejected options; risks/rollback/revisit trigger.
