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

## Open research questions
Graphics: actual backend/device support; Android Vulkan/fallback; Apple Metal; shader compile/reflection/hot reload; abstraction overhead; multithreaded recording/synchronization; 2D/3D shared resource model.
Gameplay: data-oriented ECS versus higher-level object facade; reflection/serialization; visual graph runtime; prefab references/variants; deterministic gameplay testing.
Simulation: ECS layout on VICTUS workloads; scheduler affinity/cancellation; LOD invariants/causality; determinism across CPU architectures.
Editor/assets: undo/redo, serialization, recovery, node graph UX, import formats, OpenUSD necessity, cook/cache/versioning, project merges and collaboration.
Networking: transport candidates, server tick model, prediction scope, replication bandwidth, relevancy and security threat model.
Mobile/XR: memory budgets, thermals, texture compression, suspend/resume, minimum device matrix, OpenXR runtime coverage and physical-device CI limits.
Production: code/asset hot reload safety, incremental build cache correctness, mod/plugin isolation, package signing, SBOM and crash privacy.

Search snippets and secondary mirrors are discovery aids, not license proof or normative API references. Confirm claims against primary documentation and exact upstream versions before implementation.
