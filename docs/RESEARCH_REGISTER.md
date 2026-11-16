# Research Register

Prefer primary documentation, upstream repositories, release notes, issues, benchmarks and exact license texts. Record URL, access date, version/commit, findings and uncertainty. This register is a starting evidence map, not proof that any dependency has been selected or licensed.

## Engine and product references
| Project/source | Study | VICTUS lesson | Caution |
|---|---|---|---|
| Unreal Engine Gameplay Framework — https://dev.epicgames.com/documentation/en-us/unreal-engine/gameplay-framework-in-unreal-engine | Game instance, modes/states, controllers, pawns, components, cameras and modular gameplay | A reusable gameplay layer is essential between low-level runtime and game code | Learn public concepts; no copying proprietary implementation |
| Unreal Engine Networking Overview — https://dev.epicgames.com/documentation/unreal-engine/networking-overview-for-unreal-engine | Server authority and networked gameplay | Authority, replication and prediction need explicit architecture | Public docs only |
| Unity — https://docs.unity3d.com/Manual/index.html | Editor workflows, assets, build profiles, profiling and platform delivery | Production pipeline and usability are core engine capabilities | Proprietary implementation; public docs only |
| Godot 2D docs — https://docs.godotengine.org/en/stable/tutorials/2d/index.html | Dedicated 2D renderer, physics, tilemaps, particles and animation | 2D must be a complete first-class workflow | Evaluate trade-offs and scaling limits |
| Godot feature list — https://docs.godotengine.org/en/stable/about/list_of_features.html | Supported workflows/platforms and feature completeness | Use a broad feature checklist when auditing product coverage | Not a performance guarantee |
| O3DE — https://www.o3de.org/ | Modular engine, Gems, Atom renderer and asset processing | Modularity and production asset workflows | Evaluate build/maintenance footprint |
| Bevy — https://bevyengine.org/ | ECS, schedules and data-oriented runtime | Benchmark data-oriented approaches | Rust ecosystem and editor maturity differ |
| Filament — https://github.com/google/filament | PBR and mobile-oriented renderer | Renderer quality and mobile performance reference | Not a complete engine/editor |
| bgfx — https://github.com/bkaradzic/bgfx | Graphics abstraction and shader tooling | RHI candidate | Verify backend/platform matrix and exact license family |
| Diligent Engine — https://github.com/DiligentGraphics/DiligentEngine | Explicit resource model and graphics API abstraction | RHI candidate | Verify exact support, maintenance and bundled dependencies |
| Blender — https://www.blender.org/ | Authoring, geometry nodes and asset workflows | Integrate via open formats and tooling | Keep Blender external; review GPL obligations |

## Standards and interoperability
| Standard | Source | Use | Important boundary |
|---|---|---|---|
| glTF 2.0 | https://registry.khronos.org/glTF/ | Runtime 3D asset delivery, scenes, meshes, materials, cameras, skins and animation | Not a full authoring format or complete streaming system |
| OpenXR | https://www.khronos.org/openxr/ | Cross-vendor XR runtime interface | Requires runtime/device validation; optional target |
| Vulkan | https://www.khronos.org/vulkan/ | Explicit graphics/compute backend candidate | Complexity and driver/device coverage must be measured |
| OpenUSD | https://openusd.org/ | Complex scene interchange and collaboration workflows | Distinguish authoring/interchange from runtime cooked assets |

