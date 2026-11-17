# VICTUS Roadmap

Living, evidence-driven plan. Work directly on `main` in small, buildable commits. Status reflects verified repository state, never ambition.

**Current maturity (2026-11-17, through Slice 56):** core runtime, SDL3 platform layer, RHI on SDL_GPU (Vulkan verified), render graph, 2D sprite renderer and forward HDR PBR 3D renderer (cascaded and local light shadows, SSAO, bloom, auto exposure, FXAA, instancing, mesh LODs, GPU occlusion culling, quality tiers, materials, surface shaders, material instances, transparency, fog), archetype ECS, versioned scenes and prefabs, asset pipeline (textures, glTF meshes/skins/animations, cooked cache), VICTUS Studio editor (play-in-editor, multi-client play, material editor, tile painting, input actions), Luau scripting with hot reload, Jolt/Box2D physics with characters and joints, skeletal and sprite animation, CPU particles, in-game UI, miniaudio audio, Recast/Detour navigation, tilemaps, save games, deterministic captures, encrypted server-authoritative multiplayer (ENet, prediction, interpolation, delta snapshots, listen/dedicated servers, reconnect), `victus` CLI and `victus export` packaging, plus two complete sample games (2D `samples/dungeon_crawler`, 3D `samples/arena`). An Android arm64 APK build exists (CI-built, not yet run on a physical device). Not yet production-ready: no iOS build, no GI, no visual scripting.

## Direction
VICTUS is a general-purpose engine, not an Auroria-specific one. Auroria remains a demanding showcase only. The defining product goal is **high-end visual quality that scales down to entry-level Android phones**: every rendering feature must have a cheap mobile path (or a documented Low-tier fallback), favour bandwidth-light techniques on tile-based GPUs, and be measured rather than assumed.

## Next slices (priority order, updated 2026-11-17)
1. [x] (Slice 44) Image-based lighting: HDR environment maps (equirect `.hdr` → cubemap, GGX-prefiltered specular mips, SH9 irradiance), environment-lit sky background, per-scene `Environment.environment_map`.
2. [x] (Slice 45) Physically based sky/atmosphere baked into the environment cubemap (dynamic time of day feeding IBL).
3. [x] (Slice 46) Local reflection probes (box-projected, captured from the scene, baked into cubemaps).
4. [x] (Slice 47) Temporal anti-aliasing with jittered projection and depth-based reprojection; dynamic resolution driven by frame time. Follow-up: per-object motion vectors for skinned/moving meshes, temporal upscaling.
5. [x] (Slice 48) Clustered forward lighting (hundreds of point/spot lights) with spot lights. Follow-up: local light shadows.
6. [x] (Slice 49) GPU texture compression in the cooker: BC7/BC1(BC3) for desktop, ASTC 4x4/6x6 for mobile (SDL_GPU has no ETC2), cooked as variants with runtime selection and RGBA8 fallback. Follow-up: per-platform stripping in `victus export`, project-wide default.
7. [x] (Slice 50) Android arm64 build: NDK + Gradle APK (`platforms/android`, AGP 9.5.1) that packages an exported `game.vpak`, touch input for scripts, background/foreground lifecycle, mobile quality defaults, a required Vulkan 1.1 manifest feature, and a CI job that builds and checks the APK of the 3D template. Follow-up: physical-device runs, on-screen virtual controls, thermal/memory profiling.
8. [x] (Slice 51) Editor transform gizmos and snapping (move/rotate/scale, world/local, snapping, undo, 2D and 3D); (Slice 52) asset thumbnails (rendered model/material/prefab previews, texture/sprite sheet/tile set images, list and grid views). Follow-up: on-disk thumbnail cache, pivot/centre toggle, absolute grid snapping.
9. [x] (Slice 53) 3D sample game: `samples/arena` third-person robot arena (five navmesh-driven enemy waves, melee, health pickups, HUD, checkpoints, autopilot CTest `victus_sample_arena`). Follow-up: title menu, cinematic sequence, behaviour-tree AI.
10. [x] (Slice 54) Mesh LODs and vertex/index optimisation via meshoptimizer: import-time optimisation, automatic LODs in cooked models, screen-space error LOD selection in every pass, per-level instancing; (Slice 55) GPU occlusion culling (Hi-Z pyramid from the depth pre-pass, compute cull, indirect draws). Follow-up: LOD cross-fade, two-phase culling of the pre-pass and shadows.
11. [x] (Slice 56) Local light shadows: spot (1 tile) and point (6 tiles) perspective shadow maps in one atlas, opt-in per light, budgeted by screen coverage, off on Low. Follow-up: caching static lights, per-light resolution.
12. [ ] (Slice 57) Per-object motion vectors for moving and skinned meshes (TAA ghosting), then temporal upscaling.
13. [ ] Scalable global illumination (baked irradiance probes first, then a dynamic option on High/Ultra).
14. [ ] Visual scripting graph over the Luau runtime (editor graph, compile to Luau, debugging).

