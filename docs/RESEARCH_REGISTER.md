# Research Register

Prefer primary documentation, upstream repositories, release notes, issues, benchmarks and exact license texts. Record URL, access date, version/commit, findings and uncertainty.

## Engine references
| Project | Study | VICTUS lesson | Caution |
|---|---|---|---|
| Unreal | World Partition, Nanite, Lumen, Mass Entity, PCG, cooking/editor UX | Streaming, virtualized rendering, data-oriented simulation, authoring | Proprietary implementation; public docs/behavior only |
| Unity | Entities/DOTS, Burst, Jobs, SRP, Addressables, Profiler | Data-oriented execution, jobs, render pipelines | Proprietary implementation; public docs only |
| Godot | Renderer tiers, scene/resources, editor/import/export | Modular renderer and accessible editor | Evaluate scalability/trade-offs |
| O3DE | Gems, Atom, Asset Processor, serialization | Modular architecture and content processing | Build/maintenance footprint |
| Bevy | ECS, schedules, reflection, render graph | Parallel scheduling/data-driven design | Rust ecosystem/editor maturity |
| Filament | PBR, clustered forward, mobile performance | Scalable renderer | Not a complete editor/engine |
| bgfx | Backend abstraction/shader tooling | RHI candidate | Verify backend/platform matrix |
| Diligent | Explicit resources and HLSL pipeline | Modern RHI candidate | Verify current exact support |
| Blender | Geometry Nodes/asset automation | Reuse mature creator tooling | Keep external; don't rebuild Blender |

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
| bgfx/bx/bimg | Graphics abstraction | BSD-2-Clause family; verify each |
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

Shortlist only, not legal determination or adopted dependencies. Check bundled licenses.

## Research questions
Graphics: actual backend/platform support; Android Vulkan/fallback; Apple Metal; shader compile/reflection/hot reload; abstraction overhead; multithreaded recording/synchronization.
Simulation: ECS layout on VICTUS workloads; scheduler affinity/cancellation; LOD invariants/causality; determinism across CPU architectures.
Editor/assets: undo/redo, serialization, recovery, node graph UX, import formats, OpenUSD necessity, cook/cache/versioning.
Mobile: memory budgets, thermals, texture compression, suspend/resume, minimum device matrix and physical-device CI limits.

Search snippets are discovery aids, not license proof. Check upstream LICENSE and bundled notices before adoption and upgrades.
