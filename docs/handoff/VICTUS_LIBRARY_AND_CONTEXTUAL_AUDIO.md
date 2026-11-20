# VICTUS Library & Contextual Audio — Implementation Handoff

**Priority:** Owner-approved direction; queued under Roadmap → Creator experience  
**Status:** Research and repository audit complete; implementation not started  
**Target:** Direct, incremental commits to \`main\`; no branch or PR  
**Relationship:** Technical implementation brief complementary to [Studio UX and Workspace Maturity](STUDIO_UX_AND_WORKSPACE_MATURITY.md), not a replacement for it  
**Research date:** 2026-11-20

## Mission

Build a professional, offline-first VICTUS Library experience that is available when VICTUS Studio opens, makes useful starter content easy to discover, imports reusable content safely into projects, and gives VICTUS games an authorable context-aware ambience system.

The Library should feel built into VICTUS rather than like a loose download folder or an external marketplace. A creator should be able to browse available content, preview it, understand its licence and platform suitability, add it to a project, and use it without learning internal asset-pipeline details.

Contextual audio should make the world sound coherent by selecting and blending explicitly authored ambience layers for the environment. It must remain controllable: creators can edit, override or disable automatic selection at project, scene, zone and source level.

**This is a reuse-first project.** VICTUS already has a project asset database, stable asset IDs, import/cook infrastructure, model and texture optimization, thumbnails, audio playback, buses, 3D spatialization and basic acoustic zones. Extend those systems. Do not create competing asset databases, resource managers, importers, audio engines or editor panels.

## Product boundaries and non-goals

### What “VICTUS Library” means

Keep three concepts distinct:

1. **Project Assets** — the source-of-truth for one game project. Existing \`engine/assets::AssetDatabase\` owns project-local UUIDs, metadata, source paths and cooked cache entries. These assets are committed with the project as appropriate.
2. **VICTUS Library Catalogue** — a user-level, offline-readable catalogue of available packs and their metadata. It indexes content the user already has; it is not a replacement for a project database.
3. **Installed Library Content** — reusable source packages stored in a user-level content directory. When a creator chooses **Add to Project**, copy/import the selected content into that project's Assets through the existing import path. The resulting project asset receives/retains project-local identity and must remain usable if the global Library is later unavailable or removed.

The exported game must never depend on a developer's machine-wide Library directory, a network connection, or a Studio-only catalogue.

### MVP exclusions

- No public marketplace, account system, cloud sync, user upload service or unsolicited asset downloads.
- No automatic download of a large content collection on first launch.
- No remote assets in a project at runtime.
- No importing arbitrary archive files or running code from an asset pack.
- No AI model or image-recognition dependency for deciding that an environment “looks like” a forest. Context must be explicit, inspectable and deterministic in the first implementation.
- No replacement of glTF/cgltf, meshoptimizer, the existing BC/ASTC pipeline, miniaudio or V.A.U.L.T.
- No full DCC replacement: Blender and other specialist content-creation tools remain useful authoring tools.
- No promise of a full mobile Studio. Current repository configuration targets Android as a player/runtime build; desktop Studio and mobile player workflows must remain separate unless the platform strategy changes.

## Repository audit — current foundations

The agent must re-check current \`main\` before implementation. The following reflects the repository inspected on 2026-11-20, not a claim that it cannot have changed since then.

### Existing asset pipeline

- \`engine/assets/include/victus/assets/database.hpp\` and \`engine/assets/src/database.cpp\`: project-local database; scans the project's Assets folder; stable UUIDs in sidecar \`.meta\` files; repairs copied duplicate UUIDs; stores importer settings; provides lookup/load APIs; writes/reads a packaged asset index.
- Project source files live under \`<project>/assets\`; import metadata is stored next to the source; disposable cooked data is under \`<project>/.victus/cache\`. Asset identity is UUID-based, not path-only.
- \`engine/assets/importers.hpp\` and \`importers.cpp\`: texture and glTF/GLB import paths; versioned importer settings and cooked formats; model import can optimize meshes and generate LODs.
- \`engine/assets/texture_compression.hpp\` and \`texture_compression.cpp\`: existing BC and ASTC variants; device-capability-based runtime selection with an RGBA8 fallback.
- \`engine/assets/async_loader.*\`: asynchronous model/texture fetch and budgeted upload handoff; this is not yet a complete editor import queue.
- \`editor/src/assets_panel.cpp\`: current folder/type/search filtering; \`editor/src/thumbnails.cpp\`: model/sprite thumbnails and previews; \`editor/src/import_files.cpp\`: external file/drop import and glTF companion handling.
- \`docs/KNOWN_LIMITATIONS.md\` records the current gaps: no import-queue UI or dependency inspector; editor import/loading still has synchronous paths; asset streaming and residency cover only documented classes and paths.

### Existing audio pipeline

- \`engine/audio\` uses miniaudio behind VICTUS-owned \`victus::audio\` interfaces.
- Existing support includes WAV/Ogg Vorbis/MP3/FLAC, six buses (Master, Music, Sfx, Ui, Ambient, Voice), playback, looping, streaming, voice budgets/priorities, virtual voices, fades, a no-device test path and Luau-facing APIs.
- Existing scene components include audio source/listener, acoustic material transmission, and reverb zones. Geometry occlusion and underwater muffling are already present.
- Existing acoustics model controls how sound is filtered/transmitted in a location. It does **not** yet choose which forest, city, rain, interior or underwater ambience assets should be active.
- \`docs/STATUS.md\`, \`docs/KNOWN_LIMITATIONS.md\` and ADR-0010 in \`docs/TECHNOLOGY_DECISIONS.md\` are the authoritative current-state references. Reconcile discrepancies before changing code.

### Studio and platform boundaries

- Studio already has an Assets panel, import/drop support, thumbnails, a Material Editor and other specialized tools. Extend its existing workspace and design system.
- \`docs/handoff/STUDIO_UX_AND_WORKSPACE_MATURITY.md\` Phase 4 already specifies much of the creator-facing asset browser, import queue, metadata, dependency view and safe reimport UX. Treat that document as the UX contract; this handoff supplies catalogue architecture, library storage, provenance and contextual-audio engineering detail.
- Root CMake defaults \`VICTUS_BUILD_TOOLS\` off for Android/iOS builds. \`docs/PLATFORM_SUPPORT.md\` states Android currently builds the player only and has not been installed on a physical device. Do not describe mobile as having the full desktop Studio.
- Runtime assets are packaged/cooked into the game. The player must remain independent of the user-level Library and Studio.

## Architecture decision direction

Write/update a technology decision record before introducing a new data format, persistent index, cache, platform service or third-party dependency. Use VICTUS-owned interfaces and keep third-party types private.

### A. Catalogue and pack model

Create a versioned, deterministic manifest format. The precise file extension and schema location are implementation decisions; inspect existing project/config formats and avoid extension collisions before selecting one.

At minimum, a pack manifest should describe:

- schema version and minimum/maximum compatible VICTUS versions;
- stable pack ID, semantic pack version, display name, description, category and tags;
- publisher/author, canonical source URL and publication/update metadata;
- per-asset stable catalogue ID, display name, asset type, description, tags and preview/thumbnail path;
- relative payload path(s), byte size and cryptographic digest (SHA-256 or a stronger established project primitive);
- required platform capabilities and any platform-specific variants;
- import instructions/settings only from a constrained, validated data schema (never arbitrary executable hooks);
- exact asset licence identifier/text reference, copyright holder, attribution text, source URL, redistribution permissions, modification status and any restrictions;
- dependencies between packs/assets and declared conflicts;
- optional quality tiers or estimated resource metadata only where measured/known.

Use stable IDs and content digests to avoid duplicate downloads/copies and detect corruption. The existing FNV-1a hash/checksum facilities are useful for accidental-corruption detection where already used, but do not present a non-cryptographic checksum as publisher authenticity or a security signature.

Manifest parsing must be versioned, size-bounded, path-sanitized, strict about duplicate IDs and duplicate paths, and safe against path traversal, symlinks escaping the pack root, malformed metadata and decompression bombs. Unknown optional fields may be preserved only if the existing schema conventions support that safely; unknown required schema versions must fail with an actionable error.

### B. User-level storage and first-launch

- Inspect and reuse \`victus::core::fs\` user-data/config/cache directory helpers. Do not hard-code a platform path or store user-library content inside the engine install directory.
- At startup, Studio should load a small bundled catalogue index and scan the local installed-content directory. This local browse operation must not require network access.
- Ship only a deliberately small, useful, licence-cleared starter collection with the installer. Prefer metadata and previews in the core install; keep larger optional content packs opt-in.
- First launch should open directly into Studio/project launcher with the Library available. No mandatory login, network check, marketplace terms screen or blocking multi-gigabyte download.
- Provide explicit controls for pack install/remove, storage usage, refresh/rebuild catalogue, offline mode and download settings. Removal of a pack must not silently delete project copies already imported into games.
- If a pack is missing/corrupt, show its unavailable state and preserve project assets already copied from it.
- Never upload user files, project content or telemetry as part of library indexing. Remote catalogue update/download, if ever added, must be a separate opt-in feature with explicit provenance, transport security, content verification, signature policy and user-visible source information.

### C. Import into a project

The operation **Add to Project** must go through the existing AssetDatabase and importer/cooker path. It must not add a second project asset registry.

Required behavior:

1. Resolve the selected catalogue asset and its pack dependency closure.
2. Check licence and project compatibility before copying.
3. Validate all relative paths, source sizes, hashes and required dependencies.
4. Show an understandable preview of files to be added and any name collisions.
5. Stage content in a temporary project-local location; do not expose a half-imported folder.
6. Import/cook through existing APIs and the future Studio import queue.
7. Commit the staged files atomically or with a recoverable journal, then refresh/register the project AssetDatabase.
8. Preserve stable project references on later rename/move/reimport; never replace a valid cooked asset with a failed partial reimport.
9. Store provenance in project metadata so the project can still show pack/source/licence/attribution after the global pack is uninstalled.
10. Support cancel, retry, rollback and clear per-file diagnostics.

Use the project's existing \`.meta\` sidecars for asset identity/settings. Add provenance fields only through a versioned migration and backward-compatible parser. Do not put machine-specific absolute paths in committed project metadata.

### D. Studio Library experience

Integrate into the existing Assets workspace and VICTUS design tokens; do not add a competing asset-management window with separate state.

A clear navigation model should distinguish **Project**, **Library**, and **Installed Packs**. The exact tab/menu placement should follow the current workspace audit.

Library browsing should support:
- searchable catalogue and type/category filters;
- grid/list views, consistent thumbnails, detail/preview pane and keyboard navigation;
- tags/collections/favourites if implemented without creating an unrelated state store;
- platform/quality filters only where metadata is real;
- asset detail with preview, source, size, licence/attribution, compatibility, dependencies and installation status;
- Add to Project, install/remove pack, reveal local file and copy stable ID/path;
- empty, offline, unavailable, corrupt, licence-restricted and incompatible states;
- progress, cancel, retry and actionable errors;
- audio audition with stop/volume and a safe preview voice budget.

The first useful deliverable is a working, local, offline Library catalogue with a small curated starter pack and a complete **Add to Project** path. A decorative catalogue populated with mock cards is not acceptable.

## Dependency and format recommendations

These recommendations reflect a repository-aware evaluation on 2026-11-20. Verify exact current pins and licences again before any upgrade or adoption.

| Area | Direction | Reason / constraint |
|---|---|---|
| Project asset identity and database | Keep VICTUS AssetDatabase | It already owns stable UUIDs, .meta files, cooked artifacts and packaging index. Extend; do not replace. |
| Model interchange | Keep glTF 2.0 / GLB and current cgltf importer | It is an open, API-neutral runtime delivery format; GLB can bundle JSON, geometry and images. Importer hardening and current asset support are more valuable than adding formats indiscriminately. |
| Mesh optimization / LOD | Keep existing meshoptimizer integration | Already fetched/pinned and used by the model import/render path. Do not add a second mesh optimizer. |
| Texture compression | Keep current BC7/BC family and Arm ASTC paths | Already integrated with platform variant selection and fallbacks. |
| KTX2 / Basis Universal | Optional research item; do not adopt by default | Evaluate only if texture packaging, Basis transcoding, web distribution or memory/streaming evidence exposes a measurable gap that current cooked formats cannot solve. KTX-Software contains many third-party licences and its published licence page identifies an ETC decoder file under a non-open Ericsson licence; any future build must explicitly exclude that component or complete a separate legal review. |
| Audio backend | Keep miniaudio 0.11.26 and VICTUS audio API | Already integrated and cross-platform in the current build. Use its documented resource-management/streaming capabilities where they solve measured gaps; do not add another general mixer. |
| Advanced spatial audio | Steam Audio is an optional evaluation, not a prerequisite | It offers HRTF/binaural and propagation features absent from the current simple miniaudio spatializer. Its upstream repository is Apache-2.0, but the Valve/Steam Audio trademarks are not granted by that licence; retain notices, avoid implying endorsement, inspect bundled datasets/components and measure CPU/memory/build footprint before adoption. Prototype behind a VICTUS interface, measure on target devices and complete licence review before adoption. Existing custom occlusion/reverb must remain functional as a fallback. |
| Broad model import | Assimp is not an immediate dependency | It advertises many formats including FBX/OBJ, but the current first-party interchange path is glTF. Add only after a prioritized format requirement, importer feature comparison, bundled-subdependency review, security/fuzzing plan, platform build and license-notice review. |
| Asset-pipeline interoperability | OpenUSD / OpenAssetIO are future investigations | They solve larger scene interchange/production-pipeline problems, not the immediate local Library/catalogue gap. Do not introduce them into the runtime or starter project without a concrete consumer. |

### Dependency adoption gate

Before adding/upgrading any dependency:

- inspect exact upstream repository, immutable release/tag/commit and release activity;
- inspect the full licence and all bundled subcomponents, codecs, tools, generated files and data;
- confirm target compiler/platform support, mobile binary size and runtime footprint;
- inspect security advisories and fuzzing history for parsers/decoders;
- pin source URL and SHA-256 in CMake; update dependency inventory, SPDX data and CycloneDX SBOM tooling;
- record the decision and rejected alternatives in \`docs/TECHNOLOGY_DECISIONS.md\`;
- keep public VICTUS headers free of third-party types;
- include exact notices in Studio and exported games where required.

## Context-aware environmental audio

### Separate ambience selection from acoustics

Do not conflate these responsibilities:

- **Environment profile / ambience resolver:** decides which authored loops, one-shots and event layers should be active for the current environment.
- **Existing acoustic system:** applies spatial attenuation, geometry occlusion, acoustic-material transmission, reverb-zone behavior and underwater filtering to sounds already playing.

The new feature must use existing buses, AudioSource/AudioListener components, transforms, voice limits, fades and VICTUS audio APIs. It must not bypass the mixer or create a separate audio backend.

### Authorable environment context

Introduce a versioned, serializable VICTUS-owned environment-audio profile asset and/or component only after checking all current scene/environment components and the project serialization conventions. A tentative extension such as \`.vaudioenv\` is a proposal, not a fixed requirement; check collisions and naming rules first.

Context should use explicit, editor-visible properties, for example:

- biome: forest, grassland, desert, snow, coast, urban, industrial, cave, interior;
- spatial type: indoor/outdoor/underground/underwater;
- weather: clear, rain, storm, snow, wind;
- time-of-day tags where a game actually drives them;
- authored zone identity/priority and blend boundary;
- user-defined tags for game-specific settings.

Do not infer context from a folder name, a model filename or the visual appearance of the scene. Provide simple defaults and let the game/creator set or drive context explicitly. Dynamic weather/time tags must have a real source in game state; avoid a hidden global clock/weather dependency.

### Profile contents

An environment profile should reference project-local audio assets by stable project UUID (or the same safe asset-reference convention already used by VICTUS), not global catalogue file paths.

A profile can declare:
- continuously looped ambience layers and target bus/gain;
- one-shot event pools with variation, cooldown and probability controls;
- optional distance/spatial behavior and source placement mode;
- priority, blend-in/out duration, fade curves and overlap policy;
- condition tags and optional weights;
- random seed/selection policy so tests and captures can be deterministic;
- optional weather/time/state selectors that are driven by explicit game context;
- fallbacks for missing clips and disabled audio devices.

Audio selection must be deterministic for a given context/seed and stable across frame rates. Sort candidate profiles/layers by explicit priority and stable ID, not unordered-container iteration. Blend parameter changes smoothly. Do not restart loops on every update. Do not repeatedly trigger one-shots while the listener stays in a zone; define entry/exit/cooldown and retrigger rules.

### Override hierarchy and user control

Provide understandable modes:
- **Automatic** — resolve from authored environment context/profile.
- **Manual** — use explicitly selected ambience/events; automatic matching is bypassed.
- **Off** — no automatic ambience layers are generated. Explicit game-authored AudioSources can still play unless the creator separately mutes/disables them.

Define and document override precedence. Recommended default: per-source explicit settings always win; a local zone/profile override wins over scene/project defaults; project default is the fallback. A clear global Studio/game setting may disable the resolver without rewriting source assets.

Never silently replace a manually assigned clip or edit an AudioSource's authored bus, spatial settings or loop state. Automatic output should be traceable in Studio: show the winning profile, matched tags, priority, active layers and reason for selection.

### Studio audio authoring

Integrate into the existing Audio workspace:
- profile editor with context tags, priorities, layer list, loop/event configuration and blend controls;
- zone placement/volume visualization using existing scene gizmos where feasible;
- audio audition and layer preview with stop/solo/mute;
- live resolver inspector that explains why a profile/layer is active;
- explicit Automatic / Manual / Off controls;
- warnings for missing assets, duplicate priorities, no matching profile, unsupported format and excessive concurrent ambience;
- use existing bus routing, preview playback and undo/redo patterns.

Do not advertise a full visual audio graph or HRTF controls until the underlying runtime supports them.

## Runtime, platform and performance requirements

- Startup and Library browsing must remain usable offline. Core project creation, loading, editing, running and exporting must never be blocked by Library/network availability.
- Desktop Studio may index a small bundled seed catalogue and local installed packs on startup. Expensive thumbnail generation, hashing and import work should be cancellable background work with progress.
- Android currently receives the VICTUS player, not the full Studio. The player consumes packaged project assets and profile data only; it must not initialize the desktop catalogue UI or access a developer's user-library folder.
- Any future remote download must be opt-in, resumable, hash-verified, size-limited and installed atomically. Verify publisher signatures only after a documented signing/key-distribution design exists.
- Asset indexing should avoid repeatedly hashing unchanged content. Cache metadata keyed by canonical path, size, modification marker and content digest; content digests remain the correctness source before install/use.
- Measure cold start, warm start, catalogue scan time, thumbnail CPU/memory, import throughput, cooked size, cache hits, audio CPU per active layer, transition cost and memory on named hardware.
- Respect V.A.U.L.T. budgets. Future audio streaming/residency work should extend V.A.U.L.T. only after an explicit ownership/API review; do not create a second cache/eviction manager.
- Define sensible defaults for low-memory devices: small starter collection, lazy previews, limited concurrent ambience layers, audio voice budget and immediate manual/off override.

## Implementation sequence

Work in small, independently useful vertical slices. Continue honoring Roadmap Priority 0 (photoreal low-end rendering) before starting this Priority 1 creator-experience work, unless the owner or updated roadmap explicitly reprioritizes it.

### Slice 0 — Re-audit and decision record

- Re-read \`AGENTS.md\`, \`ROADMAP.md\`, \`docs/STATUS.md\`, \`docs/KNOWN_LIMITATIONS.md\`, \`docs/THIRD_PARTY_AND_LICENSES.md\`, \`docs/TECHNOLOGY_DECISIONS.md\`, \`docs/EDITOR_SPEC.md\`, Studio UX handoff and all current asset/audio/editor sources.
- Inspect latest main and CI. Map existing APIs, duplicate systems, current metadata/schema migration support, user-data path APIs, import error handling and asset reference consumers.
- Write an ADR (or extend ADR-0008/ADR-0010 with cross-links) for catalogue separation, storage, manifest integrity, project provenance and audio context resolution.
- Produce a short source-backed audit/decision table before code. Do not change runtime code during the audit.

### Slice 1 — local catalogue core

- Implement a VICTUS-owned, versioned catalogue manifest/parser and local index service in the correct module/layer, selected after architecture dependency review.
- Add path safety, schema validation, digest verification, deterministic ordering, duplicate/collision diagnostics and migration tests.
- Use only a small in-repo fixture pack of assets with verified redistribution rights. No remote downloads in this slice.
- Unit tests cover valid/invalid manifests, unknown schema versions, duplicate IDs/paths, traversal, symlink escape, hash mismatch, missing previews, oversized manifests/payloads and offline startup.

### Slice 2 — starter pack and Add to Project

- Create a tiny curated starter pack with asset-by-asset provenance and licence records.
- Add transactional import/copy through AssetDatabase; project files receive stable IDs and are self-contained.
- Test same pack added twice, name collisions, dependency closure, cancellation, interrupted copy, corrupt file, failed cooking, re-add after global pack removal, project rename/move and clean export.

### Slice 3 — Studio Library integration

- Extend existing Assets workspace to show Project / Library / Installed Packs.
- Reuse thumbnail cache, search/filter, design tokens, commands, undo where applicable, diagnostics and task/progress infrastructure.
- Add meaningful empty/offline/error/loading states and accessible keyboard workflows.
- Make browse → preview → licence review → Add to Project → assign/use in scene work end-to-end.
- Keep Studio's existing local import path working; no regression to direct file drag/drop.

### Slice 4 — production-grade import/dependency workflow

- Implement or complete background import queue with bounded concurrency, progress, cancel, retry and per-asset errors.
- Add dependency/reference indexing and impact preview before destructive rename/move/delete.
- Reimport must preserve the last known-good cooked output if decoding/cooking fails.
- Add recovery/repair for missing moved source files and duplicate content detection without conflating identical bytes with identical semantic assets.
- Update the existing Studio UX handoff/audit and user guide with actual shipped behavior.

### Slice 5 — environment-audio data model and resolver

- Audit audio component serialization, scene update hooks, environment state, event scheduling and existing ReverbZone code.
- Decide whether profiles are assets, components, or a combination. Prefer reusable profile assets referenced by scene zones.
- Implement a deterministic context resolver with explicit context input, priority, stable tie-breaking, smoothing, loop continuity, one-shot cooldowns and manual/off modes.
- Keep the current acoustic system, buses and miniaudio path intact.
- Unit tests must run with VICTUS_NO_AUDIO/no-device path and verify mix output where practical.

### Slice 6 — Studio environmental audio authoring

- Integrate the profile editor, zone gizmos, audio audition, active-layer inspection, override controls and actionable validation into the existing Audio workspace.
- Demonstrate a real sample scene transitioning between at least two explicitly tagged environments without restarting the game or producing repeated one-shot spam.
- Verify manual override and Off work immediately and persist correctly.

### Slice 7 — optimization and optional technology evaluation

- Measure Library cold/warm startup, catalogue scan, preview generation, import throughput, cache behavior, and runtime memory.
- Measure audio transition CPU/memory, number of active ambience layers, streaming behavior, device suspend/resume and no-device fallback.
- Only then consider KTX2/Basis, Assimp or Steam Audio. Each candidate needs a separate evidence-backed ADR, exact licence review, per-platform build validation and a measurable acceptance threshold.
- Do not bundle optional dependencies merely because they are popular.

## Required acceptance tests

### Library and assets

1. Studio opens with Library navigation available in airplane/offline mode and without blocking first-run.
2. A clean install exposes only the small bundled starter catalogue; optional packs are not silently downloaded.
3. Catalogue manifests validate deterministically and reject malformed, unsafe, incompatible or hash-mismatched content with actionable errors.
4. Add to Project copies/imports the selected asset and dependency closure, creates valid project metadata, preserves attribution/provenance, and does not create a runtime dependency on the global Library.
5. A project containing Library assets opens, runs and exports on a clean machine after the Library pack is removed.
6. Re-adding the same pack is idempotent or presents an explicit collision resolution; it never silently overwrites different user content.
7. Failed/cancelled imports leave no partial visible asset and preserve prior valid cooked outputs.
8. Existing drag/drop, glTF companion import, texture/model import, thumbnails, Material Editor, audio preview and project export continue to work.
9. Dependency impact is shown before destructive actions; UUID references survive supported moves/renames.
10. Every bundled asset has a recorded, individually verified licence and attribution record.

### Contextual audio

1. A scene with no environment profile behaves exactly as it does before the feature.
2. Entering an authored forest zone activates the expected loop layers and transitions smoothly.
3. Moving from forest to indoor/cave/outdoor zones changes layers without clicks, unbounded overlap or repeated loop restarts.
4. One-shot triggers obey entry/exit, probability, deterministic seed and cooldown rules.
5. Higher-priority zone/profile selection and stable tie-breaks are deterministic.
6. Manual mode prevents automatic selection; Off stops resolver-generated ambience without disabling explicitly authored sources; returning to Automatic fades in the correct profile.
7. Missing profile clips produce warnings and fallbacks rather than crashing or stopping the audio engine.
8. Existing acoustic material occlusion, ReverbZone, underwater filtering, bus levels, voice budgets and source settings remain correct.
9. Tests pass in no-device mode; platform output is marked unverified until heard/measured on real devices.
10. Performance is measured with many zones, profiles, active sources and transitions; no per-frame unbounded allocation or file I/O on the audio callback.

### Release and trust

- Asset parser fuzz/negative tests and archive/path-bounds tests are included before accepting untrusted downloaded packs.
- CI validates desktop builds and existing Android player compilation; physical platform claims remain limited by \`docs/PLATFORM_SUPPORT.md\`.
- Licence inventory, notices and SBOM are updated for every new library and bundled asset.
- User data, installed packs and project assets are never silently deleted by uninstall or cleanup.
- Documentation explains offline behavior, storage location, licence details, manual/automatic audio controls and actual platform support.

## Completion criteria

The feature is complete only when a new user can open Studio offline, browse a real licence-cleared starter library, preview and add content into a project, edit/use that content, and export a self-contained game. A creator can author environment profiles and zones, see why ambience is active, override or disable it, and rely on deterministic, tested transitions.

A manifest parser, mock cards, a standalone demo, an audio profile file that does not reach the runtime, or a panel with inactive controls is not completion.

## Research references

Primary upstream references checked on 2026-11-20:

- Khronos glTF registry and current glTF 2.0 specification: https://registry.khronos.org/glTF/ and https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- meshoptimizer project and MIT licence: https://github.com/zeux/meshoptimizer and https://github.com/zeux/meshoptimizer/blob/master/LICENSE.md
- miniaudio manual, resource manager and streaming: https://miniaud.io/docs/manual/ and https://miniaud.io/docs/manual/index.html
- miniaudio spatializer limitations: its documented spatialization is simple; HRTF and environmental occlusion are not currently supported by the built-in spatializer: https://miniaud.io/docs/manual/index.html
- KTX-Software licence details, including repository-wide third-party licences and the non-open Ericsson-licensed ETC decoder: https://github.khronos.org/KTX-Software/license.html
- Steam Audio product and C API documentation for HRTF/binaural processing and platform support: https://partner.steamgames.com/doc/features/steam_audio and https://valvesoftware.github.io/steam-audio/doc/capi/
- Assimp formats and licence: https://github.com/assimp/assimp and https://github.com/assimp/assimp/blob/master/LICENSE

Re-check these references and exact release/licence files before implementation; upstream main-branch pages are research references, not immutable dependency pins.
