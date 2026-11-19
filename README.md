<div align="center">

# VICTUS

**Make a whole game, 2D or 3D, without writing code. You can still drop down to scripts and C++ when you need them.**

[![VICTUS CI](https://github.com/FAHTHIZ/VICTUS/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/FAHTHIZ/VICTUS/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS%20%7C%20Android-555.svg)

[Getting started](docs/GETTING_STARTED.md) · [User guide](docs/USER_GUIDE.md) · [Samples](#samples) · [Status](#status) · [Roadmap](ROADMAP.md)

<img src="docs/images/studio.jpg" alt="VICTUS Studio editing the No-Code Platformer: hierarchy, viewport, inspector with Platformer Movement, and the Getting Started guide" width="100%">

</div>

VICTUS is an independent, open engine for complete games, real-time rendering and simulation. It comes with **VICTUS Studio**, an editor built so a first-time creator can finish a game. Ready-made **Behaviours** cover movement, enemies, pickups, health, checkpoints and score. Behind them is a full engine: physically based rendering, Jolt and Box2D physics, navigation, AI, animation, audio, multiplayer, Luau scripting, visual scripting and one-command export.

## Your first game in five minutes

No programming. Every step happens in VICTUS Studio.

1. **Create a project.** Open `victus_editor`, choose **New Project**, pick **No-Code Platformer** and press **Create**. From a terminal: `victus new MyGame --template nocode`.
2. **Press Play.** The level is already a game: run with A/D, jump with Space (press it again in the air to double jump), stomp slimes, collect coins and reach the flag.
3. **Change how it feels.** Stop, select **Player**, and edit **Platformer Movement** in the Inspector. Raise Jump Speed or set Max Jumps to 3. Every field has a tooltip.
4. **Add something new.** Select any object, choose **Add Component > Behaviours** and pick one, such as **Patrol**, **Chase**, **Hazard**, **Collectible** or **Spawner**. It works as soon as you press Play.
5. **Ship it.** **File > Export Game** writes a standalone build you can send to friends.

In the Inspector, **Make it a...** turns any selected object into a Player, Enemy, Coin, Hazard, Checkpoint or Goal with one click. The **Getting Started** panel walks you through these steps and ticks each one off. It can also do a step for you, and its lessons build a whole first 2D or 3D game with you in seven steps.

<table>
<tr>
<td width="50%"><img src="docs/images/nocode_platformer.png" alt="The No-Code Platformer template running: player, slime, heart, coins, spikes and a HUD"><br><sub>No-Code Platformer: built only from Behaviours, with no scripts</sub></td>
<td width="50%"><img src="docs/images/nocode_3d.jpg" alt="The No-Code 3D Adventure running: a robot on grassy islands over glowing lava, with coins, a guard robot and a HUD"><br><sub>No-Code 3D Adventure: islands, lava, a guard robot and a turret, still with no scripts</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/images/template_3d.jpg" alt="The 3D template: a robot character, a fire, physics crates and a controls panel"><br><sub>3D template: character controller, physics, particles, prefabs and save games</sub></td>
<td width="50%"></td>
</tr>
</table>

## Behaviours

Behaviours are ready-made gameplay components, similar to GDevelop behaviors and Construct behaviors. Add one to an entity and it starts acting that way.

| Make it move | Make it a threat | Make it reward | Run the game |
|---|---|---|---|
| Platformer Movement | Hazard (damage, knockback, stomp) | Collectible (score, health, lives) | Game Rules (score, lives, timer, HUD) |
| Top-Down Movement | Patrol | Checkpoint | Goal |
| Camera Follow | Chase | Health | Spawner |
| Projectile | Shooter | Lifetime | Events for scripts and graphs |

Every behaviour sends events (`collected`, `died`, `game_won` and others), so scripts and visual graphs can react to them later without replacing them. Reference: [Make a game without code](docs/USER_GUIDE.md).

## What is inside

| Area | What you get |
|---|---|
| **Studio** | Start screen with templates and samples, docked layouts, gizmos, undo and redo, searchable Add Component, asset browser with drag-and-drop import, play-in-editor, Problems panel, command palette, Getting Started guide, one-click export |
| **Rendering** | SDL_GPU (Vulkan, Metal, D3D12) render graph, clustered forward PBR, cascaded shadows, SSAO, IBL and sky, reflection probes, TAA and temporal upscaling, bloom, auto exposure, volumetric fog and clouds, water, decals, terrain and foliage, GPU particles, LODs, occlusion culling, quality tiers |
| **2D** | Sprite batching, sprite animation, tilemaps with autotiling, 2D lights, Box2D physics, one-way platforms |
| **Gameplay** | Archetype ECS, prefabs, scenes with stable IDs, input actions with gamepad support, character controllers, Jolt physics and joints, Recast/Detour navigation, behaviour-tree AI with perception, save games, localisation, in-game UI |
| **Animation and audio** | Skeletal animation with GPU skinning, animation controllers, timeline sequencer, miniaudio with spatial sound |
| **Code, when you want it** | Luau scripting with sandboxing and hot reload, node-based visual scripting, a C++ API and plugins |
| **Multiplayer** | Server-authoritative sessions over ENet with encryption by default, listen servers and dedicated `victus serve` |
| **Shipping** | `victus export` to `.vpak` packs and a standalone player, Android packaging, crash reports and diagnostics bundles, `victus check` validation |

## Templates

| Template | Command | You get |
|---|---|---|
| No-Code Platformer | `victus new MyGame --template nocode` | A complete 2D level made only from Behaviours |
| No-Code 3D Adventure | `victus new MyGame --template nocode3d` | A 3D island level with an animated character, made only from Behaviours |
| 2D platformer | `victus new MyGame --template 2d` | Box2D platformer in Luau: one-way ledge, swinging lantern, coins, music |
| 3D | `victus new MyGame --template 3d` | Third-person robot, physics crates, fire, prefabs, save and load |
| Simulation | `victus new MyGame --template simulation` | 576 entities animated by native systems, with a scripted orbiting camera |
| Multiplayer | `victus new MyGame --template multiplayer` | Networked coin arena: host with `victus serve`, join with `victus run --connect` |

## Samples

Each sample is a finished game made with engine features alone. Open it in Studio or run `victus run samples/<name>`. CTest plays every sample to the end headlessly (`ctest -L samples`).

<table>
<tr>
<td width="50%"><img src="docs/images/sample_arena.jpg" alt="Robot Arena: a third-person arena with pillars and a wave HUD"><br><sub><a href="samples/arena/README.md">Robot Arena</a>: five waves of robots that chase you over the navmesh</sub></td>
<td width="50%"><img src="docs/images/sample_dungeon.png" alt="Dungeon crawler: a pixel-art hero and slime in a stone maze"><br><sub><a href="samples/dungeon_crawler/README.md">Dungeon Crawler</a>: generated tilemap floors, gems and slimes</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/images/render_showcase.jpg" alt="Render showcase: PBR spheres, lit cubes and cylinders on a checker floor"><br><sub>Render Showcase: the 3D material and lighting feature set</sub></td>
<td width="50%"><sub><a href="samples/platformer/README.md">Ruin Runner</a>: a three-level 2D platformer with one-way planks, spikes, stompable beetles, checkpoints and lives</sub></td>
</tr>
</table>

## Quick start

~~~bash
tools/setup/install_deps.sh          # Linux/macOS toolchain (Windows: see the guide)
python3 tools/setup/check_env.py     # verify the toolchain
cmake --preset dev && cmake --build --preset dev && ctest --preset dev

B=build/dev/bin
$B/victus new MyGame --template nocode   # or nocode3d, 3d, 2d, simulation, multiplayer
$B/victus_editor MyGame                  # open it in VICTUS Studio
$B/victus run MyGame                     # or play it directly
$B/victus export MyGame --out MyGame-export && ./MyGame-export/MyGame
~~~

CMake fetches dependencies from pinned, hash-verified archives. You do not need an account or a sign-in. Presets, Windows notes and troubleshooting are in [Getting started](docs/GETTING_STARTED.md).

Template controls: 2D, A/D or arrows to run, Space to jump; 3D, WASD to walk, Space to jump; multiplayer, WASD and Space, with coins scored by the server. A gamepad works too. In Studio, click the viewport after pressing Play so the game receives the keyboard.

## Status

VICTUS is under active development and is **not yet production-ready**. No game has shipped with it yet. Rendering has been validated on software Vulkan and CI's virtual Apple GPU, and it has not been run on a phone. Each feature listed above has automated tests, and the evidence is public:

- [Known limitations](docs/KNOWN_LIMITATIONS.md): the maturity of each subsystem and what is still missing
- [Platform support](docs/PLATFORM_SUPPORT.md): what has been executed on each platform
- [Engineering status](docs/STATUS.md): every slice, with the commands that verified it
- [Roadmap](ROADMAP.md) and [Changelog](CHANGELOG.md)

## Documentation

**Using VICTUS:** [Getting started](docs/GETTING_STARTED.md) · [User guide](docs/USER_GUIDE.md) · [Gameplay AI](docs/AI.md) · [Animation](docs/ANIMATION.md) · [Sequencer](docs/SEQUENCER.md) · [Characters](docs/CHARACTERS.md) · [Plugins](docs/PLUGINS.md) · [Android](docs/ANDROID.md) · [Diagnostics](docs/DIAGNOSTICS.md)

**Contributing:** [CONTRIBUTING.md](CONTRIBUTING.md) · [Architecture](docs/ARCHITECTURE.md) · [Technology decisions](docs/TECHNOLOGY_DECISIONS.md) · [Release process](docs/RELEASE_PROCESS.md) · [Third-party licenses](docs/THIRD_PARTY_AND_LICENSES.md) · [Security](SECURITY.md) · [Engineering contract](AGENTS.md)

**Specifications:** [Gameplay](docs/GAMEPLAY_SPEC.md) · [Rendering](docs/RENDERING_SPEC.md) · [Networking](docs/NETWORKING_SPEC.md) · [Production pipeline](docs/PRODUCTION_PIPELINE_SPEC.md) · [Editor](docs/EDITOR_SPEC.md) · [Performance](docs/PERFORMANCE_STRATEGY.md) · [Platforms](docs/PLATFORM_STRATEGY.md) · [Reference projects](docs/REFERENCE_PROJECTS.md) · [Research register](docs/RESEARCH_REGISTER.md)

## Repository layout

| Path | Contents |
|---|---|
| `engine/<module>/` | Engine modules (`core`, `platform`, `rhi`, `render`, `ecs`, `scene`, `assets`, `physics`, `script`, `net` and more), each with `include/victus/<module>/` and `src/`. See [Architecture](docs/ARCHITECTURE.md) |
| `editor/` | VICTUS Studio |
| `apps/player/`, `platforms/android/` | Standalone game player and the Android Gradle project |
| `tools/` | The `victus` CLI, shader compiler, dependency scanner, lint, setup and release scripts |
| `samples/`, `tests/`, `benchmarks/` | Sample games, automated tests (CTest) and benchmarks |
| `docs/` | Guides, specifications, decisions and status |

## License

Original VICTUS code is licensed under [Apache-2.0](LICENSE). Third-party components keep their own licenses and notices; see [docs/THIRD_PARTY_AND_LICENSES.md](docs/THIRD_PARTY_AND_LICENSES.md).
