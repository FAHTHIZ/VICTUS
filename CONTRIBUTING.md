# Contributing to VICTUS

Read AGENTS.md, ROADMAP.md and relevant architecture/decision docs first. Inspect main and existing code before changing anything. Prefer focused vertical slices.

Expectations: C++20; RAII/explicit ownership; no hidden global mutable state; diagnostics and error handling; tests; platform/fallback notes; reproducible performance methodology. Do not commit build output, credentials, private data or unlicensed assets.

Build/test:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
Run Release before performance-sensitive changes.

Use focused Conventional Commits, e.g. `feat(core): add structured diagnostics`, `test(sim): cover population LOD invariants`, `docs(render): record backend decision`. Keep each commit coherent and buildable. Owner currently requests direct commits to main; preserve unless changed.

Follow docs/THIRD_PARTY_AND_LICENSES.md for every dependency/asset. Never copy proprietary engine code or assets.