Legend: [x] verified · [~] in progress/partial · [ ] not started · [!] blocked.

## Phase 0 — Foundation and project quality
- [x] Project identity, principles, repository layout, agent contract, roadmap, architecture and license policy.
- [x] Minimal C++20 core/runtime/test slice and initial CMake/desktop CI.
- [x] Verify hosted CI; fix any failures. (Linux/Windows/macOS Debug+Release, ASan+UBSan and TSan lanes.)
- [~] Formatting, static analysis, sanitizers, warning policy and dependency scanning. (Sanitizer CI lanes, -Werror policy and .clang-format done; clang-tidy and dependency scanning pending.)
- [ ] Reproducible developer setup, presets, troubleshooting and contribution workflow.
**Exit:** clean Debug/Release build and tests on supported desktop CI.

## Phase 1 — Runtime, platform layer and diagnostics
- [~] Lifecycle, ownership, shutdown ordering, error/result types and validated configuration. (Result/Error, JSON config parsing done.)
- [x] Structured logging, diagnostic events, crash context, build metadata and tracing.
- [~] Platform services: filesystem, paths, clock, threads, window, input, device capabilities and lifecycle. (SDL3 windows, keyboard/mouse/gamepad/touch input, lifecycle events, clipboard, GPU capabilities implemented; mobile lifecycle untested on devices.)
- [~] CLI modes: editor, game, headless, cook, test, benchmark and package. (`victus new/templates/info/import/run/check/simulate/export`, `victus_editor`, `victus_player` done; `simulate` is the headless mode and `export` packages; a separate cook-only command is pending.)
- [~] Stable project configuration and runtime/editor/server target separation. (Versioned `victus.project`; separate editor and player targets; dedicated headless server via `victus serve` (Slice 17).)
**Exit:** predictable lifecycle and useful diagnostics in graphical and headless modes.

## Phase 2 — Performance laboratory
- [x] Reproducible benchmark harness with warm-up, repetitions, machine/build metadata and machine-readable output.
- [ ] CPU/GPU profiling, frame/tick histograms, p50/p95/p99, memory accounting and allocation tracking.
- [~] Workloads for ECS, jobs, transforms (done: `victus_bench_scene`, `victus_bench_script`, `victus_bench_physics`, `victus_bench_audio`), animation, visibility, rendering, asset import, streaming, networking and saves.
- [ ] Desktop baselines; Android physical-device baselines when hardware is available.
- [ ] Performance budgets and regression gates based on measured reference hardware.
**Exit:** another developer can reproduce the published results and compare equivalent workloads.

## Phase 3 — Core simulation, ECS and jobs
- [ ] Benchmark custom SoA/archetype approaches against Flecs and EnTT on VICTUS workloads.
- [~] Compare task schedulers and define dependency, affinity, cancellation and priority semantics. (VICTUS work-stealing JobSystem + TaskGraph with priorities implemented and benchmarked; external comparison pending.)
- [x] VICTUS-owned ECS/system API or documented reason to expose a dependency. (`engine/ecs`, ADR-0004.)
- [~] Transform hierarchy (done: `engine/scene`), spatial indexes, events, deterministic random streams and controlled simulation clock.
- [~] Versioned snapshots, serialization (done: `victus.scene` v1 JSON), migrations and headless simulation host.
- [ ] Simulation LOD: full entities, simplified entities, regional aggregates and event-driven dormant regions.
- [ ] Invariant tests for populations, resources, ownership, time, relationships and event ordering.
**Exit:** headless simulation saves, reloads, replays and passes correctness/performance workloads.