## Component candidates (verify exact release)
| Component | Role | Initial license expectation |
|---|---|---|
| SDL3 | Platform/window/input | zlib |
| Flecs / EnTT | ECS | MIT |
| Taskflow | Task graph | MIT |
| enkiTS | Scheduler | zlib |
| Jolt | Physics | MIT |
| Recast/Detour | Navigation | zlib |
| ozz-animation | Animation | MIT |
| bgfx/bx/bimg | Graphics abstraction | BSD family; verify each |
| Diligent | Graphics abstraction | Apache-2.0 |
| Filament | Renderer/reference | Apache-2.0 |
| meshoptimizer | Geometry | MIT |
| fastgltf/cgltf | glTF loading | Verify exact release |
| KTX/Basis | Texture/transcoding | Verify exact release/codecs |
| miniaudio | Audio | MIT-0/public-domain option; verify |
| Dear ImGui/ImGuizmo | Editor/gizmos | MIT |
| Tracy | Profiling | BSD-3-Clause |
| Catch2/doctest | Tests | BSL-1.0/MIT |
| Blender | External authoring | GPL-3.0-or-later; keep external |
| OpenUSD | Scene interchange | Apache-2.0; inspect dependencies |

Shortlist only, not legal determination or adopted dependencies. Check upstream LICENSE, bundled notices, transitive dependencies, patent terms and platform support before adoption.

## Research conclusions added 2026-11-16
- Unreal's public Gameplay Framework documentation demonstrates the value of a composable layer for game/session lifetime, rules/state, controllers, pawns, cameras and components. VICTUS therefore specifies its own flexible gameplay framework rather than exposing only ECS primitives.
- Godot's official 2D documentation explicitly describes a dedicated 2D renderer and physics engine plus tilemaps, particles and animation. VICTUS therefore treats 2D as a complete product track, not merely sprites drawn by the 3D renderer.
- Unreal's networking overview describes server authority as the source of truth. VICTUS adopts server-authoritative networking as the default shared-state model, with client prediction/reconciliation and impairment testing as explicit requirements.
- Khronos describes glTF 2.0 as an API-neutral runtime delivery format with scene, mesh, material, camera, skin and animation data. It is a strong import/cook interchange baseline, but does not replace VICTUS authoring or world-streaming systems.
- Khronos describes OpenXR as a royalty-free common API for AR/VR devices. XR remains an optional, evidence-gated extension after the core renderer and input model are stable.

## Research conclusions added 2026-11-16 (Slice 5)
- Scripting: Luau 0.747 (luau-lang/luau, MIT) was chosen over Lua 5.4, LuaJIT, Wren, AngelScript, C# and WASM (ADR-0008). Deciding factors: a supported sandbox (`luaL_sandbox`, read-only globals), VM interrupts for time budgets, gradual typing, a native `vector` value type, interpreter-only portability to iOS/consoles, and active releases. LuaJIT was rejected because its JIT is not permitted on iOS/consoles and its release cadence is irregular.
- Physics: Jolt Physics v5.7.0 (jrouwe/JoltPhysics, MIT) and Box2D v3.2.0 (erincatto/box2d, MIT) were verified as the latest releases on 2026-11-16 from their GitHub release pages and chosen over PhysX 5, Bullet 3 and Chipmunk2D (ADR-0009). Deciding factors: permissive licenses, active maintenance, multithreaded solvers with external task-system hooks, sensor/contact event APIs, small static builds suitable for mobile, and production use (Jolt: Horizon Forbidden West, Godot 4.4+). Open questions: cross-platform determinism cost, character controller choice (`CharacterVirtual` vs. kinematic body), mesh collider cooking in the asset pipeline.
- Physics (next slice): Box2D v3.2.0 (2026-10-19, MIT; adds double-precision large-world positions) for 2D and Jolt v5.7.0 (2026-11-14, MIT; always-deterministic simulation, samples on iOS/Android) for 3D. Box3D (announced 2026-06) noted as a younger alternative; Jolt is preferred for maturity and shipped-title track record.

## Research conclusions added 2026-11-16 (Slice 7)
- Audio: miniaudio 0.11.26 (mackron/miniaudio, public domain or MIT-0) was verified as the latest release (published 2026-10-31, GitHub releases API) and chosen over SDL3 audio, OpenAL Soft, SoLoud and proprietary FMOD/Wwise (ADR-0010). Deciding factors: single-file build with no runtime dependencies, backends for every target platform including AAudio/OpenSL ES and Web Audio, built-in decoders and spatializer, a no-device mode for deterministic headless tests, and a license that needs no attribution. Open questions: DSP effects/reverb and HRTF (miniaudio has no HRTF; Steam Audio is a candidate), Opus decoding, streaming from packed archives.

