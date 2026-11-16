# VICTUS Architecture

## Product boundary
VICTUS is general-purpose for games, visualization and simulation. Editor, content pipeline and runtime share project/asset schemas but have distinct build targets.

## Layers
```text
Creator tools: Studio | CLI | Asset tools | Profilers
Project / Asset / Build services
VICTUS APIs: Core | ECS | Tasks | Simulation | Physics | Animation
             Rendering/RHI | Audio | Input | UI | Networking | Scripting
Platform adapters + audited third-party libraries
Windows | Linux | macOS | Android | iOS (validated per release)
```
Dependencies point inward toward VICTUS interfaces. Do not expose third-party types in public APIs without an ADR.

## Runtime/editor separation
Intended modules: core, runtime, editor, asset, render, sim, physics, audio, animation, navigation and tools. Split targets only when there is a real boundary and testable contract; do not create empty modules for appearances. Headless runtime must run without graphics/audio initialization. Editor dependencies must not ship with games.

## Simulation/presentation separation
Authoritative state must not depend on camera visibility, render frame rate or editor viewport. Simulation uses controlled time; rendering consumes safe snapshots. LOD changes update frequency/detail, not declared invariants. Distant regions may use aggregates/event-driven progression. Save/load captures authoritative state, random seeds, schema version and pending events. Headless and graphical runtime share rules.

## Data-oriented execution
Benchmark archetype ECS, sparse-set ECS, SoA and hybrids. Scheduler expresses dependencies, read/write access, affinity and cancellation. Avoid abstractions that obscure simple work.

## Rendering
VICTUS-owned RHI separates engine concepts from backend APIs. Render graph owns resource lifetime, synchronization and transient allocation. Evaluate D3D12, Vulkan, Metal and mobile fallback. Rendering tiers share assets/material definitions but vary pipeline, shader variants, texture formats, geometry LOD, shadows and post effects. Adaptation is measurable and user-overridable.

## Asset pipeline
Source assets are immutable inputs. Importers validate and convert to versioned cooked artifacts. Stable asset IDs survive file moves. Dependency graph supports incremental cooking, cache invalidation, platform variants and diagnostics. Runtime loads cooked artifacts.

## Editor
Editor is a client of engine/project services, not owner of runtime state. Scene editing uses undoable commands and stable object identity. Play-in-editor uses isolated world/snapshot so stopping play cannot corrupt authored content.

## Safety
Prefer interoperable formats (glTF 2.0 first); evaluate OpenUSD for complex scenes. Version project/scene/asset/save schemas. Validate lengths, paths, references and decompression limits. Prevent path traversal and unsafe plugin/script loading.

## Review checklist
For each subsystem: user capability, ownership/lifetime, data access, headless tests, profiling, unsupported-hardware fallback, dependency/license, migration seam, memory/binary/startup cost, editor/runtime/server/mobile behavior.