## Phase 4 — Graphics API and rendering foundation
- [x] Research and record backend choices: Vulkan, D3D12, Metal, OpenGL/ES or other justified fallback; compare specialist abstractions. (ADR-0003: SDL_GPU behind VICTUS RHI.)
- [~] VICTUS-owned RHI, capability model, device/swapchain/resource lifetime, synchronization and diagnostics. (Implemented and tested on Vulkan; Metal/D3D12 unexecuted.)
- [~] Render graph, frame resources, shader compilation/cache/reflection and GPU profiling. (Render graph with culling/aliasing/transient pool, offline shader pipeline with reflection done; GPU timing pending.)
- [x] Real 2D and 3D render paths; validated geometry, textures, materials and camera. (SpriteBatch; forward PBR with shadow map, point lights, sky, tonemap; pixel-verified tests.)
- [ ] Backend/device-loss recovery and compatibility fallbacks.
**Exit:** actual 2D and 3D scenes render on the validated desktop backend; mobile path has a verified capability plan.

## Phase 5 — Asset pipeline and content cooking
- [~] Stable asset IDs, metadata, dependency graph and versioned source-to-cooked format. (UUID `.meta` sidecars + versioned cooked cache done; cross-asset dependency graph pending.)
- [~] glTF 2.0/GLB import, validation, mesh/material/skin/animation/image/HDR handling. (Meshes, PBR materials, images, node hierarchy, skins and animation clips done (Slice 12); morph targets pending.)
- [ ] Evaluate authoring interchange such as OpenUSD separately from runtime delivery formats.
- [~] Texture mips, color management (done), platform compression and asset variants. Done (Slice 49): cooker-built mip chains with BC1/BC3/BC7 and ASTC 4x4/6x6 variants per texture (import setting `compression`, model setting `texture_compression`), device-capability selection with RGBA8 fallback. Remaining: per-platform stripping on export, project-wide default, BC5/two-channel normal maps, HDR (BC6H/ASTC HDR) textures.
- [~] Mesh optimization, LODs, collision proxies and import validation. Done (Slice 54): meshoptimizer vertex cache/overdraw/fetch optimisation and automatic LODs at import, validated cooked LODs. Remaining: collision proxies, import validation reports.
- [~] Content-addressed cache, incremental cook, dependency invalidation and deterministic outputs. (Hash-keyed cooked cache with up-to-date checks done.)
- [ ] Background import, cancellation, progress, recoverable errors and asset diagnostics.
- [ ] External DCC workflow and Blender interchange/export presets.
**Exit:** import, validate, cook, cache, hot-reload and load real project assets on a clean machine.

## Phase 6 — VICTUS Studio editor core
- [~] Project launcher/templates, dockable workspace, command palette and saved layouts. (Launcher, 3 templates, docking workspace with persisted ImGui layout done; command palette pending.)
- [~] Scene/world viewport, camera, hierarchy, inspector, selection, gizmos and snapping. (Viewport, 2D/3D editor camera, hierarchy with drag reparent, inspector for every component, click picking done; Slice 51: move/rotate/scale gizmos with world/local space, snapping, one undo entry per drag, prefab overrides, 2D and 3D.)
- [~] Undo/redo command model, multi-selection, autosave and crash recovery. (Snapshot-based undo/redo with dirty tracking done; multi-selection/autosave pending.)
- [~] Asset browser, metadata/dependency inspector, import queue and previews. (Asset table with drag/drop instantiate, material/texture assignment, prefabs, tile sets and reimport done; Slice 52: thumbnails for textures, sprite sheets, tile sets and rendered previews of models, materials and prefabs, list and grid views, refreshed on reimport/hot reload/material save. Dependency inspector and import queue pending.)
- [~] Logs, entity diagnostics, profiler, project settings and build configuration. (Console over the log ring sink, Network window, rendering-quality selector and draw-call readout done; profiler panel pending.)
- [x] Isolated play-in-editor with safe play/pause/step/stop and authored-world protection.
**Exit:** create project, import assets, author a scene, save/reopen and play without editing engine source.

