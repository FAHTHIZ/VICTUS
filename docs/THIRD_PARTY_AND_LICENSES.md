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

## Adopted
| Component | Version / pin | License (verified from upstream LICENSE) | Use | Distribution |
|---|---|---|---|---|
| SDL3 | 3.4.20, release tarball SHA-256 `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f` | Zlib | Platform, input, SDL_GPU RHI backend | Statically linked; notice must ship with binaries |
| doctest | 2.5.3 (vendored `third_party/doctest`) | MIT | Unit tests only | Not shipped in games |
| stb (stb_image, stb_image_write, stb_truetype) | commit `9600522de53973336fa033a289858a4354cbb04c` (vendored `third_party/stb`) | MIT or public domain (dual, `third_party/stb/LICENSE`) | Image decode/encode, font rasterisation | Linked into engine |
| cgltf | v1.15 (vendored `third_party/cgltf/cgltf.h`, SHA-256 `e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec`) | MIT (`third_party/cgltf/LICENSE`, Copyright (c) 2018-2021 Johannes Kuhlmann) | glTF 2.0 / GLB parsing in the asset importer | Linked into editor/cooker; notice must ship |
| Dear ImGui | v1.93.0-docking, commit `b916186e66aa3c02b72c6ca17455516512ed8049` (vendored `third_party/imgui`, unmodified; core + `backends/imgui_impl_sdl3` + `misc/cpp/imgui_stdlib`) | MIT (`third_party/imgui/LICENSE.txt`, Copyright (c) 2014-2026 Omar Cornut); embedded ProggyClean and ProggyForever fonts MIT (Tristan Grimmer; Disco Hello) | Editor UI (`engine/ui`, `victus_editor`) | Linked into editor and, via `engine/ui`, available to tools; notice must ship with the editor |
| Luau | 0.747, release tarball `https://github.com/luau-lang/luau/archive/refs/tags/0.747.tar.gz` SHA-256 `2c72471f91c42af7f2c07cc4cc602b27c215346d491d7ab3d80da63050ad43f2`, fetched by CMake (`victus_fetch_luau`); only VM, Compiler, Ast, Common, Bytecode built; unmodified | MIT (`LICENSE.txt`: Copyright (c) 2019-2026 Roblox Corporation; Copyright (c) 1994–2019 Lua.org, PUC-Rio; plus `lua_LICENSE.txt`) | Gameplay scripting (`engine/script`) | Statically linked into player, editor and CLI; both notices must ship |
| Jolt Physics | v5.7.0, release tarball `https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.7.0.tar.gz` SHA-256 `9a0a405f2375dc4bd9f72486c56e1cbc7794c6a62e42fc48fadea45dfc93dacc`, fetched by CMake (`victus_fetch_jolt`); library only (samples, viewer, tests off); unmodified | MIT (`LICENSE`: Copyright 2021 Jorrit Rouwe) | 3D physics (`engine/physics`) | Statically linked into player, editor and CLI; notice must ship |
| Box2D | v3.2.0, release tarball `https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz` SHA-256 `ffec747b27a2267016f8d0240d658c82d187fb2271fa0f69fcbe53fbc067180d`, fetched by CMake (`victus_fetch_box2d`); library only (samples, benchmarks, tests off); unmodified | MIT (`LICENSE`: Copyright (c) 2022 Erin Catto) | 2D physics (`engine/physics`) | Statically linked into player, editor and CLI; notice must ship |
| glslang (glslangValidator) | build-time tool only | BSD-3-Clause / mixed (tool, not redistributed) | GLSL → SPIR-V for built-in shaders | Not shipped; generated SPIR-V is VICTUS output |
| SPIRV-Cross | build-time tool only | Apache-2.0 (tool, not redistributed) | SPIR-V → MSL and reflection | Not shipped |
| Mesa lavapipe | CI/test environment only | MIT | Software Vulkan for headless GPU tests | Not shipped |

## Candidate list — not adopted
| Component | Candidate use | License expectation (verify exact version) |
|---|---|---|
| Flecs / EnTT | ECS | MIT |
| Taskflow | Task graphs | MIT |
| enkiTS | Scheduler | zlib |
| Jolt | 3D physics (v5.7.0, 2026-11-14) | MIT |
| Box2D | 2D physics (v3.2.0, 2026-10-19) | MIT |
| Recast/Detour | Navigation | zlib |
| ozz-animation | Animation | MIT |
| bgfx/bx/bimg | Graphics abstraction | BSD-2 family |
| Diligent | Graphics abstraction | Apache-2.0 |
| Filament | Renderer/reference | Apache-2.0 |
| meshoptimizer | Geometry | MIT |
| glTF loaders | Import | Verify exact release |
| KTX/Basis | Texture/transcoding | Verify release and codecs |
| miniaudio | Audio | MIT-0/public-domain option |
| ImGuizmo | Editor gizmos | MIT |
| Tracy | Profiling | BSD-3-Clause |
| Blender | External authoring | GPL-3.0-or-later; keep external |
| OpenUSD | Scene interchange | Apache-2.0; inspect dependencies |

This is not a legal determination. Upstream license files and bundled notices are authoritative. Original VICTUS code uses Apache-2.0; third-party files retain their own licenses.
