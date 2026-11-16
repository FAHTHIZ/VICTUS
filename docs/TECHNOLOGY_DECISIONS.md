# Technology Decisions

A candidate is not adopted until exact version, license, platform matrix and maintenance status are verified.

| Area | Current position | Status |
|---|---|---|
| Language | C++20 baseline | Provisional |
| Build | CMake + CTest | Implemented |
| Graphics | Compare bgfx, Diligent, direct APIs behind RHI | Open |
| ECS | Compare Flecs, EnTT, custom SoA | Open |
| Scheduler | Compare Taskflow, enkiTS, custom | Open |
| Physics | Jolt candidate | Open |
| Navigation | Recast/Detour candidate | Open |
| Animation | ozz-animation candidate | Open |
| Editor UI | Dear ImGui vs Qt/Avalonia/native shell | Open |
| Asset interchange | glTF 2.0 first; evaluate OpenUSD | Open |
| Audio | miniaudio candidate | Open |
| Scripting | Compare Lua, Wren, AngelScript, managed options | Open |

## ADR-0001 — C++20 dependency-light foundation
Accepted for bootstrap. Standard-library-only core gives CI a reliable baseline. Revisit C++23 only after compiler/platform validation.

## ADR-0002 — Main-branch delivery
Accepted by owner. Commit coherent verified increments directly to main; keep commits small, preserve history and never force-push.

## ADR-0003 — Graphics backend
Open; prototype required. Compare bgfx, Diligent, direct Vulkan/Metal/D3D12 behind VICTUS RHI and Filament as renderer/reference. Validate Windows/Linux/Android/Apple backend matrix, shader tooling, resource lifetime, multithreaded recording, debugging, build complexity, license and maintenance. Use identical workloads.

## ADR-0004 — ECS and scheduler
Open; benchmark required. Compare Flecs, EnTT and focused SoA with identical data/access patterns. Compare Taskflow and enkiTS/custom scheduler for simulation, streaming and editor workloads. Measure memory, iteration, structural changes, scheduling overhead, worker use and platform support. Retain migration seams.

## ADR-0005 — Editor
Open. Prototype Dear ImGui docking for internal tools and richer shell options (Qt, Avalonia or native) for accessibility/text/assets. Review Qt licensing and packaging.

## ADR-0006 — Original license
Selected: Apache-2.0 for original VICTUS code. Third-party components retain their own licenses/notices. Maintain SBOM.

## ADR template
Date/status; problem/constraints; candidates; exact versions and primary sources; license/platform matrix; prototype/benchmark method/results; decision/rejected options; risks/rollback/revisit trigger.