## Phase 7 — Complete 2D development
- [~] 2D camera, sprites, atlases, batching, sorting/layers and pixel-art controls. Done: camera, sprites, batching, layers, grid atlases with `Sprite.uv` (Slice 39). Done (Slice 40): per-texture nearest filtering for sprites/particles/materials and 2D camera pixel snap.
- [x] Script-driven procedural meshes, helper-process bridges (`bridge.open`), `json`, `require` and mouse wheel input (Slice 41).
- [x] Tilemaps, tile sets, autotiling, grid and 2D world editing. Done (Slice 42): `.vtileset` tile sets, `Tilemap` component, 16-mask terrain autotiling, merged Box2D tile colliders (solid and one-way), Luau tile API. Done (Slice 42.1): Create Tile Set, Tile Palette, viewport paint/erase/rectangle/terrain/pick with undo per stroke and grid overlay.
- [x] 2D physics, collision shapes, queries and debug visualization: Box2D 3.2.0 bodies, box/circle/capsule colliders, triggers, raycast/overlap queries, editor collider overlay (Slice 6); 2D character mover, one-way platforms and 2D joints (Slice 14).
- [~] 2D animation, skeletal/rigged workflows where justified, particles and parallax. Done: particles (Slice 13), sprite sheets and `SpriteAnimator` clips (Slice 39). Remaining: parallax, rigged 2D.
- [~] 2D UI, input, audio, scene transitions and 2D lighting/effects. Done: input, audio (Slice 7), in-game UI (Slice 15), scene transitions (`scene.load/reload`, Slice 38). Remaining: 2D lighting/effects.
- [~] 2D templates, profiling, export and playable reference game. Done: 2D template, `victus export`; Slice 43: `samples/dungeon_crawler` top-down maze game (procedural tilemap floors, top-down `CharacterController2D`, autopilot CTest). Remaining: 2D platformer sample, profiling tooling.
**Exit:** build and ship a complete 2D game using only supported VICTUS workflows.

## Phase 8 — Complete 3D renderer and world systems
- [~] PBR materials, physically meaningful units/color management, forward/clustered baseline and justified alternatives. Done (Slice 33): `.vmat` material assets (PBR factors, base colour/metallic-roughness/normal/emissive textures by UUID, UV tiling/offset, nearest/linear filtering, alpha cutoff, unlit, double sided) assigned to mesh renderers, hot reloaded, validated by export/`check`, edited in the Material Editor; 3D template floor uses a tiled material. Done (Slice 35): per-entity `MaterialOverrides` (UV offset, UV scroll, emissive) from the inspector and scripts. Done (Slice 36): project surface shaders (`.vshader`, GLSL compiled at load time with glslang/SPIRV-Cross, four per-material params, time, hot reload, built-in fallback, check/export validation, Material Editor slot). Done (Slice 37): material instances (`parent` + per-field overrides, chains, cycle checks, editor inherit/override markers). Remaining: clustered lighting, anisotropic filtering. Done (Slice 35.1): vertex colours, distance fog, coverage (snow) layer with vertex-alpha masks, glTF COLOR_0. Done (Slice 41.1): alpha/additive materials in a sorted transparent sub-pass, blended surface shaders, vertex colour in surface shaders.
- [~] Direct/indirect lighting, shadows, reflection probes, environment lighting and scalable global illumination. Done (Slice 9): cascaded shadow maps, SSAO, specular occlusion, analytic sky/environment lighting. Done (Slice 44): image-based lighting from `.hdr` environment maps (GPU GGX prefilter, SH9 irradiance, environment sky). Done (Slice 45): physical single-scattering sky driven by the sun, baked into IBL. Done (Slice 46): local box-projected reflection probes captured from the scene. Done (Slice 48): clustered forward point and spot lights. Done (Slice 56): spot and point light shadows in an atlas. Remaining: GI.
- [~] Post-processing, temporal techniques, anti-aliasing, depth of field, motion blur and tone mapping. Done (Slice 9): dual-filter bloom, GPU auto exposure, ACES/PBR Neutral/Reinhard tonemapping with grading/vignette, FXAA 3.11. Done (Slice 47): TAA (Halton jitter, depth reprojection, Catmull-Rom history, YCoCg variance clipping). Remaining: per-object motion vectors, temporal upscaling, DOF, motion blur.
- [~] GPU culling/indirect draws where beneficial; instancing, occlusion, mesh LOD and streaming. Done (Slice 10): automatic GPU instancing in all mesh passes (~12× on 4 096 cubes, llvmpipe). Done (Slice 54): discrete mesh LODs with screen-space error selection. Done (Slice 55): GPU Hi-Z occlusion culling with indirect draws for the main pass. Remaining: GPU frustum/LOD selection, two-phase culling for the pre-pass and shadows, streaming.
- [ ] Terrain, foliage, water, atmosphere, weather, sky/clouds and large-world precision.
- [ ] Decals, dynamic environment effects and destruction interfaces.
- [~] Scalable high/standard/mobile/compatibility tiers, dynamic resolution and user overrides. Done (Slice 9): Low/Medium/High/Ultra presets, render scale, per-project setting and `--quality` override. Done (Slice 47): frame-time driven dynamic resolution (`rendering.target_fps`). Remaining: device-based defaults, mobile GPU validation.
- [ ] Research-only advanced paths: ray tracing, virtualized geometry/textures and other high-cost features; implement only after evidence.
**Exit:** representative outdoor and indoor scenes with measured quality tiers on desktop and mobile.