## Research conclusions added 2026-11-16 (Slice 8)
- Packaging: an owned `.vpak` format was chosen over zip (miniz/libzip) and PhysicsFS (ADR-0011) to keep the parsing surface small, keep payloads aligned for future memory mapping, verify per-entry checksums and produce byte-reproducible exports; no new dependency was added. Open questions: compression codec (LZ4 for speed vs. zstd for ratio), GPU texture compression (ASTC for mobile, BC7 for desktop, e.g. via Basis Universal/KTX2), signing, and platform bundles (APK/AAB, iOS/macOS app bundles).

## Open research questions
Graphics: actual backend/device support; Android Vulkan/fallback; Apple Metal; shader compile/reflection/hot reload; abstraction overhead; multithreaded recording/synchronization; 2D/3D shared resource model.
Gameplay: data-oriented ECS versus higher-level object facade; reflection/serialization; visual graph runtime; prefab references/variants; deterministic gameplay testing.
Simulation: ECS layout on VICTUS workloads; scheduler affinity/cancellation; LOD invariants/causality; determinism across CPU architectures.
Editor/assets: undo/redo, serialization, recovery, node graph UX, import formats, OpenUSD necessity, cook/cache/versioning, project merges and collaboration.
Networking: transport candidates, server tick model, prediction scope, replication bandwidth, relevancy and security threat model.
Mobile/XR: memory budgets, thermals, texture compression, suspend/resume, minimum device matrix, OpenXR runtime coverage and physical-device CI limits.
Production: code/asset hot reload safety, incremental build cache correctness, mod/plugin isolation, package signing, SBOM and crash privacy.

Search snippets and secondary mirrors are discovery aids, not license proof or normative API references. Confirm claims against primary documentation and exact upstream versions before implementation.

## Research conclusions added 2026-11-16 (Slice 9, rendering quality)
Techniques are implemented from the published descriptions below; no third-party code was copied or added.
- Bloom: Jorge Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014 Advances in Real-Time Rendering) — https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/ . 13-tap downsample with Karis-average on the first mip to suppress fireflies, 3×3 tent upsample accumulated additively up the chain. VICTUS adds a soft-knee threshold so artists can keep bloom energy-preserving (threshold 0) or stylised.
- Ambient occlusion: Morgan McGuire et al., "The Alchemy Screen-Space Ambient Obscurance Algorithm" (HPG 2011) and "Scalable Ambient Obscurance" (HPG 2012) — https://research.nvidia.com/publication/2012-06_scalable-ambient-obscurance . Spiral sampling from a depth buffer with reconstructed view-space positions, followed by a depth-aware blur; VICTUS runs it at half resolution on the High tier and with distance fade so far geometry does not darken.
- Specular occlusion: Sébastien Lagarde and Charles de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0" (SIGGRAPH 2014 course notes) — https://seblagarde.wordpress.com/2015/07/14/siggraph-2014-moving-frostbite-to-physically-based-rendering/ . Used to attenuate environment specular by AO and roughness.
- Environment BRDF approximation: Brian Karis, "Physically Based Shading on Mobile" (Unreal Engine blog, 2014) — https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile . Analytic fit chosen over a BRDF LUT to save a texture fetch on mobile.
- Cascaded shadows: Fan Zhang et al., "Parallel-Split Shadow Maps" (GPU Gems 3, ch. 10) — https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus . Practical split scheme (logarithmic/uniform blend, lambda 0.8), texel-snapped bounding spheres to stop shimmering, cascades packed into one 2×2 atlas.
- Anti-aliasing: Timothy Lottes, "FXAA" (NVIDIA white paper, 2009; FXAA 3.11 quality preset) — https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf . Luma stored in alpha by the tonemap pass.
- Tonemapping: Khronos PBR Neutral tone mapper — https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral (spec published under Apache-2.0; VICTUS implements the formula), Stephen Hill's ACES fitted curve (BakingLab, MIT, formula only), and extended Reinhard. Auto exposure follows the log-average luminance method with exponential adaptation (Reinhard et al. 2002, "Photographic Tone Reproduction for Digital Images").
- Open questions: TAA/TSR (needs motion vectors and jitter — next rendering slice), clustered lighting beyond 16 point lights, reflection probes, and measured timings on real mobile GPUs (only llvmpipe CPU numbers exist today).

