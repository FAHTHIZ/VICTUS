# VICTUS Production Pipeline Specification

## Goal
Make the path from source project to tested, distributable game reproducible, diagnosable and repeatable.

## Project and asset database
- Stable asset IDs independent of file paths.
- Versioned project, scene, asset and save schemas with migration tools.
- Source-to-cooked dependency graph and explicit import settings.
- Validation for malformed files, missing references, unsupported formats and unsafe paths.
- Asset provenance, license metadata and source attribution fields where relevant.

## Import and cooking
- Background import with progress, cancellation, logs and recoverable errors.
- Incremental cooking and content-addressed cache with correct invalidation.
- Platform-specific texture, mesh, shader and audio variants.
- Deterministic cooking where feasible; record tool versions and settings.
- Cook reports for size, duplicate content, dependencies, warnings and unsupported features.

## Build and package
- Build profiles per platform/configuration and reproducible command-line builds.
- Incremental compilation, build graph caching and clear failure diagnostics.
- Asset validation before package creation.
- Runtime-only packages exclude editor-only code and authoring metadata.
- Package manifests, version information, symbols, notices and SBOM.
- Signing and store-specific packaging only through approved toolchains and credentials.

## Hot reload
Support safe reload for assets and shaders first. Code hot reload requires explicit ABI/state migration rules and must never corrupt editor or runtime state. When safe reload is impossible, provide a clear restart/rebuild path.

## Testing and release
- Unit, integration, editor workflow, gameplay smoke, save migration and package tests.
- Automated reference-project builds from clean checkouts.
- Crash diagnostics with symbol support and privacy controls.
- Performance regression comparisons on named hardware.
- Release notes, compatibility matrix, known issues, dependency updates and rollback plan.

## Collaboration and source control
Provide source-control status and diffs, stable text-based project metadata, merge-aware asset/project serialization, conflict diagnostics and safe locking for binary assets when necessary. Multi-user live editing is a later capability; do not promise it until consistency and recovery are proven.

## Modding and plugins
Define plugin lifecycle, versioned API, permissions, capability grants, isolation, package integrity, signing/trust model and failure handling. Treat downloaded mods and project files as untrusted. Never load arbitrary native code from untrusted packages by default.

## Acceptance
A clean machine can clone a reference project, validate/import/cook assets, build the runtime, run automated tests, produce a distributable package and inspect diagnostics using documented commands.

## Current state (2026-11-22)

Checked against `engine/assets`, `engine/player` (export, validate, migrate), `.github/workflows` and the ADRs. Limits are in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

| Spec area | On main | Missing |
|---|---|---|
| Project and asset database | Stable UUIDs in `.meta` files (ADR-0008, scene format), versioned formats with per-component migrators and `victus migrate` (ADR-0100), reference-safe rename, move and delete (ADR-0175, ADR-0178), and durable writes with two verified generations (ADR-0082). Asset Store installs record licence and provenance and write CREDITS.md into exports (ADR-0174). | Licence and provenance fields for assets that are not installed from the store. |
| Import and cooking | Background import queue with per-file results, cooperative cancellation and retry (ADR-0189), a cooked cache plus a machine-wide shared cache (ADR-0180), BC and ASTC variants (ADR-0047), per-platform texture profiles (ADR-0149), and meshoptimizer LODs (ADR-0049). | Cook reports on duplicates and unsupported features. The export report gives only counts and payload bytes. Audio has no per-platform variants. |
| Build and package | CMake presets; `victus export` validates scenes and scripts, re-imports stale assets and writes a byte-reproducible `.vpak` v2 with zstd, BLAKE2b digests and optional Ed25519 signing (ADR-0154), plus notices and a manifest. Release CI packages Linux, Windows and macOS with separate symbols, SHA256SUMS and an SBOM. | Cross-platform export (exports are host-only), installers, OS code signing, store packaging and patching. |
| Hot reload | Assets and shaders (ADR-0022), and Luau modules with a `reload()` hook on script instances. | Native plugin code reload. |
| Testing and release | Unit, editor-workflow, sample-game, export, determinism, perceptual-reference and performance-budget suites in CTest. Crash reports and diagnostics bundles (ADR-0072). | Performance comparison on named physical hardware (CI is lavapipe). No release tag exists yet, so the release process has never run end to end. |
| Collaboration and source control | Scenes, prefabs, materials and the project file are text JSON. | No VCS status, diff or merge tooling in the Studio, no binary-asset locking and no multi-user editing. |
| Modding and plugins | Versioned native C ABI with manifests, dependency order and per-plugin failure isolation (ADR-0078). Editor plugins with undoable commands (ADR-0192). | No sandbox, permissions or capability grants, and no signing or trust model for plugins. Native plugins are trusted code. |

Acceptance: CI covers a clean checkout through configure, build, tests and (in `release.yml`) packaging for three desktop platforms. "Clone a reference project and produce a distributable package" is exercised by the sample-game and export tests, not by a separate reference-project repository.