## Phase 9 — Gameplay framework and scripting
- [ ] Game instance/session, world/level lifecycle, game mode/rules, game state and player state.
- [~] Actors/entities, components, player controllers, pawns/characters, cameras and spawn points. Done: entities/components (Slice 3), scripted player control (Slice 5), 3D physics characters (Slice 11). Remaining: reusable controller/camera rigs and spawn points.
- [~] Input actions/mappings for keyboard, mouse, controllers and touch. Done (Slice 18): named actions in `victus.project` with defaults, key/mouse/gamepad button and stick/trigger half-axis bindings, dead zones, Luau action/value/vector/rebind API, SDL3 gamepads (up to 4), validation in `victus check`, `--hold Pad.*`, gamepad input over the network, templates playable with a pad. Done (Slice 19): editor Edit > Input Actions panel (press-to-bind capture for keys, mouse, pad buttons and sticks; typed bindings; rename, dead zone, add/delete, reset; saved to `victus.project`, applied live while playing). Remaining: touch, per-player device assignment for local multiplayer, an in-game rebinding UI with persisted per-player overrides, rumble.
- [ ] Interaction, gameplay events, timers, tags, attributes, inventory/ability primitives as modular systems.
- [~] Prefabs/packed scenes, reusable components, variants and safe instantiation. Done (Slice 21): versioned `.vprefab` assets (asset kind `prefab`), instances with deterministic derived uuids, component-level overrides that survive prefab updates, Apply/Revert/Unpack and Create Prefab in the editor, drag/double-click instantiation, prefab file hot reload updating instances, Luau `scene.instantiate`, validation in `victus check`/`victus export`, 3D template crates as prefab instances. Remaining: nested prefabs (currently flattened when a prefab contains instances), variants, per-property overrides, network replication of runtime-spawned instances, an isolated prefab editing mode.
- [x] Scripting runtime: Luau 0.747 sandboxed VM, `ScriptComponent` + `.luau` assets, entity/scene/input/time/log API, lifecycle (`start`/`update`/`fixed_update`/`reload`), editor-visible properties with per-entity overrides, hot reload in editor and `victus run`, time/memory budgets, `victus check` and headless `victus simulate` (Slice 5). Rendering API (Slice 34): `set_material/material`, `set_mesh/mesh`, `set_color/color`, `set_sprite/sprite`, `set_cast_shadows`, mesh `set_visible`, paths resolved through the asset database.
- [ ] Script debugging (breakpoints/stepping), type checking in `victus check`, API versioning, events/messages between scripts.
- [ ] Visual scripting graph with typed nodes, validation, debugging and deterministic serialization.
- [~] Save/load, checkpoints, profile data, version migration and recovery. Done (Slice 22): `SaveStore` slots (`.vsave`, checksummed, atomic writes, `.bak` recovery, listing), data-version migrations (C++ and Luau `save.migrate`), project `save_version`, Luau `save.*` API for profile/progress data, whole-scene checkpoints with per-script `save_state`/`load_state` and entity references by id, per-user saves for exported games and `.victus/saves` for development, quick save/load (F5/F9, LB/RB) in the 2D and 3D templates. Remaining: physics velocities and animation/particle/audio playback state in checkpoints (bodies restart at rest), multiplayer checkpoints, cloud/platform save backends, an editor save browser, compression/encryption of save files.
**Exit:** complete playable 3D game loop authored in VICTUS with both code and visual-logic options.