## Research conclusions added 2026-11-16 (Slice 11, characters and joints)
No third-party code was added; Jolt 5.7.0 was already a dependency.
- Jolt `CharacterVirtual` class reference — https://jrouwe.github.io/JoltPhysics/class_character_virtual.html . It has no rigid body and moves by collision checks only, it is updated by the application, `ExtendedUpdate` combines `Update`, `StickToFloor` and `WalkStairs`, and `mInnerBodyShape` makes the character visible to the simulation. VICTUS uses all of these; velocity handling follows Jolt's `CharacterVirtualTest` sample in the 5.7.0 source.
- Jolt architecture documentation, constraints section — https://jrouwe.github.io/JoltPhysics/index.html . Constraint types (fixed, point, distance, hinge, slider), motors with force/torque limits, breakable constraints by checking total lambda after each step and calling `SetEnabled(false)`, the fact that bodies do not track their constraints (VICTUS removes constraints before bodies), and `GroupFilter` for skipping collisions between connected bodies.
- Open questions: ragdolls (swing-twist + `GroupFilterTable`), Box2D 3.2 joints and a 2D character mover, and parallel character updates.

## Research conclusions added 2026-11-16 (Slice 12, skeletal animation)
No third-party code was added; cgltf 1.15 was already a dependency.
- glTF 2.0 specification, sections 3.7.3 Skins and 3.11 Animations — https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html . Joint order matches `inverseBindMatrices` (identity when absent); `JOINTS_n`/`WEIGHTS_n` sets give up to four influences each and implementations may support only one set; the skinned vertex matrix is the weighted linear sum of joint matrices and the transform of the node holding the skinned mesh must be ignored; animation outputs clamp outside the input range; LINEAR rotations use shortest-path slerp and CUBICSPLINE rotations must be normalized; glTF does not define playback order or looping, which VICTUS adds in the `Animator`. VICTUS follows all of these, keeps the top four influences across sets 0–1, and skips `weights` (morph) channels.
- Open questions: blend trees/state machines, root motion extraction, dual-quaternion skinning, and a compact pose buffer for large crowds.

## Research conclusions added 2026-11-16 (Slice 14, 2D characters and joints)
No third-party code was added; Box2D 3.2.0 was already a dependency.
- Box2D v3.2.0 `samples/geometric_mover.cpp` and `include/box2d/collision.h`/`world.h` — https://github.com/erincatto/box2d/tree/v3.2.0/samples . The geometric mover collects collision planes with `b2World_CollideMover`, solves a position correction with `b2SolvePlanes`, sweeps the remaining delta (`b2World_CastMover`) and clips velocity with `b2ClipVector`. VICTUS follows that loop but sweeps with `b2World_CastShape` so it can skip sensors, itself and rejected one-way platforms per hit, and takes plane contact points from `b2Shape_GetClosestPoint`, because `b2PlaneResult.point` lies on the mover's core segment rather than the surface, which is not usable for "is the character above this platform".
- Box2D v3.2.0 `samples/dynamic_mover.cpp`, `samples/sample_character.cpp` (one-way elevator) and `b2PreSolveFcn` in `types.h`/`world.h` — same tree. One-way collision for rigid bodies uses pre-solve (enable the contact only when the normal points up and the body is not already deep inside); it requires `enablePreSolveEvents` on the shape and is called from worker threads, so the callback must be thread-safe (VICTUS keeps flags in shape user data only).
- Open questions: 2D step-up for small ledges, 2D ragdolls/soft bodies, and polygon/chain colliders for terrain.

