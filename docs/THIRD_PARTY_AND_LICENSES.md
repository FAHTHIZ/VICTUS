# Third-Party Software, Assets and Licensing

Mandatory for code, libraries, SDKs, tools, datasets, fonts, models, textures, audio and documentation.

1. Verify exact upstream repository, release/tag/commit and LICENSE.
2. Review bundled dependencies, not just top-level license.
3. Record SPDX identifier, copyright, URL, version, modifications and notices.
4. Check patent, attribution, redistribution, trademark, export and platform terms.
5. Publicly available does not mean freely redistributable.
6. Never copy proprietary engine source, leaked code, private assets or unlicensed sample content.
7. Maintain SPDX/CycloneDX SBOM as dependencies grow.
8. Preserve upstream notices and modification records.
9. Re-audit upgrades/releases.
10. Escalate ambiguous/copyleft/custom terms for owner/legal review.

## Candidate list — not adopted
| Component | Candidate use | License expectation (verify exact version) |
|---|---|---|
| SDL3 | Platform/window/input | zlib |
| Flecs / EnTT | ECS | MIT |
| Taskflow | Task graphs | MIT |
| enkiTS | Scheduler | zlib |
| Jolt | Physics | MIT |
| Recast/Detour | Navigation | zlib |
| ozz-animation | Animation | MIT |
| bgfx/bx/bimg | Graphics abstraction | BSD-2 family |
| Diligent | Graphics abstraction | Apache-2.0 |
| Filament | Renderer/reference | Apache-2.0 |
| meshoptimizer | Geometry | MIT |
| glTF loaders | Import | Verify exact release |
| KTX/Basis | Texture/transcoding | Verify release and codecs |
| miniaudio | Audio | MIT-0/public-domain option |
| Dear ImGui/ImGuizmo | Editor/gizmos | MIT |
| Tracy | Profiling | BSD-3-Clause |
| Catch2/doctest | Tests | BSL-1.0/MIT |
| Blender | External authoring | GPL-3.0-or-later; keep external |
| OpenUSD | Scene interchange | Apache-2.0; inspect dependencies |

This is not a legal determination. Upstream license files and bundled notices are authoritative. Original VICTUS code uses Apache-2.0; third-party files retain their own licenses.
