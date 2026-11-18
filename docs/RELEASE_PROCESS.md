# Release process

How VICTUS versions are numbered, what a release contains, and the exact steps to cut one. Tooling: `tools/release/release.py` (tested by `ctest -R victus_release_tools`) and `.github/workflows/release.yml`. Related: [CHANGELOG.md](../CHANGELOG.md), [SECURITY.md](../SECURITY.md), [DIAGNOSTICS.md](DIAGNOSTICS.md).

## Versions and compatibility

- Versions follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). The single source is `project(VICTUS VERSION x.y.z)` in CMakeLists.txt; `victus version`, crash reports and diagnostics bundles print it with the git revision.
- Tags are `vX.Y.Z` (pre-releases `vX.Y.Z-rc.N`, published as GitHub pre-releases). Every tag is cut from `main`.
- Before 1.0.0: a minor version (0.3.0) may change public C++ headers and the Lua API; a patch version (0.2.1) only fixes bugs and never changes file formats incompatibly.
- File formats (`victus.project`, `.vscene`, `.vmat`, `.vpak`, save games, network protocol) carry their own version numbers. A release must open everything the previous release wrote (migrations on load, tested); anything that cannot be migrated is listed under "Changed" with the manual step. Exported games embed the player they were exported with, so shipped games never change underneath their players.
- A versioned plugin/module SDK does not exist yet (roadmap 6b, Workstream D: plugin SDK ADR); until it does, no binary compatibility is promised for C++ extensions.

## Changelog rules

- Every user-visible change adds a line under `## [Unreleased]` in CHANGELOG.md in the same commit, in one of Added, Changed, Deprecated, Removed, Fixed, Security ([Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/)).
- Write for game developers, not engine developers: what they can do now, what they must change. Do not claim more than docs/STATUS.md verifies; name unverified platforms.
- At release time the Unreleased section becomes `## [X.Y.Z] - YYYY-MM-DD` and a new empty Unreleased section goes on top. `release.py check` refuses tags whose section is missing, undated or empty; `release.py notes X.Y.Z` prints the section, which becomes the GitHub release text.

## What a release contains

Per platform (linux-x86_64, windows-x86_64, macos-arm64), built as RelWithDebInfo (optimised, `NDEBUG`, with debug information) after the full test suite passes on that runner:

| Archive | Contents |
|---|---|
| `victus-X.Y.Z-<platform>.tar.gz` (Windows: `.zip`) | `bin/victus`, `bin/victus_editor`, `bin/victus_player` without debug information; `samples/arena`, `samples/dungeon_crawler`; README, LICENSE, THIRD_PARTY_NOTICES.txt, CHANGELOG, SECURITY, RELEASE_NOTES.md; `docs/` (getting started, user guide, diagnostics, known limitations, platform support) |
| `victus-X.Y.Z-<platform>-symbols.tar.gz` (Windows: `.zip`) | Linux: `.build-id/aa/rest.debug` (objcopy `--only-keep-debug`; the stripped binaries carry a `.gnu_debuglink`); macOS: `<exe>.dSYM`; Windows: `<exe>.pdb` |

Plus `victus-X.Y.Z-sbom.cdx.json` (CycloneDX SBOM from `tools/deps/victus_deps.py sbom`) and `SHA256SUMS.txt` covering every file.

Symbol archives are what turn crash-report frames into function names (`tools/crash/symbolize.py crash.txt --symbols <unpacked symbols folder>`; it matches GNU build-ids, so the wrong build's symbols are refused). Never delete the symbol archive of a release that is still in use; GitHub release assets are the archive of record.

## Cutting a release

1. On an up-to-date `main` with a green VICTUS CI run: choose the version (see Versions above).
2. Set `project(VICTUS VERSION X.Y.Z ...)` in CMakeLists.txt; move the Unreleased entries in CHANGELOG.md under `## [X.Y.Z] - <today>`, add a new empty `## [Unreleased]`, update the link references at the bottom.
3. Check locally: `python3 tools/release/release.py check --tag vX.Y.Z`, `python3 tools/deps/victus_deps.py check`, and read docs/KNOWN_LIMITATIONS.md for anything the notes must mention.
4. Commit (`chore(release): VICTUS X.Y.Z`), push to `main`, wait for CI.
5. Tag and push: `git tag -a vX.Y.Z -m "VICTUS X.Y.Z" && git push origin vX.Y.Z`.
6. The release workflow checks the tag again, runs shader/dependency/advisory gates, builds and tests on Linux, Windows and macOS, packages each platform, smoke-runs the packaged CLI (`version`, `new`, `check`), and creates a draft GitHub release with all archives, the SBOM and checksums.
7. The owner downloads at least one archive per platform, verifies `sha256sum -c SHA256SUMS.txt`, starts the editor from the archive, and publishes the draft.
8. Afterwards: record the release in docs/STATUS.md (what was verified, on which machines).

Dry run: Actions → "VICTUS release" → Run workflow with a tag name builds and packages without creating a release (an unreleased version only warns in the check job); the archives are attached to the run as artifacts.

## Not done by the pipeline

- Code signing and notarization: Windows Authenticode and macOS Developer ID signing + notarization need certificates and Apple credentials only the owner can provide. Until then, Windows SmartScreen and macOS Gatekeeper warn on first launch (macOS: right-click → Open, or `xattr -d com.apple.quarantine`).
- Installers (MSI, DMG, .deb/AppImage) and auto-update; archives only.
- Android and other platforms: the Android APK is built by CI per commit but is not a release asset.
- Universal macOS binaries (arm64 only; Intel Macs are not built).

## Security response

[SECURITY.md](../SECURITY.md) describes private reporting. Process for maintainers:

1. Acknowledge a private report within 3 working days; reproduce it on `main` and the latest release.
2. Fix privately (a GitHub security advisory draft keeps the discussion private), add a regression test, and prepare a patch release (X.Y.Z+1) from `main`.
3. Release, then publish the advisory with affected versions, the fix version, credit (if the reporter wants it) and a CHANGELOG "Security" entry.
4. Only the latest release receives security fixes before 1.0.0.

## Dependency updates

- Pins live in cmake/VictusDependencies.cmake (URL + SHA-256 for every fetched dependency); licences and reviewed advisories in tools/deps/dependencies.json; versions in docs/THIRD_PARTY_AND_LICENSES.md.
- VICTUS CI runs `victus_deps.py check` and `osv` on every push and weekly, so a new advisory against an unchanged pin fails the weekly run.
- To update a dependency: change the URL and SHA-256, update THIRD_PARTY_AND_LICENSES.md and docs/RESEARCH_REGISTER.md if behaviour changed, run the full test suite (and the sanitizer jobs in CI), add a CHANGELOG line ("Changed" or "Security"). A security update of a dependency is handled like a VICTUS vulnerability above.
- An advisory that cannot be fixed upstream goes into `osv_ignore` with the mitigation VICTUS applies, and into KNOWN_LIMITATIONS.md.
