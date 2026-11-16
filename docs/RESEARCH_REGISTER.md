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
