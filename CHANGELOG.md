# Changelog

All notable changes to VICTUS are recorded here. The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html); before 1.0.0 a minor version may change public C++ APIs (see "Compatibility" in [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)). Project, scene and asset file formats carry their own version numbers and are migrated on load; any format change is listed under "Changed".

Every entry links to the evidence in [docs/STATUS.md](docs/STATUS.md); nothing here claims more than STATUS verifies. Platform and hardware limits are in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).

## [Unreleased]

No VICTUS version has been released yet. The first tagged release will be 0.2.0 (the version in CMakeLists.txt) and will move this section under `## [0.2.0] - YYYY-MM-DD`.

### Added

- Engine core: results and errors, logging, JSON with limits, sandboxed filesystem helpers, job system, profiler with Chrome/Perfetto traces, math, `.vpak` archives.
- Crash reports, session logs and unclean-exit detection for `victus` and `victus_player`; `victus diagnose` diagnostics bundles with path redaction and opt-outs; offline symbolizer (`tools/crash/symbolize.py`). ADR-0072, docs/DIAGNOSTICS.md.
- Rendering on SDL_GPU (Vulkan verified, Metal and D3D12 compiled): render graph, 2D sprite batching and 2D lights, forward HDR PBR with cascaded shadows, SSAO, bloom, auto exposure, TAA, decals, terrain, particles, quality tiers, spatial upscaling and debug views.
- ECS, scenes with prefabs and UUID identity, asset database with importers (glTF, images, audio), materials and surface shaders.
- Gameplay framework, Luau scripting with sandboxing and hot reload, visual scripting, Jolt 3D and Box2D 2D physics, Recast/Detour navigation, behaviour-tree AI, animation (skinning, controllers, sprite animation), timeline sequencer, in-game UI, localization, audio, save games, tilemaps.
- Multiplayer: client/server sessions over ENet with encryption by default, listen servers, dedicated `victus serve`.
- VICTUS Studio editor: start screen (new project with live validation, recent, samples, learn), docked workspace with a Window menu and five layout presets, viewport gizmos, inspector, asset browser, play mode, Problems panel with background project checks, File > Export Game, Help > Report a Problem (diagnostics bundle), crash notice after an unexpected exit, Help > About, a Hierarchy with search, a grouped Add menu and collapsed deep trees, an Inspector with a searchable Add Component picker and readable field names, a Console with severity filters, search, merged repeats and copy, an Assets panel with search, a type filter and a folder list, and importing files by dropping them on the window or with Import.
- `victus` CLI: `new`, `templates`, `info`, `import`, `run`, `simulate`, `serve`, `check`, `export`, `diagnose`; exported games run with `victus_player`; Android packaging.
- Project templates (3D, 2D, simulation, multiplayer), Vanguard starter characters, sample projects (arena, dungeon crawler, render showcase).
- Release process: this changelog, docs/RELEASE_PROCESS.md, `tools/release/release.py` and the tag-triggered release workflow with separate debug-symbol archives.

### Security

- Untrusted-input hardening for glTF, archives, project files and network packets is described in [SECURITY.md](SECURITY.md) and docs/THIRD_PARTY_AND_LICENSES.md (cgltf advisories are mitigated, not fixed upstream).

[Unreleased]: https://github.com/FAHTHIZ/VICTUS/commits/main
