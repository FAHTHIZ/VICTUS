# VICTUS

**An independent, cross-platform engine for complete games, real-time rendering, and large-scale simulation.**

VICTUS aims to give developers an end-to-end path from an empty project to a finished, playable, distributable game. It is intended for 2D and 3D games, high-fidelity visuals, simulation-heavy worlds, mobile experiences, multiplayer projects, and dedicated servers. It must provide real gameplay and real rendering—not just technical demos.

**Status:** early; not production-ready. The list below describes product goals; see [Development status](#development-status) for what works today. A feature counts as delivered only when implementation, integration, tests and a usable workflow prove it.

## What VICTUS is being built to provide

- **Complete gameplay framework:** worlds, scenes, entities, components, game modes/states, player controllers, cameras, input mapping, interaction, physics, navigation, gameplay AI, scripting, visual scripting, events, timers, save/load, and reusable prefabs.
- **Real 2D and 3D development:** dedicated 2D and 3D workflows, sprites, tilemaps, UI, physics, 3D meshes, skeletal animation, materials, terrain, large worlds, and spatial audio.
- **Actual rendering:** a production-oriented graphics architecture with PBR materials, lighting, shadows, reflections, post-processing, GPU profiling, particles, visual effects, atmosphere, water, scalable quality tiers, and researched advanced rendering paths.
- **Complete creator tools:** VICTUS Studio, scene/world editing, visual graphs, material/shader tools, animation tools, terrain and procedural tools, cinematic sequencing, asset inspection, debugging, profiling, and play-in-editor.
- **Production pipeline:** asset import and validation, stable asset IDs, dependency tracking, incremental cooking, caching, build automation, packaging, platform presets, crash diagnostics, and reproducible releases.
- **Multiplayer and online foundations:** authoritative server architecture, replication, prediction/reconciliation, interpolation, dedicated/headless servers, network simulation, and replaceable online-service integrations.
- **Large-scale simulation:** deterministic and controlled simulation where feasible, populations, economies, ecosystems, crowds, autonomous agents, world streaming, and validated simulation LOD.
- **Extensibility:** documented VICTUS APIs, plugin/module boundaries, safe modding architecture, versioning, migration tools, and an ecosystem that does not require another complete engine.
- **Cross-platform delivery:** Windows, Linux, macOS, Android, and iOS as researched and validated targets; web, XR, and console support only when technically and legally viable. Console development depends on platform-holder access and approval.
- **Three complete reference projects:** a 2D game, a 3D gameplay-focused game, and a simulation project, all built with VICTUS systems rather than hidden custom workarounds.

## Product principles

1. **Independent core:** VICTUS owns its architecture, runtime, editor experience, project model, and public APIs. Specialist libraries may be used behind replaceable interfaces after research and license review.
2. **Complete, not cosmetic:** no placeholder, mock, or screenshot-only feature counts as implemented.
3. **Finish vertical slices:** implement, integrate, test, profile, document, and demonstrate each subsystem before marking it complete.
4. **Measure performance:** publish reproducible results with hardware, OS, driver, build, quality settings, and test methodology.
5. **Mobile is a first-class constraint:** account for memory, bandwidth, battery, sustained thermals, startup, and device capability.
6. **Separate concerns:** gameplay, authoritative simulation, presentation, rendering, editor, and server execution have clear boundaries.
7. **Build for creators:** usability, undo/redo, recovery, import feedback, debugging, tutorials, and packaging are engine requirements.
8. **Keep main buildable:** use small verified commits, remove obsolete implementations, and never claim unverified support.
9. **Respect rights and safety:** audit code, assets, licenses, patents, trademarks, plugin boundaries, and untrusted project content.

## Development status

VICTUS is early and not production-ready. What exists and is tested today: core runtime (jobs, memory, profiling, JSON, logging), SDL3 platform layer, an RHI on SDL_GPU (Vulkan verified), a render graph, real 2D sprite rendering and 3D PBR rendering with cascaded shadows, SSAO, bloom, auto exposure, filmic tonemapping, FXAA, automatic GPU instancing and Low/Medium/High/Ultra quality tiers, an archetype ECS, a versioned scene format, a texture/glTF asset pipeline, the VICTUS editor, Luau gameplay scripting with hot reload, 3D (Jolt) and 2D (Box2D) rigid-body physics with triggers, queries and script collision events, 3D and 2D character controllers (slopes, steps, one-way platforms, pushing bodies), 3D and 2D joints with limits, motors and breaking, glTF skeletal animation with GPU skinning and cross-fades, CPU particles with soft HDR billboards, in-game UI (screen-scaled HUDs and menus with text, images and buttons), miniaudio-based audio (buses, 3D spatialization, streaming music, script API), Recast/Detour navigation, server-authoritative multiplayer over UDP (ENet) with a dedicated headless server, client prediction and interpolation, a standalone player, the `victus` CLI and `victus export` packaging. [docs/STATUS.md](docs/STATUS.md) lists exactly what is verified and how; [ROADMAP.md](ROADMAP.md) is the plan.

## Quick start

Build (CMake 3.24+, a C++20 compiler; dependencies are fetched and pinned automatically, no account or sign-in needed):

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
~~~

Create, edit and run a project:

~~~bash
./build/bin/victus templates                         # 3d, 2d, simulation, multiplayer
./build/bin/victus new MyGame --template 3d          # creates MyGame/victus.project, scenes/, assets/
./build/bin/victus_editor MyGame                     # open in the editor (or run with no arguments for the launcher)
./build/bin/victus run MyGame                        # run the game in its own window
./build/bin/victus run MyGame --quality low          # override the project's quality tier (low/medium/high/ultra)
./build/bin/victus import MyGame                     # import/cook assets dropped into MyGame/assets
./build/bin/victus check MyGame                      # validate scenes and script syntax
./build/bin/victus simulate MyGame --frames 600 --hold D   # run gameplay + scripts headless (no GPU)
./build/bin/victus export MyGame --out MyGame-export  # runnable folder: player + game/game.vpak
./MyGame-export/MyGame                               # play the exported game
~~~

`victus export` imports stale assets, validates every scene and script (it fails on syntax errors or missing script references), and writes the player executable, `game/game.vpak` (project, scenes, scripts, audio and cooked textures/models; source images and glTF files are not shipped), `THIRD_PARTY_NOTICES.txt` and an `export.json` manifest. Export for the platform you are on; cross-platform export needs a player built for the target.

Template controls (a gamepad works too: left stick moves, A/South jumps, View/Back toggles the help panel): 2D platformer — A/D or arrows to run, Space/W/Up to jump (also up through the light one-way ledge), S/Down to drop back through it, bump the swinging lantern, collect the coins; 3D — WASD/arrows walk the animated robot (it climbs the steps, swings the hinged gate and shoves the crate stack), Space jumps, and the camera follows; simulation — the camera orbits the wave field; multiplayer — WASD/arrows walk, Space jumps, coins are scored by the server. In the editor, click the viewport after pressing Play so the game receives the keyboard.

### Scripting

Gameplay scripts are [Luau](https://luau.org) files in `assets/`. A script returns a table; public fields become editable properties in the inspector, and `start`, `update(dt)`, `fixed_update(dt)` and `reload` are called for each entity that has a Script component pointing at the file. Saving the file reloads it while the game runs.

~~~lua
-- assets/scripts/spin_and_jump.luau
local Spin = {}
Spin.speed = 90          -- degrees per second, editable per entity

function Spin:update(dt)
    self.entity:rotate(vector.create(0, 1, 0), self.speed * dt)
    if input.pressed("Space") then
        self.entity:translate(vector.create(0, 1, 0))
        log.info(self.entity.name, "jumped")
    end
end

return Spin
~~~

Available globals: `scene.find/spawn/destroy/query/count`, `input.down/pressed/released/axis/mouse_*`, `time.delta/now/frame`, `log.info/warn/error`. Entities expose `position`, `rotation` (Euler degrees), `scale`, `name`, `parent`, `forward/right/up`, and `get/set/has/remove` for any component as JSON-like tables. Scripts run sandboxed with time and memory limits, so a bad loop reports an error instead of freezing the editor.

Physics: add `RigidBody`/`Collider` (3D) or `RigidBody2D`/`Collider2D` (2D) from the inspector's Add Component menu. Scripts get `physics.raycast/raycast2d/overlap_sphere/overlap_box/overlap_circle/overlap_box2d/gravity/set_gravity`, entity `velocity`, `angular_velocity`, `add_force/add_impulse/add_torque/add_angular_impulse`, and the callbacks `on_collision_enter(other, info)`, `on_collision_exit(other)`, `on_trigger_enter(other)` and `on_trigger_exit(other)`. Characters: add `CharacterController` (3D) or `CharacterController2D` (2D) and call `entity:move_character(velocity)` each fixed update, `entity:jump(speed)` (only succeeds when grounded), `entity.grounded`, `entity:character_state()` and, in 2D, `entity:drop_through(seconds?)` to fall through `Collider2D`s marked `one_way`. Joints: `Joint` (fixed/point/hinge/slider/distance) and `Joint2D` (weld/revolute/prismatic/distance/wheel) connect an entity to another or to the world, with limits, springs, motors (`entity:set_motor(on, speed)`) and `break_force`; `entity:joint_state()` reports whether it is active or broken. View > Physics Colliders toggles collider wireframes in the editor viewport.

Audio: add `AudioSource` (clip, bus, volume, pitch, loop, play on start, stream, spatial with min/max distance and rolloff) and `AudioListener` from the inspector's Audio menu; drag a `.wav/.ogg/.mp3/.flac` asset onto the clip field, double-click an audio asset to preview it. Scripts get `audio.play(clip, {volume, pitch, bus, position})`, `audio.set_volume/volume/set_muted/muted(bus)`, `audio.available()` and entity `play_audio/stop_audio/pause_audio/audio_playing`. Buses: `master`, `music`, `sfx`, `ui`, `ambient`, `voice`. Set `VICTUS_NO_AUDIO=1` to run without an output device (mixing still happens). The 2D template ships generated jump/coin/music sounds and the 3D crates play impact sounds.

UI: put a `UiCanvas` on an entity and give its children `UiRect` (anchors, pivot, position, size) plus `UiPanel`, `UiImage`, `UiText` (built-in Inter or a project `.ttf`), `UiButton` (with an `action` string) or `UiLayout`. The canvas is authored at 1280x720 and scales with the window; it draws over 2D and 3D scenes in the player, editor viewport and screenshots. Scripts get `on_click(button, action)` on the button or any ancestor, entity `set_text/text`, `set_visible/visible`, `ui_state()`, and `ui.screen_size/pointer_over_ui/focus/focused/click`. Both templates ship a HUD (Controls button, H toggles the panel); the 2D one counts coins and shows a banner when all are collected.

Navigation: add `NavAgent` (speed, acceleration, radius, stopping distance, avoidance) to any entity and call `entity:set_destination(point)`; VICTUS builds a navmesh automatically from the scene's static colliders (Recast & Detour), rebuilds it when they change (kinematic bodies carve an upright cylinder) and steers agents along the path with local avoidance (DetourCrowd). Agents with a `CharacterController` walk through the physics character mover, so gravity, steps and pushing still apply. An optional `NavMeshSurface` sets the agent size, step height, slope and voxel size the mesh is built for. Scripts also get `entity:stop()`, `entity:nav_state()` and `nav.find_path/nearest/raycast/random_point/rebuild/available`. View > Navigation Mesh draws the navmesh (and agent paths in play mode) in the editor viewport. The 3D template's companion robot follows you around the obstacles.
Input: scripts read named actions instead of hard-coded keys, so keyboards, mice and gamepads (up to four, SDL3 standard layout) all work. Actions live in the `input` section of `victus.project` (created with defaults when absent: `move_x`, `move_y`, `look_x`, `look_y`, `jump`, `fire`, `interact`, `sprint`, `pause`, `help`); each has `bindings` (buttons) or `positive`/`negative` (axes) and an optional `dead_zone` (default 0.2). Controls are key names (`Space`, `W`, `Left`), `Mouse0`–`Mouse2`, gamepad buttons `Pad.South/East/West/North`, `Pad.LeftShoulder`, `Pad.Start`, `Pad.Back`, `Pad.DpadUp`… (aliases `A/B/X/Y/LB/RB/LT/RT/Select/Menu`), and half-axes `Pad.LeftX+`, `Pad.LeftY-`, `Pad.RightTrigger`. Scripts get `input.action/action_pressed/action_released(name)`, `input.value(name)` (−1..1), `input.vector(x_name, y_name)` (clamped to length 1), `input.actions()`, `input.bindings(name)`, `input.rebind(name, {...})` (for the running game), `input.gamepad_connected/gamepad_button/gamepad_axis(pad?)`; raw `input.down/pressed/axis` still work. `victus check` reports unknown controls and `victus simulate --hold Pad.LeftX+` holds a gamepad control headlessly. Gamepad input is carried over the network to the server like keys. In the editor, Edit > Input Actions lists every action: click + Listen and press a key, mouse button, gamepad button or push a stick to bind it (Escape cancels), or type a control name; changes are saved to `victus.project` and apply immediately, even while playing.

### Regression captures

~~~bash
./build/bin/victus run MyGame --record walk.vcapture          # play normally; input, frame times and state digests are saved
./build/bin/victus simulate MyGame --replay walk.vcapture     # headless check: exit 5 at the first frame whose state differs
./build/bin/victus run MyGame --replay walk.vcapture          # watch the recording replay in the game window
./build/bin/victus simulate MyGame --hold D,Space --frames 600 --record bot.vcapture   # scripted capture for CI
~~~

A capture stores every frame's input (keys, mouse, gamepads), the frame times, the random seed scripts used for `math.random` and a digest of the scene every 60 frames: entity names and transforms, every script's own number, boolean, string and vector fields (fields starting with `_` are left out) and the velocities of moving bodies and characters. Replaying re-runs the game from its first frame with the same input and seed and compares the digests, so a change to scripts, physics or the engine that alters how the game plays fails with the frame where it diverged. When a replay diverges it names the entities that differ, for example `replay diverged at frame 60: Buddy: position (-0.843, 0, 1.343) expected (-1.2961, 0, 1.798); ...` or `Keeper: script.score 11 expected 10` (captures keep the per-entity state behind each digest for scenes of up to 4096 entities). In the editor, the toolbar's Record button (or Play > Record Capture) plays while recording and Stop saves the capture to `<project>/.victus/captures/`; Play > Replay Last Capture / Replay Capture runs one back in the viewport, pausing at the end (Matched) or at the first divergence (Diverged, with the entities in the tooltip and status bar). Captures work offline (not with `--connect`); determinism is only verified on one machine and compiler so far.

### Multiplayer

~~~bash
./build/bin/victus new Arena --template multiplayer
./build/bin/victus serve Arena --port 27015                     # dedicated headless server (Ctrl+C stops it)
./build/bin/victus run Arena --connect 127.0.0.1:27015 --name ann   # each player in their own window
./build/bin/victus run Arena --host --port 27015 --name host       # or: host a session and play in it (listen server)
./build/bin/victus simulate Arena --connect 127.0.0.1:27015 --name bot --hold D --frames 600   # headless test client
./build/bin/victus simulate Arena --connect 127.0.0.1:27015 --conditions latency=80,jitter=15,loss=0.05
~~~

The server owns the game: it runs scripts and physics, spawns a copy of the scene's player template (an entity whose `NetworkIdentity` has `player_template` on) for every client, applies each client's input to that player only, and sends snapshots of every `NetworkIdentity` entity (transform, velocity, script-declared values) 30 times a second, each encoded as millimetre deltas against the last snapshot the client acknowledged (entities the client already knows cost nothing). Clients predict their own player and reconcile with the server, and interpolate everyone else 100 ms in the past. Scripts get `net.role/is_server/is_client/authority/id/connected/local_player/clients/stats`, `net.send(name, value, to?)` with `on_net_message(name, value, sender)`, and entity `set_net/get_net` (replicated values), `has_authority`, `is_local`, `owner`. Clients of a different game or engine build are refused. `--conditions` emulates latency, jitter and loss on either side. Traffic is encrypted and authenticated by default: `victus serve` creates a server key in `<project>/.victus/server.key` and prints its public half (`encrypted; server key 43ae...`); players can pin it with `--server-key <hex>` so nobody can pose as the server, and `victus serve --password <p>` makes players send `--password <p>` (only ever over the encrypted channel). Forged, altered and replayed packets are dropped. `--insecure` on both sides turns encryption off for trusted networks. With `--host` (also on exported games' `victus_player`) a player hosts the session and plays in it: the host's character is simulated by the server with no network delay, shows up as player 0 to everyone, and the session ends when the host quits; use `victus serve` for fair or long-running games. Hosting takes the same `--port`, `--max-clients`, `--key`, `--password` and `--insecure` options as `victus serve`. Dropped connections recover on their own: the server keeps a player's character for 30 s after the connection drops, and the client keeps trying to rejoin for 20 s and gets the same character, score and player number back. Scripts see `client_joined`, `client_dropped`, `client_rejoined` and `client_left` in `on_net_message` on the server.

In the editor, Play > Play as Host (or the toolbar's Play button with Play > Network Mode set to Host) runs Play mode as such a session with you as player 0; View > Network shows the server key and a ready-to-copy join command, bandwidth graphs, connected players and every replicated entity with its owner and traffic. `victus_editor <project> --host --port 27960 --show network` opens straight into it. Set In-editor clients (Network window, or `--clients 2`) to add up to three more players inside the editor, each with its own world; the toolbar's View menu or Ctrl+1..4 picks which player you see and control.

### Editor

In the editor: Play/Stop runs the scene in place on a copy (edits made during play are discarded), Ctrl+S saves, Ctrl+Z/Ctrl+Y undo/redo, F frames the selection, right-drag orbits, middle-drag pans, the wheel zooms. Drop `.gltf`/`.glb`/`.png`/`.jpg` files into the project's `assets/` folder and press Reimport; drag a model from the asset table into the viewport to instantiate it, or a `.luau` script onto an entity's Script component (double-click attaches it to the selection).

Prefabs: select an entity in the editor and choose Create Prefab (hierarchy context menu) to save it and its children as `assets/prefabs/<Name>.vprefab`; drag or double-click the prefab to place instances. Changes you make to an instance are kept as overrides (shown in the inspector's Prefab section) when the prefab changes; Apply writes them back to the prefab, Revert discards them and Unpack turns the instance into plain entities. Scripts spawn prefabs with `scene.instantiate("prefabs/crate.vprefab", { position = vector.create(0, 2, 0) })`; in the 3D template, E or the gamepad X button drops a crate.

Save games: scripts store progress and settings with `save.write("profile", { volume = 0.8 })` / `save.read("profile")`, and capture the whole game with `save.checkpoint("quick")` / `save.restore("quick")`; each script keeps its own data across a checkpoint through `function M:save_state()` / `function M:load_state(state)`. Files are checksummed, written atomically and recovered from a backup if damaged; raise `save_version` in `victus.project` and register `save.migrate(old_version, fn)` when the saved data changes shape. The 2D and 3D templates quick save with F5 (gamepad LB) and quick load with F9 (RB). Exported games keep saves in the per-user VICTUS folder; editor play mode, `victus run` and `victus simulate` use the project's `.victus/saves` (`--saves <dir>` overrides).

Hot reload: while `victus run` (unless `--no-hot-reload`) or the editor is running, saving a texture, model (or its `.meta` settings, `.bin` or textures) or audio clip under `assets/` swaps it in within about a second, and scripts reload as before. If a file is saved half-way and cannot be imported, the previous version keeps running and the error is shown. Scene and font changes appear the next time the game or project starts; shaders are part of the engine and are not reloaded.

Ship a game with `victus export` (above); the result runs anywhere the player runs, with no editor or SDK installed. Your project files are plain JSON and assets you own; Apache-2.0 allows commercial use.

For Visual Studio/Xcode generators, binaries are under `build/bin/<Config>/`. See [CMake presets](CMakePresets.json).

## Repository map

- `engine/<module>/` — engine modules (`core`, `platform`, `rhi`, `render`, `ecs`, `scene`, `assets`, `runtime`, `ui`, `gameplay`, `physics`, `animation`, `particles`, `audio`, `script`, `project`, `player`), each with `include/victus/<module>/` and `src/`
- `editor/` — the VICTUS editor (library + executable)
- `apps/player/` — standalone game player
- `tools/` — `victus` CLI and shader compiler
- `samples/`, `benchmarks/`, `tests/` — samples, benchmark harness, automated tests
- `third_party/` — vendored dependencies with licence files
- `docs/` — architecture, specifications, research, decisions and status
- `AGENTS.md` — autonomous engineering contract

## Engineering documents

- [Autonomous engineering contract](AGENTS.md)
- [Roadmap](ROADMAP.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Gameplay specification](docs/GAMEPLAY_SPEC.md)
- [Rendering specification](docs/RENDERING_SPEC.md)
- [Networking specification](docs/NETWORKING_SPEC.md)
- [Production pipeline specification](docs/PRODUCTION_PIPELINE_SPEC.md)
- [Reference projects and acceptance](docs/REFERENCE_PROJECTS.md)
- [Editor specification](docs/EDITOR_SPEC.md)
- [Performance strategy](docs/PERFORMANCE_STRATEGY.md)
- [Platform strategy](docs/PLATFORM_STRATEGY.md)
- [Research register](docs/RESEARCH_REGISTER.md)
- [Technology decisions](docs/TECHNOLOGY_DECISIONS.md)
- [Third-party and license policy](docs/THIRD_PARTY_AND_LICENSES.md)
- [Engineering status](docs/STATUS.md)

## License

Original VICTUS code: Apache-2.0. Third-party components retain their own licenses and notices.
