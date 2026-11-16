# VICTUS Gameplay Framework Specification

## Purpose
Provide reusable, engine-level foundations for complete playable games. The framework must remain flexible: small 2D games, large 3D worlds, simulation-heavy projects and dedicated servers should not be forced into one rigid inheritance model.

## Runtime concepts
- **Game instance/session:** application-lifetime services, session configuration and cross-world persistence.
- **World/level:** authoritative entities, systems, world settings, streaming regions and lifecycle.
- **Game rules/mode:** server-authoritative rules for a particular match, level or game mode.
- **Game state/player state:** replicated or local state with explicit authority and persistence semantics.
- **Controller and pawn/character:** input/AI control separated from the entity being controlled; support non-humanoid agents.
- **Camera/view:** player and cinematic viewpoints independent of simulation ownership.
- **Components and systems:** composable behavior with clear ownership and data access.
- **Spawn/despawn and prefab system:** validated reusable entities, variants, overrides and deterministic instantiation where feasible.

## Player and gameplay systems
- Input actions and mapping contexts for keyboard, mouse, gamepad, touch and accessibility devices.
- Character movement, interaction, triggers, collision queries and camera control.
- Gameplay events, timers, tags, attributes, state machines and modular ability/inventory primitives.
- Scene transitions, checkpoints, save slots, profiles, migrations and recovery.
- UI, audio, animation, physics and navigation integration through stable APIs.
- Debug views for entity state, event flow, collision, AI decisions and performance.

## Gameplay AI
Keep gameplay AI distinct from large-scale simulation agents. Support perception/sensing, navigation requests, behavior/state machines, behavior trees or utility-based decision systems, blackboards/data context, squad/crowd coordination and deterministic debugging/replay where feasible. AI decisions must be inspectable and profileable.

## Scripting and visual logic
Provide a documented scripting interface with reflection/serialization rules, error diagnostics, debugging and safe reload boundaries. Visual scripting uses typed nodes, explicit execution/data pins, validation, graph versioning, deterministic serialization, breakpoints and runtime inspection. Graphs compile or execute through a documented runtime; they are not static mockups.

## Prefabs and reusable content
Support reusable entities/scenes, nested composition, variants, overrides, stable references, dependency tracking, validation and safe updates. Undo/redo and source-control-friendly serialization are required.

## Multiplayer semantics
Gameplay rules define authority, ownership, replication policy and prediction behavior. Server and client use shared gameplay contracts, but only the authoritative server commits shared state. See [NETWORKING_SPEC.md](NETWORKING_SPEC.md).

## Acceptance
A complete reference game must demonstrate player control, cameras, world transitions, interaction, physics, AI, animation, UI, audio, saves, scripting/visual logic and packaging. Each feature must have tests and editor workflows; no custom hardcoded demo path may substitute for a reusable engine capability.