## Research conclusions added 2026-11-16 (Slice 13, particles)
No third-party code was added.
- Tristan Lorach, "Soft Particles" (NVIDIA white paper, 2007) — https://developer.download.nvidia.com/whitepapers/2007/SDK10/SoftParticles_hi.pdf . Compare each particle fragment's depth with the scene depth read as a texture, fade with the scaled, saturated difference, and discard fragments behind the scene. VICTUS reconstructs linear view depth from the depth buffer (`proj[3].z / (d + proj[2].z)` for its 0..1 right-handed projection) and fades over the emitter's `softness` in metres.
- Open questions: GPU simulation with compute and indirect draws, depth-buffer collisions, trails/ribbons, flipbook animation with motion vectors, and lit particles.

## Research conclusions added 2026-11-16 (Slice 16, navigation)
- Recast & Detour 1.6.0 — https://github.com/recastnavigation/recastnavigation/releases/tag/v1.6.0 ; build pipeline from `RecastDemo/Source/Sample_SoloMesh.cpp` (heightfield → walkable filters → compact heightfield → erosion → distance field/watershed regions → contours → poly mesh → detail mesh → `dtCreateNavMeshData`). Voxel parameters are converted as `walkableHeight = ceil(h/ch)`, `walkableClimb = floor(climb/ch)`, `walkableRadius = ceil(r/cs)`; polygon vertex indices are 16-bit, so one tile holds < 65 535 vertices.
- DetourCrowd (`DetourCrowd/Include/DetourCrowd.h`, CrowdTool sample) — agent parameters `collisionQueryRange = 12 r`, `pathOptimizationRange = 30 r`, separation weight 2 and the four obstacle-avoidance quality presets used by the sample; agents are moved by `dtCrowd::update`, and external positions are applied with `dtPathCorridor::movePosition`.
- Triangles are walkable when `cross(v1 - v0, v2 - v0)` points up (`rcMarkWalkableTriangles`), so collider triangles are wound outward from the convex shape's center.
- Open questions: tiled/background rebuilds for open worlds (DetourTileCache), off-mesh links for jumps and ladders, area costs, 2D (top-down/platformer) navigation.

## Research conclusions added 2026-11-16 (Slice 15, in-game UI)
- Unity uGUI `CanvasScaler.cs` (MatchWidthOrHeight) — https://github.com/Unity-Technologies/uGUI/blob/main/com.unity.ugui/Runtime/UGUI/UI/Core/Layout/CanvasScaler.cs . The scale factor is `2^lerp(log2(w/ref_w), log2(h/ref_h), match)`, so opposite deviations on the two axes cancel at 0.5. VICTUS uses the same blend.
- Inter 4 (Rasmus Andersson, SIL OFL 1.1) — https://github.com/rsms/inter ; latin 500/700 TTFs from Fontsource 5.2.8 embedded (THIRD_PARTY_AND_LICENSES).
- Open questions: text shaping and bidi for localization, SDF text for world-space UI, and a visual UI editor.

## Research conclusions added 2026-11-17 (Slice 30, editor multiplayer debugging)
- Unreal Engine, "Play In Editor Multiplayer Options" — https://dev.epicgames.com/documentation/en-us/unreal-engine/play-in-editor-multiplayer-options-in-unreal-engine (accessed 2026-11-17): Play-In-Editor net modes Standalone, Play As Listen Server and Play As Client, with a configurable number of players. VICTUS starts with Offline and Host (listen server); multiple in-editor clients are the next slice.
- Unity Netcode for GameObjects tools, Network Profiler — https://docs-multiplayer.unity3d.com/tools/current/profiler/ (accessed 2026-11-17): a Messages module with total/named/scene-event bytes sent and received per frame, and an Objects module grouped by network object (RPC bytes, network-variable deltas, spawns, destroys, ownership changes); host-to-self traffic is greyed out. Informed the per-entity updates/bytes columns and the bandwidth graphs.
- Open questions: per-entity bandwidth over time, relevance visualisation in the viewport, profiling cost in sessions with thousands of entities.