## Phase 10 — Physics, animation, AI, audio and UI
- [~] 3D/2D physics, rigid bodies, character controllers, collision queries, joints and debug tools. Done (Slice 6): Jolt 5.7.0 + Box2D 3.2.0 behind `victus::physics`, rigid bodies, box/sphere/capsule/cylinder colliders, triggers, layers/masks, raycasts/overlaps, contact events to scripts, collider debug lines, reflected inspector. Done (Slice 11): 3D character controller (Jolt `CharacterVirtual`: slopes, steps, ground detection, jumping, pushing, moving platforms) and 3D joints (fixed/point/hinge/slider/distance with limits, motors, breaking). Done (Slice 14): 2D character controller on the Box2D geometric mover (ground detection, max slope, snapping, pushing, one-way platforms with drop-through) and 2D joints (weld/revolute/prismatic/distance/wheel with limits, springs, motors, breaking). Remaining: mesh/convex colliders, 2D polygons/chains, ragdolls.
- [~] Skeletal animation, blend spaces, state machines, retargeting, root motion and animation tooling. Done (Slice 12): glTF skins and clips imported/cooked, GPU skinning in all mesh passes, `Animator` playback with loops and cross-fades, parallel evaluation, Luau control. Remaining: blend spaces, state machines, layers, root motion, IK, retargeting, morph targets, animation editor tooling.
- [~] Navigation meshes, pathfinding, avoidance, crowds and behavior/state systems. Done (Slice 16): `engine/navigation` (Recast & Detour 1.6.0) automatic navmesh from static colliders with change-driven rebuilds, path/nearest/raycast/random queries, DetourCrowd agents with avoidance (CharacterController agents via the physics mover), Luau API, editor navmesh view, template companion. Remaining: tiled/background rebuilds, off-mesh links, area costs, 2D navigation, behavior trees/state machines.
- [ ] Gameplay AI: perception, decision systems, behavior trees/utility/state machines and debugging.
- [~] Audio playback, buses/mixing, spatialization, effects, streaming and profiling. Done (Slice 7): miniaudio 0.11.26 behind `victus::audio`, `AudioSource`/`AudioListener`, WAV/Ogg/MP3/FLAC, six buses with volume/mute, 3D attenuation following transforms, streaming music, one-shots, Luau API, editor inspector + preview, null-device mixing tests. Remaining: effects/reverb, HRTF, ducking, profiling view.
- [~] Runtime UI, layout, input/accessibility, localization and visual UI editor. Done (Slice 15): `engine/gameui` canvases, anchored rects, panels, images, text (Inter), buttons, layouts, pointer/keyboard interaction, Luau API, rendering in 2D/3D. Remaining: visual UI editor, localization, accessibility, rich text, more widgets.
- [~] Particle/VFX system, GPU/CPU emitters, trails, curves, collision and VFX authoring. Done (Slice 13): deterministic CPU emitters (`ParticleEmitter`: rate/bursts/cycles, shapes, gravity/drag, size/colour over life with HDR glow, spin, alpha/additive), parallel simulation, soft depth-faded instanced billboards in the HDR pass, 2D sprite path, Luau control, template fire/sparks/smoke/dust/coin sparkles. Remaining: GPU simulation, trails/ribbons, flipbooks, curve editing, collision, sub-emitters, lit particles, VFX authoring tools.
**Exit:** reference game exercises the systems together without one-off engine bypasses.

