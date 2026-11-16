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