## Research conclusions added 2026-11-17 (Slice 29, state digests)
- Factorio Friday Facts #55 — https://www.factorio.com/blog/post/fff-55: multiplayer runs a "heuristic CRC" every tick, "incomplete but very fast", over a few crucial values (players, active entity count, production statistics); full-state CRCs are a slower debugging mode. Factorio Friday Facts #47 — https://factorio.com/blog/post/fff-47: replays save state with human-readable tags at the first diverging tick, where "the difference is usually very small - typically just a value of one variable". VICTUS follows both: probes choose the state that matters and every line stays readable, so the first divergent digest names the variable.
- Photon Quantum, "Debugging Desyncs" — https://doc.photonengine.com/quantum/v1/community-wiki/knowledge-base/debugging_desyncs: checksums recorded after individual systems show which step desynced (e.g. after `PhysicsSystemPre`), and frame dumps are diffed between clients. VICTUS tags lines by subsystem (`script.*`, `physics.*`) rather than hashing per system, which gives the step and the value in one report.
- Open questions: probing nested script tables and entity references, cost of probes on very large scenes, and sharing the probe format with networked desync checks.

## Research conclusions added 2026-11-17 (Slice 28, reconnect)
- Unreal Engine `AGameMode` API — https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/GameFramework/AGameMode: disconnected players' PlayerStates go to `InactivePlayerArray` "in case they reconnect", kept for `InactivePlayerStateLifeSpan` and capped by `MaxInactivePlayers`; `FindInactivePlayer`/`OverridePlayerState` restore them on login. VICTUS adopts the same shape (held characters with a grace period, counted against capacity, restored on rejoin).
- WizardCell, "Unreal Engine Persistent Data Compendium" — https://wizardcell.com/unreal/persistent-data/: the inactive copy lives 300 s by default and is matched by `UniqueId` or the saved network address; a new controller and pawn are created on reconnect. VICTUS has no player identities and addresses change when a device switches networks, so it matches a random 64-bit token issued in the (encrypted) welcome, and keeps the original character entity instead of recreating it.
- Open questions: persisting held state across server restarts, rejoining after a host migration, tokens bound to player accounts once identities exist.

## Research conclusions added 2026-11-17 (Slice 27, listen servers)
- Valve Developer Community, Source Multiplayer Networking — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking: the server is authoritative over simulation, rules and input; entity interpolation adds 100 ms of view delay even on a listen server, and low latency is an advantage that prediction and lag compensation only reduce. VICTUS keeps one authoritative world for the host and gives the host no prediction or interpolation (it reads the authoritative state directly).
- Edgegap glossary, "Listen Server" — https://edgegap.com/glossary/listen-server: a listen-server game has three roles (server, remote client, host) and code written for two often misses the third; the host's inputs never cross the network, so playtesting as host hides remote-player problems; the match ends when the host leaves unless host migration exists; the host's frame rate sets the pace for everyone. VICTUS therefore models the host as a server-owned character (same script code path as offline: `net.authority()` and `is_local()` are true), lists it as id 0 so scoreboards include it, and keeps `victus serve` for fair or long-running sessions.
- Open questions: host migration, NAT traversal or relays for internet play, ticking the server at a fixed rate independent of the host's render frame rate.

