## What changed?

<!-- Describe the user-facing or developer-facing change. Link a related issue if one exists. -->

## Why?

<!-- What problem does this solve? Link the ROADMAP.md item or ADR. -->

## Verification

- [ ] `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` passes with `-Werror`.
- [ ] GPU changes: `victus_gpu_tests` pass on Vulkan (lavapipe) and on GLES (`VICTUS_GPU_DRIVER=gles`).
- [ ] Shader changes: `python3 tools/shaders/compile_shaders.py --check` is clean.
- [ ] Dependency changes: `python3 tools/deps/victus_deps.py check` is clean and docs/THIRD_PARTY_AND_LICENSES.md is updated.
- [ ] Added or updated focused tests, including error and fallback paths.
- [ ] docs/STATUS.md, ROADMAP.md and CHANGELOG.md updated where behaviour changed; an ADR for architectural changes.
- [ ] README counts changed: `python3 scripts/readme_assets.py` re-run.

## Risk and compatibility

- [ ] No change to file formats, network protocol, script API or plugin ABI.
- [ ] Any such change is versioned, described below and covered by tests.

**Notes:**