## Phase 11 — Cinematics, procedural and world authoring
- [ ] Timeline/sequencer, camera rigs, animation tracks, events, audio and render/export.
- [ ] Material/shader graph, animation graph and procedural graph.
- [ ] Terrain/biome/vegetation, spline/road, scatter and settlement tools.
- [ ] Deterministic procedural seeds, validation and reproducible generation.
- [ ] Optional AI-assisted authoring with human review, provenance and reproducibility.
**Exit:** create, edit, save, cook and play a substantial procedural world and cinematic sequence.

## Phase 12 — Multiplayer and online runtime
- [~] Transport abstraction, authoritative server model and session lifecycle. Done (Slice 17): `net::Transport` with ENet 1.3.18 UDP and in-memory backends, versioned handshake with game fingerprint, join/leave, timeouts, graceful disconnect. Listen server (Slice 27): `victus run --host` / `victus_player --host` host a session and play in it (server-owned host character, id 0 in rosters). Reconnect (Slice 28): dropped clients retry for 20 s and reclaim their held character with a server-issued token. Remaining: host migration.
- [~] Entity/property/event replication, relevancy, ownership and bandwidth budgets. Done (Slice 17): `NetworkIdentity` snapshots at 30 Hz (transform, velocity, script values), per-player spawn from a template, ownership, reliable script messages, relevancy radius, per-snapshot byte budget with priority (deferred entities stay fresh). Delta compression (Slice 23): quantized snapshots encoded against the newest acknowledged baseline (measured 977 → 486 bytes per snapshot in the net test scene). Remaining: smallest-three/bit-packed rotations, interest management beyond radius, RPC on arbitrary components.
- [~] Client prediction, reconciliation, interpolation and lag-tolerant movement. Done (Slice 17): owned-player prediction with server correction, 100 ms snapshot interpolation with bounded extrapolation. Remaining: input-replay reconciliation with physics rollback, lag compensation for hits.
- [~] Dedicated/headless server builds, server config, logs and graceful shutdown. Done (Slice 17): `victus serve` (port/bind/max clients/frames, SIGINT/SIGTERM shutdown, logs). Remaining: server config files, exported dedicated-server packages.
- [~] Network emulation for latency, jitter, packet loss and bandwidth constraints. Done (Slice 17): latency/jitter/loss emulation on any transport (`--conditions`). Remaining: bandwidth caps, duplication/reordering profiles.
- [~] Multiplayer debugging, replay/recording, reconnect and persistence. Done: reconnect (Slice 28); editor Play as Host with Network window and replication inspector (Slice 30); in-editor multi-client play mode (Slice 31). Remaining: persistence, networked captures.
- [ ] Replaceable interfaces for matchmaking, lobbies, identity and platform services; no mandatory vendor lock-in.
- [~] Security model, input validation, server authority and abuse-resistant boundaries. Done (Slice 17): server authority, clients may only drive their own player, bounded message sizes/rates, malformed packets rejected. Encrypted, authenticated sessions (Slice 26): Noise NX-style X25519 handshake, XChaCha20-Poly1305 packets with replay protection, pinned server keys and server passwords (Monocypher); `victus serve` encrypts by default. Remaining: player identities/accounts, key rotation, NAT traversal/relays.
**Exit:** two or more clients play a representative networked game against an authoritative dedicated server under repeatable network tests.