## Research conclusions added 2026-11-17 (Slice 26, session security)
- The Noise Protocol Framework — https://noiseprotocol.org/noise.html: handshake patterns built from DH tokens; NX (`-> e  <- e, ee, s, es`) gives an anonymous initiator and a responder that transmits and proves its static key; a running hash of the transcript is authenticated. Adopted the NX shape and the transcript hash.
- Stratum V2 protocol security — https://stratumprotocol.org/specification/04-protocol-security/: a production protocol using Noise NX for the same client/server trust model (clients verify the server they connect to; servers do not authenticate clients in the handshake), terminating the session on any MAC failure. Supported choosing NX over XX.
- Monocypher — https://monocypher.org/: XChaCha20-Poly1305, BLAKE2b, X25519, EdDSA; under 2000 lines, no dependencies, CC0-1.0 or BSD-2-Clause. Chosen library.
- libsodium documentation — https://doc.libsodium.org/: portable NaCl fork, ISC licence, broad platform support. Rejected for build size, kept as the fallback if more primitives are needed.
- Open questions: player identity/accounts without a mandatory backend, rekeying, NAT traversal and relays.

## Research conclusions added 2026-11-17 (Slice 25, divergence reports)
- Factorio wiki "Desynchronization" — https://wiki.factorio.com/Desynchronization: a desync report keeps both the player's and the server's copy of the game state; it records the state at the desync rather than the cause, and its files can be compared with text diff tools. Informed keeping the hashed state lines inside the capture.
- Factorio Friday Facts #47 — https://factorio.com/blog/post/fff-47: a replay that checks the CRC every tick stops at the first desynchronized tick, where the difference is typically just one variable; human-readable tags make the diff usable. Informed reporting the first diverging digest with named entities and readable values.
- Factorio Friday Facts #55 — https://factorio.com/blog/post/fff-55: a cheap "heuristic" CRC of crucial state runs every tick, with full-state checks reserved for debugging. Informed keeping the digest interval at 60 frames with optional detail.
- Open questions: per-entity hashes beyond the detail cap, including script variables in the state lines.

## Research conclusions added 2026-11-16 (Slice 24, regression captures)
- Gaffer On Games "Deterministic Lockstep" — https://gafferongames.com/post/deterministic_lockstep/: the same initial condition and inputs must give exactly the same result; a checksum of the physics state at the end of each frame would be identical; determinism on one machine/binary/OS does not imply it across compilers, OSes or architectures, or even between debug and release. Informed digests every N frames and the cross-platform caveat (our Debug and Release builds did match on this machine).
- Luau standard library — https://luau.org/library: `math.randomseed(seed)` makes later `math.random` calls a deterministic sequence that depends only on the seed (the VM's default seed varies per run). Informed `HostOptions::random_seed`; Luau's implementation takes a 32-bit integer, so the 64-bit capture seed is folded.
- Open questions: cross-platform float determinism of Jolt/Box2D builds, digesting script state, capture of networked sessions.

