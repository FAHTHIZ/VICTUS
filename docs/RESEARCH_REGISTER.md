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

## Research conclusions added 2026-11-17 (Slice 42, tilemaps)
- Godot, "Using TileSets" (https://docs.godotengine.org/en/stable/tutorials/2d/using_tilesets.html, accessed 2026-11-17): a TileSet atlas is created from a texture grid of a fixed tile size; collision comes from physics layers with per-tile polygons; terrain sets match corners and sides, corners, or sides, with `-1` for empty neighbours. Adopted: grid atlas (`columns`/`rows`), per-tile collision kinds, side-matching terrains with `-1` for unset entries. Deferred: per-tile polygons and corner matching.
- Unity, "Tilemap Collider 2D component reference" (https://docs.unity3d.com/Manual/tilemaps/work-with-tilemaps/tilemap-collider-2d-reference.html, accessed 2026-11-17): tile collision shapes are generated per tile and can be merged through a Composite Collider 2D. Adopted: VICTUS merges solid cells into rectangles on one static body by default.
- Open questions: chunked drawing with camera culling for very large maps, slopes, animated tiles, corner-aware (47-tile) terrains.

## Research conclusions added 2026-11-17 (Slice 41, procedural meshes and helper processes)
- Unity `Mesh.SetVertexBufferData` (https://docs.unity3d.com/ScriptReference/Mesh.SetVertexBufferData.html, accessed 2026-11-17): copies packed vertex data directly into a vertex buffer whose layout was declared first with `SetVertexBufferParams`, with minimal validation for speed. Adopted: VICTUS `set_mesh_data` takes a declared `layout` string and decodes packed buffers in C++.
- Godot `ArrayMesh` (https://docs.godotengine.org/en/stable/classes/class_arraymesh.html, accessed 2026-11-17): `add_surface_from_arrays` builds a surface from vertex, normal, colour, UV and optional index arrays; front faces wind clockwise. Adopted as the table form of `set_mesh_data`; winding is explicit (`flip_winding`, mirrored `scale`) because source engines differ.
- Open questions: mesh colliders from procedural data, saving procedural meshes into scenes, bridges in multiplayer editor clients.

## Research conclusions added 2026-11-17 (Slice 40, pixel-art texture filtering)
- Unity, "Default texture Import Settings window reference" (https://docs.unity3d.com/Manual/texture-type-default.html, accessed 2026-11-17): Filter Mode is a per-texture import setting; Point (no filter) "appears blocky up close", Bilinear "appears blurry up close". Adopted: the filter lives in VICTUS texture import settings (ADR-0039).
- Godot, CanvasItem (https://docs.godotengine.org/en/stable/classes/class_canvasitem.html, accessed 2026-11-17): `texture_filter` is "the filtering mode used to render this CanvasItem's texture(s)"; nearest "reads from the nearest pixel only". Considered and rejected as the primary switch (per-sprite setting repeats across every sprite of a sheet); VICTUS keeps a per-sprite `nearest` flag only at the render level.

## Research conclusions added 2026-11-17 (Slice 39, sprite sheets and sprite animation)
- Godot `Sprite2D` — https://docs.godotengine.org/en/stable/classes/class_sprite2d.html (accessed 2026-11-17): `hframes`/`vframes` are the columns and rows of a sprite sheet and `frame` the cell shown; `region_rect` draws a region of an atlas texture. VICTUS adopts the same grid model (`columns`, `rows`, frames numbered row by row) and exposes the region as `Sprite.uv`.
- Godot `SpriteFrames` — https://docs.godotengine.org/en/stable/classes/class_spriteframes.html (accessed 2026-11-17): a resource of named animations, each with frames, a playback speed in frames per second and a loop mode (none, linear, ping-pong). VICTUS sheets hold named clips with frame lists, fps and a loop flag; ping-pong is expressed by listing frames (`[0,1,2,1]`) and per-frame durations by repeating frames.
- Open questions: packed (non-grid) atlases with per-frame rectangles, per-frame events, half-texel insets or nearest filtering for pixel art, Aseprite import.

## Research conclusions added 2026-11-17 (Slice 38, scene transitions)
- Godot `SceneTree.change_scene_to_file` — https://docs.godotengine.org/en/stable/classes/class_scenetree.html (accessed 2026-11-17): the outgoing scene is removed immediately and the new scene is added at the end of the frame. VICTUS also defers the switch, but keeps the outgoing scene fully alive until the end of the update so the rest of the frame's scripts see a consistent world.
- Unity `SceneManager.LoadScene` — https://docs.unity3d.com/ScriptReference/SceneManagement.SceneManager.LoadScene.html (accessed 2026-11-17): a single-mode load completes in the next frame, not immediately, and unloads unused assets. VICTUS applies the switch at the end of the calling update and validates the target before accepting the request, so a bad path fails at the call site instead of a frame later.
- Open questions: additive/streamed scenes, asynchronous loading with progress, persistent entities across scenes, built-in fade transitions.

## Research conclusions added 2026-11-17 (Slice 37, material instances)
- Unreal Engine, "Instanced Materials" — https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-materials-in-unreal-engine (accessed 2026-11-17): instances inherit all attributes from a parent and look identical until edited; only parameterised attributes are editable; constant instances are fixed before run time, dynamic instances are created from scripts. VICTUS adopts constant instances with every field overridable; run-time instances remain open (scripts use `MaterialOverrides` or swap materials).
- Open questions: parent-locked fields, run-time instances from scripts, caching resolved parents for deep chains.

## Research conclusions added 2026-11-17 (Slice 36, project surface shaders)
- Godot spatial shaders — https://docs.godotengine.org/en/stable/tutorials/shaders/shader_reference/spatial_shader.html (accessed 2026-11-17): `fragment()` sets material properties (`ALBEDO`, `ROUGHNESS`, `METALLIC`, `EMISSION`, `NORMAL_MAP`, `AO`, `ALPHA`, ...) and the built-in renderer does the shading; `TIME` is seconds since start and repeats after 3 600 s. VICTUS mirrors this split (surface function before built-in lighting) and the hourly time wrap.
- glslang releases — https://github.com/KhronosGroup/glslang/releases/tag/16.6.0 (accessed 2026-11-17): 16.6.0 released 2026-09-10; adopted as a library. Its `LICENSE.txt` covers glslang proper with BSD-3-Clause, BSD-2-Clause, MIT and Apache-2.0 parts; the GPL-3.0-with-Bison-exception text applies to Bison files, which the file says were removed.
- SPIRV-Cross — https://github.com/KhronosGroup/SPIRV-Cross (accessed 2026-11-17): Apache-2.0 library for SPIR-V reflection and conversion to other languages, including MSL; used for MSL output and resource counts.
- Open questions: shader caches in exported packs, async compilation, material graphs that generate surface code, sprite/particle/post-process hooks.

## Research conclusions added 2026-11-17 (Slice 33, material assets)
- glTF KHR_texture_transform — https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_texture_transform/README.md (accessed 2026-11-17): an affine UV transform applied as scale, then rotation, then translation (`translation * rotation * scale`). VICTUS materials store scale and offset in the same order (`uv * scale + offset`), so glTF transforms without rotation map 1:1 if imported later.
- Godot BaseMaterial3D — https://docs.godotengine.org/en/stable/classes/class_basematerial3d.html (accessed 2026-11-17): `uv1_scale` multiplies and `uv1_offset` adds to UVs; `texture_filter` offers nearest/linear with and without mipmaps and anisotropy. VICTUS starts with linear (trilinear, mipmapped) and nearest; anisotropic filtering is an open item.
- Open questions: material graphs and project-authored shaders, triplanar mapping, per-texture sampler overrides, material instances (parent + overrides).

## Research conclusions added 2026-11-17 (Slice 31, in-editor clients)
- Unreal Engine PIE multiplayer options — https://dev.epicgames.com/documentation/en-us/unreal-engine/play-in-editor-multiplayer-options-in-unreal-engine (accessed 2026-11-17): Number of Players and Play Number of Clients set how many client instances PIE creates alongside a listen server. VICTUS adopts the count-based model inside one process.
- Unity Multiplayer Play Mode — https://docs-multiplayer.unity3d.com/mppm/current/about/ (accessed 2026-11-17): up to four players at once (the main Editor Player and three Virtual Players, which open in separate windows; the page does not say whether they are separate processes). Informed the small cap (3 extra, 4 total).
- Open questions: split-screen viewing of several peers, bots for idle peers, cost of many full client worlds.

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

## Research conclusions added 2026-11-18 (animation controllers, ADR-0068)
- Rune Skovbo Johansen, "Automated Semi-Procedural Animation for Character Locomotion" (MSc thesis, Aarhus 2009) sec. 6.3 — https://runevision.com/thesis/rune_skovbo_johansen_thesis.pdf (accessed 2026-11-18): gradient-band interpolation; each example's influence is the minimum over the others of a linear falloff along the vector between them, clamped at zero and normalized. Exact at examples, smooth, defined everywhere. Adopted for 2D blend spaces (Cartesian variant).
- Unity forum answer on 2D blend weights — https://discussions.unity.com/t/how-weights-of-2d-blending-are-calculated/167823 (accessed 2026-11-18): Unity's Freeform Cartesian 2D blend type uses that gradient-band method. Confirms it is production-proven for locomotion.
- Godot AnimationNodeBlendSpace2D — https://docs.godotengine.org/en/4.7/classes/class_animationnodeblendspace2d.html (accessed 2026-11-18): blends with triangles (auto-triangulated); points outside the triangulation snap to the nearest edge. Considered; gradient bands need no triangulation and need no special handling outside the hull.
- Unity Manual, Animation transitions (6000.6) — https://docs.unity3d.com/Manual/class-Transition.html (accessed 2026-11-18): exit time is the source state's normalized time; on looping states exit times below 1 are evaluated every loop and true only on the frame they are crossed, exit times above 1 once; with conditions, all must hold; any-state transitions are queued first. Adopted the exit-time and ordering semantics; per-transition interruption sources are not offered (the destination's transitions always may interrupt).
- Godot AnimationMixer (stable) — https://docs.godotengine.org/en/stable/classes/class_animationmixer.html (accessed 2026-11-18): `root_motion_track` names the node; its position/rotation/scale are "canceled visually" so the animation stays in place, and `get_root_motion_position()`/`get_root_motion_rotation()` return per-frame deltas the game applies, e.g. `velocity = rotation * root_motion_position / delta` into a `CharacterBody3D`. Informed reporting the delta (not only applying it) and the Luau `entity:root_motion()` usage with `move_character`. Godot warns its accumulator values jump when a clip loops; VICTUS unwraps the cycle instead.
- Unity Manual, How Root Motion works (6000.6) — https://docs.unity3d.com/Manual/RootMotion.html (accessed 2026-11-18): each frame the change of the Root Transform (the body projected onto the ground plane) is applied to the GameObject; "Bake Into Pose" keeps Y position (most clips), XZ (idles, avoids drift) or rotation in the pose instead. Generic rigs use a chosen Root Node. Informed the defaults: XZ travel and yaw become motion, Y stays in the pose, a controller-level `root_node`, and per-state opt-in so idles never drift.
- Open questions: layered/additive blending with masks, sync markers (foot phase) instead of normalized-time sync, sub-state machines, root motion on 2D blend spaces with turning clips (the yaw/travel coupling is approximated per clip), root motion through physics bodies other than script-applied CharacterController moves.

## Research conclusions added 2026-11-18 (competitor benchmark, Workstream F)
- Godot "List of features" (4.7 stable docs) — https://docs.godotengine.org/en/stable/about/list_of_features.html (accessed 2026-11-18). Describes three renderers (Forward+, Mobile, Compatibility); GI via lightmaps, VoxelGI, SDFGI and SSIL; a visual profiler with CPU and GPU time per pipeline step plus Tracy/Perfetto; GDExtension with official C/C++ bindings; forward and inverse kinematics. Web export is experimental and C# web export is unsupported.
- Godot releases — https://github.com/godotengine/godot/releases (accessed 2026-11-18): 4.7.2 stable is from 2026-08-18; 4.8 beta3 from 2026-11-09 (https://godotengine.org/download/archive/index.html).
- Unity 6.3 manual — https://docs.unity.com/en-us/engine/6000.3/manual and system requirements — https://docs.unity.com/en-us/engine/6000.3/manual/get-started/install-and-upgrade/getting-started-installing-unity/system-requirements (accessed 2026-11-18). 6.3 is the current LTS. Players: Android 7.1+ (GLES 3.0/Vulkan), iOS 15+ (Metal), tvOS, Web (WebGL 2/WebAssembly), Windows/UWP/macOS/Linux, dedicated servers, XR devices, embedded Linux/QNX. Consoles are available to registered developers.
- Unreal Engine 5.8 release notes — https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes (accessed 2026-11-18). Mobile defaults to multi-pass deferred; iOS SM6 is experimental; Windows ARM64 game targets are experimental; Android NDK r27c. Hardware specs (https://dev.epicgames.com/documentation/unreal-engine/hardware-and-software-specifications-for-unreal-engine) list the Lumen/Nanite/VSM/MegaLights requirements.
- Alternatives considered for the benchmark form: (a) a weighted single score per engine, rejected because the handoff forbids it and it hides the device-proof gap; (b) a per-workflow table with evidence and classification, chosen. Side-by-side runtime measurements were not possible: there is no GPU and no competitor install on this machine.
- Open questions: add Bevy/O3DE/Defold rows with sources; link specific pages for the lower-confidence Unity/Unreal cells; re-measure once VICTUS runs on a real device.

## Research conclusions added 2026-11-18 (thin geometry below a pixel)
- Emil Persson, "Phone-wire AA", 2012 — https://www.humus.name/index.php?page=3D&ID=89 (accessed 2026-11-18): thin wires "get broken up into a bunch of random disconnected dots after a distance"; MSAA only pushes the problem further away. Fix: clamp the radius to a pixel-wide wire and fade alpha by the reduction ratio. Matches the dashes measured without TAA.
- Persson, "Graphics Gems for Games: Findings from Avalanche Studios", SIGGRAPH 2012 — https://humus.name/Articles/Persson_GraphicsGemsForGames.pdf (accessed 2026-11-18): wires are stored as centre, normal and radius so the vertex shader can widen them; also applies to "antenna towers, railings, bars". Informed the open follow-up (a wire mesh format).
- "The Sane Rendering Manifesto" — https://gist.github.com/bazhenovc/c0aa56cdf50df495fda84de58ef1de5e (accessed 2026-11-18): TAA only partly resolves sub-pixel geometry; Source 2 uses the phone-wire technique for wire entities. Consistent with the TAA energy loss measured here.
- Open questions: phone-wire AA for a wire/rail component, whether TAA's clamp should widen for thin features, MSAA on the forward path.

## Research conclusions added 2026-11-18 (anisotropic filtering setting)
- Godot `Viewport.anisotropic_filtering_level` (4.4) — https://docs.godotengine.org/es/4.4/classes/class_viewport.html (accessed 2026-11-18): power-of-two levels 2x/4x/8x/16x, 4x by default, 0 forcibly disables it; materials opt in through an anisotropic texture filter. Informed power-of-two levels with a viewport-wide cap.
- Godot 3.5 `rendering/quality/filters/anisotropic_filter_level` — https://docs.godotengine.org/en/3.5/classes/class_projectsettings.html (accessed 2026-11-18): only 1 and powers of two are valid, 1 disables it, "higher values will result in sharper textures when viewed from oblique angles, at the cost of performance". Informed rounding other values down.
- Unity `AnisotropicFiltering` (6000.5) — https://docs.unity.com/en-us/engine/6000.5/script-reference/unityengine/anisotropicfiltering (accessed 2026-11-18): Disable, Enable (per texture) or ForceEnable (all textures) per quality level. Informed tying the level to the quality tier rather than per texture.
- Alternatives compared: a per-texture import flag (Godot, Unity "Enable") and a single fixed level (the previous hard-coded 8x). Chosen: one per-tier cap for all linear material textures, with nearest-filtered materials excluded; a per-texture override stays open.
- Evidence: `gpu_anisotropy_tests.cpp`, a 0.5 m checkerboard floor seen from 1.5 m at 192x108: mean adjacent-pixel contrast in the rows just below the horizon is 0.28 at 1x, 3.78 at 4x and 7.29 at 16x on lavapipe.
- Open questions: a per-texture override, the cost on tile-based mobile GPUs (needs hardware), anisotropy for terrain splat layers once they use their own samplers.

## Research conclusions added 2026-11-18 (overdraw debug view)
- Godot `Viewport.DebugDraw` — https://docs.godotengine.org/en/latest/classes/class_viewport.html (accessed 2026-11-18): `DEBUG_DRAW_OVERDRAW` displays objects semi-transparent with additive blending, using a debug material that ignores custom shaders. Informed redrawing with one debug material and additive blending.
- Godot "Using Viewports" — https://docs.godotengine.org/en/stable/tutorials/rendering/viewports.html (accessed 2026-11-18): debug draw is a per-viewport mode, not a project setting. Informed keeping the view editor-only.
- Unreal `EViewModeIndex` (5.8) — https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/EViewModeIndex?lang=en-US (accessed 2026-11-18): Quad Overdraw is "colored according to the quad coverage"; Shader Complexity is a separate mode. Informed keeping depth complexity separate from shading cost.
- ibbles, LearningUnreal "Rendering Performance Profiling And Analysis" — https://github.com/ibbles/LearningUnreal/blob/main/Rendering%20Performance%20Profiling%20And%20Analysis.md (accessed 2026-11-18): Quad Overdraw shows how many times each pixel is rendered, blue good, red bad, white very bad. Informed the heat ramp.
- Alternatives compared: quad overdraw with fragment-stage atomics and pipeline-statistics queries; see ADR-0071.
- Open questions: quad (2x2) overdraw, per-pixel shader cost, counting particles and decals, an on-screen histogram.

## Research conclusions added 2026-11-18 (alpha-tested surfaces during zoom)
- Ignacio Castaño, "Computing Alpha Mipmaps", The Witness blog, 2010-09-09 — http://the-witness.net/news/2010/09/computing-alpha-mipmaps/ (accessed 2026-11-18): define coverage as the fraction of texels with alpha above the test value; per mip, bisect for the reference value giving the same coverage and scale alpha by test value / reference. Adopted as `render::preserve_alpha_coverage` (ADR-0070).
- Castaño, "Runtime Mipmap Generation", 2026 — https://www.ludicon.com/castano/blog/2026/02/runtime-mipmap-generation/ (accessed 2026-11-18): the method is now the de facto industry standard; precomputed per-level alpha scales suit textures known in advance. Supports cook-time application.
- Unity `TextureImporter.mipMapsPreserveCoverage` and `alphaTestReferenceValue` — https://docs.unity3d.com/cn/6000.0/ScriptReference/TextureImporter-mipMapsPreserveCoverage.html and https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TextureImporter-alphaTestReferenceValue.html (accessed 2026-11-18): a per-texture opt-in with a reference value. Informed `alpha_coverage_cutoff` as an import setting.
- Wyman and McGuire, "Hashed Alpha Testing", I3D 2017 — https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf (accessed 2026-11-18): even perfect per-level thresholds do not stop thin geometry eroding with distance; a hashed (stochastic) threshold does. Matches the 6 m dip measured in the GPU test; recorded as the follow-up.
- lisyarus, "Exploring ways to mipmap alpha-tested textures" — https://lisyarus.github.io/blog/posts/exploring-ways-to-mipmap-alpha-tested-textures.html (accessed 2026-11-18): compares box filtering, shader LOD scaling (Golus) and the coverage method; coverage preservation guarantees the texture does not disappear but can fill holes and shrink thin features.
- Alternatives compared: shader-side alpha scale per LOD, hashed alpha testing, alpha-to-coverage; see ADR-0070.
- Open questions: hashed alpha or alpha-to-coverage for sub-texel erosion, alpha-weighted colour filtering to stop halos on straight-alpha textures.

## Research conclusions added 2026-11-18 (camera-responsive LOD, shadow LOD)
- Godot internal rendering architecture — https://docs.godotengine.org/en/4.4/contributing/development/core_and_modules/internal_rendering_architecture.html (accessed 2026-11-18): mesh LOD is chosen per viewport from a screen-coverage metric that accounts for resolution and FOV; shadow and reflection-probe rendering choose their own LOD thresholds, which can differ from the main scene; LOD generation also builds welded shadow meshes. Matches VICTUS's per-viewport, FOV- and resolution-aware rule; informed `shadow_lod_bias` (ADR-0049 addendum) and the open question of shadow-only meshes.
- Unreal Engine rendering FAQ — https://dev.epicgames.com/community/learning/knowledge-base/ePyY/unreal-engine-faq-rendering (accessed 2026-11-18): suggests a shadow LOD bias (a lower LOD for shadow rendering) as a shadow-depth cost reduction. Same direction as `shadow_lod_bias`.
- Unreal virtual shadow maps — https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine (accessed 2026-11-18): non-Nanite meshes need LOD hierarchies or become expensive to render into small shadow pages. Supports keeping casters on LODs.
- Alternatives compared: (a) keep main-view LOD in shadows; (b) independent per-cascade selection from shadow texel density; (c) main-view selection plus a per-tier bias. Chose (c): (b) can pick a different silhouette for shadows than for the lit surface (self-shadowing mismatch) and needs per-cascade batching; (c) is a bounded, opt-in cost trade.
- Open questions: welded shadow-only index lists, hysteresis/dithered transitions (6e A.E.G.I.S.), per-view budgets for secondary views.

## Research conclusions added 2026-11-18 (starter characters)
- Blender license — https://www.blender.org/about/license/ (accessed 2026-11-18): what you create with Blender is your sole property; scripts using the `bpy` API must, if published, be under a GPL-compliant licence. Informed ADR-0069 (generator under Apache-2.0, GLBs owned by VICTUS).
- FSF licence list — https://www.gnu.org/licenses/license-list.html (accessed 2026-11-18): Apache-2.0 is compatible with GPLv3 (not GPLv2). Blender 2.8+ is GPL-2.0-or-later, so the generator combines under GPLv3.
- Blender glTF 2.0 exporter manual — https://docs.blender.org/manual/en/latest/addons/import_export/scene_gltf2.html (accessed 2026-11-18): Actions mode exports each stashed/NLA-tracked action as one glTF animation named after its track; "Deformation Bones only" bakes animation onto deform bones; "Active" vertex colour exports `COLOR_0` even when the material does not use it. Used for the export settings in `tools/characters/build_vanguard.py`.
- Khronos glTF 2.0 specification — https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html (accessed 2026-11-18): +Y up, +Z forward for assets, metres; `COLOR_0` multiplies base colour; skins use inverse bind matrices with joints as nodes. Informed the conventions (1.70 m, feet at Y=0, facing +Z) and the socket-as-node design.
- Alternatives compared: procedural C++ meshes (the previous robot) and a data install directory; see ADR-0069.
- Open questions: weighted skinning for soft shells, a formal socket component, authored (not simplified) LODs, retargeting other humanoids onto the Vanguard skeleton.

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

## Research conclusions added 2026-11-18 (morph targets, ADR-0076)
- Khronos glTF 2.0 specification, 3.7.2.2 Morph Targets — https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html: attributes are the base plus the weighted sum of target deltas; `mesh.weights` are defaults that `node.weights` overrides; all primitives of a mesh share the weights; names are not specified, most tools use `mesh.extras.targetNames`. VICTUS follows all of these (names fall back to `target<N>`).
- Unity SkinnedMeshRenderer.SetBlendShapeWeight — https://docs.unity3d.com/6/Documentation/ScriptReference/SkinnedMeshRenderer.SetBlendShapeWeight.html: weights set per renderer by index. VICTUS offers index and name, per entity.
- Godot MeshInstance3D — https://docs.godotengine.org/en/4.1/classes/class_meshinstance3d.html: blend shape values set per instance by index, with a name lookup. Same model as VICTUS.
- Open questions: GPU morphing (compute pre-pass or vertex-shader target buffers) for crowds, tangent deltas, motion vectors from the previous morphed shape, saving script-set weights.

## Research conclusions added 2026-11-18 (retargeting, ADR-0075)
- Unity, Retarget Humanoid animations — https://docs.unity3d.com/6/Documentation/Manual/Retargeting.html: retargeting is only possible for humanoid models with a configured Avatar, which gives the correspondence between bone structures. VICTUS maps bones by name for any rig instead, without an avatar step.
- Unreal Engine, IK Rig Retargeting — https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine?application_version=5.2: transfers animation between skeletons with different bone counts, names and orientations, optionally keeping hand and foot contacts with IK. VICTUS handles different names and orientations; contact preservation is left to IKConstraint.
- Godot, Retargeting 3D Skeletons — https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/retargeting_3d_skeletons.html: BoneMap onto SkeletonProfileHumanoid with pattern-matched auto-mapping; position tracks other than the root and Hips removed; position tracks normalized by the Hips height ("motion_scale"). VICTUS matches the same rules: rotation-only below the root, root travel scaled by rest height.
- Open questions: humanoid profile with A/T-pose correction, chain-based mapping with IK for contacts, an editor bone-map panel.

## Research conclusions added 2026-11-18 (controller layers, ADR-0074)
- Unity AnimatorLayerBlendingMode.Additive — https://docs.unity3d.com/6000.2/Documentation/ScriptReference/Animations.AnimatorLayerBlendingMode.Additive.html: "based on the offset from the 1st frame of the animation to its current frame". VICTUS `reference: first_frame` matches; `rest` is offered for clips authored against the bind pose.
- Unity Learn, Animator Layers — https://learn.unity.com/tutorial/3-7-creating-and-configuring-animator-layers: layers evaluate in index order, each blended with the result so far; additive layers leave lower layers at effective weight 1.
- Open questions: per-bone mask weights, layer timing sync, a general blend graph.

## Research conclusions added 2026-11-18 (inverse kinematics, ADR-0073)
- FABRIK — Aristidou & Lasenby, Graphical Models 73(5) 243–260, 2011, https://doi.org/10.1016/j.gmod.2011.05.003 (paper: http://www.andreasaristidou.com/publications/papers/FABRIK.pdf): forward/backward passes place each joint on a line at its bone length; root fixed; unreachable targets give a straight chain toward the target. Used for `chain`.
- Godot FABRIK3D — https://docs.godotengine.org/en/stable/classes/class_fabrik3d.html: position-based, keeps the previous pose's twist, zig-zags when the target is near the root. Godot TwoBoneIK3D — https://docs.godotengine.org/en/stable/classes/class_twoboneik3d.html: plane from joints and pole, intersection of two circles. VICTUS two-bone follows the same plane construction and falls back to the current bend when there is no pole.
- Open questions: joint limits (cone/hinge) for chains, CCD as a third chain solver, full-body IK, foot locking with root adjustment.

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