## Phase 13 — Production, collaboration and ecosystem
- [~] Build automation, asset cooking, incremental compilation, caching and reproducible packaging. Done (Slice 8): `victus export` (validated, byte-reproducible `.vpak` archive with checksummed entries, cooked-only assets, embedded third-party notices, manifest) and the player's packaged mode. GPU texture compression variants (Slice 49). Remaining: archive compression, per-platform variant stripping, cross-platform targets, signing, build automation.
- [~] Hot reload for code/assets/shaders where safe; clear fallback when reload is unsupported. Done (Slice 20): `victus run` and the editor (edit and play mode) watch `assets/` and reload textures and models in place (incl. `.meta` settings and glTF dependencies), audio clips (playing sources restart with the new data) and scripts (already, since Slice 3); a broken save keeps the previous import in use and is reported; scenes and fonts are reported as needing a restart. Remaining: font and scene hot reload, re-instantiating models whose node graph changed, built-in shader hot reload (project `.vshader` surface shaders hot reload since Slice 36), C++ code reload.
- [~] Automated unit/integration/gameplay tests, smoke tests and deterministic regression captures. Done: doctest suites, template smoke tests and CI (earlier slices); Slice 24 `.vcapture` regression captures — `victus run/simulate --record` stores input, frame times, the script random seed and state digests; `--replay` re-runs headless or in the player and fails at the first diverging frame. Slice 25: entity-level divergence reports (captures store per-entity state behind each digest) and editor Record / Replay Capture in play mode. Slice 29: digests cover script fields and physics velocities through state probes (capture format 2; format-1 captures still verify). Remaining: probes for joints, animation and UI, cross-platform determinism, capture-based CI suites per project.
- [ ] Crash reporting, symbol handling, diagnostics export and privacy controls.
- [ ] Source-control integration, project diffs/merges, collaboration foundations and conflict handling.
- [ ] Plugin/module SDK, versioned public APIs, extension lifecycle and migration tooling.
- [ ] Modding sandbox, permissions, package signing/validation and safe content boundaries.
- [ ] Localization, accessibility, tutorials, sample templates and API documentation.
- [ ] Release automation, changelog, SBOM, security response and dependency update process.
**Exit:** an external developer can install VICTUS, collaborate on a project, build, test, package and diagnose a shipped game.

## Phase 14 — Mobile, XR and additional platform validation
- [ ] Windows/Linux desktop runtime and editor; macOS/Metal runtime/editor.
- [~] Android arm64 runtime, touch, thermal/memory profiling, lifecycle and physical-device matrix. (Slice 50: APK build in CI, script touch API, suspend/resume, mobile quality defaults; no device run, profiling or device matrix yet.)
- [ ] iOS runtime/build/signing feasibility and policy validation.
- [ ] Mobile asset variants, low-memory eviction, context recreation, suspend/resume and battery-aware pacing.
- [ ] OpenXR integration research and XR input/rendering only after desktop/mobile core is stable.
- [ ] Web/WebAssembly feasibility study; only pursue if required APIs, threading, memory and distribution constraints are acceptable.
- [ ] Console support only with platform-holder access, SDK agreements and compliance; never imply support without it.
**Exit:** documented, reproducible platform matrix with actual build, launch, input, rendering and package tests.

## Phase 15 — Reference projects and production readiness
- [ ] Complete 2D game demonstrating the 2D workflow.
- [ ] Complete 3D gameplay game demonstrating rendering, gameplay, AI, audio, UI, saves and packaging.
- [ ] Complete simulation project demonstrating large populations, simulation LOD, persistence and headless mode.
- [ ] Optional multiplayer sample demonstrating dedicated server and network diagnostics.
- [ ] Build all references from clean checkouts in CI; run gameplay smoke/regression tests.
- [ ] Profile on named desktop and mobile hardware; publish reproducible reports and known limitations.
- [ ] External usability testing, tutorials, migration docs and stable release criteria.
**Exit:** independent developers can build, run, test and package the reference projects using documented workflows. Production readiness is a measured release decision, not a roadmap checkbox.

## Definition of done
A task is complete only when code is integrated on main, relevant tests pass, build commands work, errors/fallbacks are handled, platform and license limits are documented, performance claims have evidence, the diff is audited, and SHA plus verification are recorded in `docs/STATUS.md`. A demo or placeholder is not completion.

## Execution order
Follow dependency order and complete each subsystem end-to-end. If a phase is blocked by unavailable hardware, credentials or platform-holder access, document the precise blocker and continue independent work. After every verified commit, update status and immediately proceed to the next unblocked task.