## Research conclusions added 2026-11-16 (Slice 23, delta snapshots)
- Quake 3 network model (Fabien Sanglard's source review) — https://fabiensanglard.net/quake3/network.php: snapshots are diffed against the last one the client acknowledged; when an update or ack is lost the server keeps using the older acknowledged baseline, so the next packet carries old and new changes; with no usable baseline it diffs against a zeroed dummy snapshot. Adopted directly.
- Gaffer On Games "Snapshot Compression" — https://gafferongames.com/post/snapshot_compression/: encode relative to the most recent acknowledged snapshot; quantize positions (512 values/m) and orientations with smallest-three (29 bits absolute, ~23 bits as deltas). Informed the millimetre quantization; smallest-three and bit packing deferred.
- Open questions: bit packing vs varints measured on real game scenes, per-entity baselines when the newest ack is missing, compressing reliable variable diffs.

## Research conclusions added 2026-11-16 (Slice 22, save games)
- Unreal Engine saving and loading — https://dev.epicgames.com/documentation/en-us/unreal-engine/saving-and-loading-your-game-in-unreal-engine: `USaveGame` objects saved to a slot name + user index; multiple slots per user. Informed named slots and slot listing.
- Godot "Saving games" — https://docs.godotengine.org/en/stable/tutorials/io/saving_games.html: persistable nodes return dictionaries from `save()`, written as JSON lines and recreated on load; JSON is readable but larger than binary `store_var`. Informed the per-script `save_state`/`load_state` hooks and JSON payloads.
- Open questions: binary/compressed checkpoints for large worlds (benchmark first), platform save locations and cloud sync per store, capturing physics velocities and playback state.

## Research conclusions added 2026-11-16 (Slice 21, prefabs)
- Unity prefabs — https://docs.unity3d.com/Manual/Prefabs.html: reusable GameObject templates; instance overrides on components, data or child objects; variants as predefined variations. Informed the override and Apply/Revert UX; property-level overrides and variants deferred.
- Godot PackedScene — https://docs.godotengine.org/en/stable/classes/class_packedscene.html: a serialized scene whose `instantiate()` creates the node hierarchy and child scenes. Informed `instantiate` as a scene-format snapshot rather than a separate format.
- Open questions: property-level override storage, variant inheritance chains, replicating runtime spawns.

## Research conclusions added 2026-11-16 (Slice 20, hot reload)
- SDL3 `SDL_ReleaseGPUTexture` — https://wiki.libsdl.org/SDL3/SDL_ReleaseGPUTexture: the texture is freed "as soon as it is safe to do so", so evicting a texture between frames while earlier command buffers are in flight is safe; the handle must not be used afterwards (the cache drops it).
- efsw — https://github.com/SpartanJ/efsw (MIT): file watcher over inotify, FSEvents/kqueue, Windows I/O completion ports (ReadDirectoryChangesExW/W) with a generic polling fallback. Not adopted for now (ADR-0022); the candidate if polling becomes too slow.
- Open questions: debounce for editors that write via temporary files, font atlas rebuilds for font reload, scene reload policy (merge vs restart).

## Research conclusions added 2026-11-16 (Slice 18, input)
- SDL3 gamepad API — https://wiki.libsdl.org/SDL3/CategoryGamepad (zlib). `SDL_OpenGamepad`, `SDL_GetGamepadButton/Axis` and `SDL_EVENT_GAMEPAD_ADDED/REMOVED` over a standard layout (`SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH`, shoulders, sticks, D-pad; axes `LEFTX/LEFTY/RIGHTX/RIGHTY/LEFT_TRIGGER/RIGHT_TRIGGER`, sticks −32768..32767, triggers 0..32767). VICTUS binding names mirror these positional names so one map fits Xbox, PlayStation and Switch pads.
- gainput — https://github.com/jkuhlmann/gainput (MIT). Action mapping with its own device layer; archived and unmaintained since 2021-09-07; considered and not adopted (ADR-0021).
- Open questions: per-player device assignment for local multiplayer, touch gestures, rumble, persisted user rebinding.

## Research conclusions added 2026-11-16 (Slice 17, networking)
- ENet 1.3.18 — https://github.com/lsalzman/enet (MIT). Reliable and unreliable sequenced channels over UDP with fragmentation and RTT estimates; no encryption. Used behind `net::Transport`.
- Valve GameNetworkingSockets v1.6.1 — https://github.com/ValveSoftware/GameNetworkingSockets/releases/tag/v1.6.1 (BSD-3). Encryption and P2P, but requires OpenSSL/libsodium and protobuf; the candidate transport for authenticated public servers.
- yojimbo v1.15.2 — https://github.com/mas-bandwidth/yojimbo (BSD-3). Connect tokens and encrypted packets; bundles libsodium, netcode and reliable.
- Snapshot interpolation and client-side prediction follow the common model of rendering remote entities a fixed delay (here 100 ms ≈ 3 snapshots at 30 Hz) in the past and predicting only the locally owned entity, correcting when the server state diverges.
- Open questions: delta compression against acknowledged snapshots, physics rollback for full input-replay reconciliation, lag compensation, encryption/authentication, NAT traversal.
