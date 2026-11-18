# VICTUS User Guide

How to use what VICTUS ships today. Every feature described here is implemented and covered by automated tests; limitations are stated where they matter. For setup see [Getting started](GETTING_STARTED.md); for what is verified and how, see [STATUS.md](STATUS.md); for subsystem maturity see [Known limitations](KNOWN_LIMITATIONS.md).

Contents: [Scripting and gameplay systems](#scripting) · [Regression captures](#regression-captures) · [Multiplayer](#multiplayer) · [Editor and authoring](#editor) · [Shipping](#shipping)

## Scripting

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

### Available globals

Available globals: `scene.find/spawn/destroy/query/count`, `input.down/pressed/released/axis/mouse_*`, `time.delta/now/frame`, `log.info/warn/error`. Entities expose `position`, `rotation` (Euler degrees), `scale`, `name`, `parent`, `forward/right/up`, and `get/set/has/remove` for any component as JSON-like tables. Scripts run sandboxed with time and memory limits, so a bad loop reports an error instead of freezing the editor.

### Physics

Physics: add `RigidBody`/`Collider` (3D) or `RigidBody2D`/`Collider2D` (2D) from the inspector's Add Component menu. Scripts get `physics.raycast/raycast2d/overlap_sphere/overlap_box/overlap_circle/overlap_box2d/gravity/set_gravity`, entity `velocity`, `angular_velocity`, `add_force/add_impulse/add_torque/add_angular_impulse`, and the callbacks `on_collision_enter(other, info)`, `on_collision_exit(other)`, `on_trigger_enter(other)` and `on_trigger_exit(other)`. Characters: add `CharacterController` (3D) or `CharacterController2D` (2D) and call `entity:move_character(velocity)` each fixed update, `entity:jump(speed)` (only succeeds when grounded), `entity.grounded`, `entity:character_state()` and, in 2D, `entity:drop_through(seconds?)` to fall through `Collider2D`s marked `one_way`. Tick `top_down` on a `CharacterController2D` for top-down games: it moves on both axes with no gravity and slides along walls. Joints: `Joint` (fixed/point/hinge/slider/distance) and `Joint2D` (weld/revolute/prismatic/distance/wheel) connect an entity to another or to the world, with limits, springs, motors (`entity:set_motor(on, speed)`) and `break_force`; `entity:joint_state()` reports whether it is active or broken. View > Physics Colliders toggles collider wireframes in the editor viewport.

### Audio

Audio: add `AudioSource` (clip, bus, volume, pitch, loop, play on start, stream, spatial with min/max distance and rolloff) and `AudioListener` from the inspector's Audio menu; drag a `.wav/.ogg/.mp3/.flac` asset onto the clip field, double-click an audio asset to preview it. Scripts get `audio.play(clip, {volume, pitch, bus, position})`, `audio.set_volume/volume/set_muted/muted(bus)`, `audio.available()` and entity `play_audio/stop_audio/pause_audio/audio_playing`. Buses: `master`, `music`, `sfx`, `ui`, `ambient`, `voice`. Bus effects: `audio.set_lowpass(bus, hz)` / `lowpass(bus)` (2nd-order low-pass; 0 turns it off; Master filters everything), `audio.set_reverb_send(bus, 0..1)` / `reverb_send(bus)` (every bus but Master can feed one shared reverb made of four feedback combs) and `audio.set_reverb_decay(0..0.95)` / `reverb_decay()`. Effects are reset when the scene changes or a play session starts; volumes and mutes are kept. The 3D template sends Sfx to the reverb (`reverb_send` field on the HUD script); the 2D template muffles the music once every coin is collected. Set `VICTUS_NO_AUDIO=1` to run without an output device (mixing still happens). The 2D template ships generated jump/coin/music sounds and the 3D crates play impact sounds.

### UI

UI: put a `UiCanvas` on an entity and give its children `UiRect` (anchors, pivot, position, size) plus `UiPanel`, `UiImage`, `UiText` (built-in Inter or a project `.ttf`), `UiButton` (with an `action` string) or `UiLayout`. The canvas is authored at 1280x720 and scales with the window; it draws over 2D and 3D scenes in the player, editor viewport and screenshots. Scripts get `on_click(button, action)` on the button or any ancestor, entity `set_text/text`, `set_visible/visible`, `ui_state()`, and `ui.screen_size/pointer_over_ui/focus/focused/click`. Scroll views: add `UiScroll` (`vertical`, `horizontal`, `offset`, `wheel_step`, `drag_to_scroll`) to a rect; its children are laid out as usual, clipped to the rect and shifted by the offset, which is clamped to the content extent (descendants' bounding box plus `UiLayout` padding, reported as `ui_state().scroll_max`). The mouse wheel scrolls the view under the pointer, dragging more than 8 px scrolls it (and cancels a pressed button), and keyboard focus scrolls the focused child into view. Scripts use `entity:scroll_to(x, y)` and `entity:scroll()` → x, y, max_x, max_y. The multiplayer template's scoreboard is a scroll view. Both templates ship a HUD (Controls button, H toggles the panel); the 2D one counts coins and shows a banner when all are collected.

### Localization

Localization: put one string table per language in `assets/localization/<locale>.vstrings` (for example `en.vstrings`, `fr.vstrings`, `pt-BR.vstrings`):

```json
{ "name": "Français", "strings": { "hud": { "controls": "Commandes", "score": "Score : {score}" } } }
```

Nested objects become dotted keys (`hud.score`). Set a `UiText`'s `key` field to show that key's translation; its `text` stays the fallback and is what the editor viewport shows outside play mode. Scripts use `loc.text(key, params?)` (fills `{name}` placeholders from `params`; an unknown key returns the key itself so it is visible), `loc.has(key)`, `loc.locale()`, `loc.set_locale(code)` (false when there is no table; it lasts for the session, across scene loads), `loc.locales()` and `loc.name(code?)` (the table's `name`). `entity:set_text(...)` shows literal text and clears the key; `entity:set_text_key(key)` switches back to a translated key. Lookups try the current locale, then the project's `default_locale` (`"default_locale"` in `victus.project`, default `"en"`); when that locale has no table the first one in name order is used. Edited tables hot-reload in editor play mode and `victus run` (unless `--no-hot-reload`; exported games do not watch files). The 3D, 2D and multiplayer templates ship `localization/en.vstrings` for their HUD.

The built-in Inter fonts cover Latin (including Latin Extended-A/B), Cyrillic and Greek. Other scripts (CJK, Arabic, Hebrew, Devanagari, Thai, Vietnamese stacked accents) need a project font in `UiText.font`, and there is no text shaping or right-to-left layout, so Arabic and Indic scripts do not render correctly yet. There is no plural or gender selection beyond what a script does with `loc.text` and its own keys.

### Rendering from scripts

Rendering from scripts: `entity:set_material("materials/gold.vmat")` / `material()` swap a MeshRenderer's material asset (nil restores the mesh's own), `set_mesh("sphere")` or `set_mesh("models/ship.glb", index)` / `mesh()`, `set_color(vector, alpha?)` / `color()` tint a MeshRenderer or Sprite, `set_sprite("textures/coin.png")` / `sprite()`, `set_cast_shadows(bool)`, and `set_visible/visible` also hide meshes. Paths resolve through the asset database, so they work in exported games too. Add `MaterialOverrides` (Rendering menu) to scroll or offset one entity's textures and override its emissive glow while sharing a material; scripts use `set_uv_offset(v)`, `set_uv_scroll(v)` and `set_emissive(v|nil, strength?)`.

### Navigation

Navigation: add `NavAgent` (speed, acceleration, radius, stopping distance, avoidance) to any entity and call `entity:set_destination(point)`; VICTUS builds a navmesh automatically from the scene's static colliders and terrains (Recast & Detour), rebuilds it when they change (kinematic bodies carve an upright cylinder) and steers agents along the path with local avoidance (DetourCrowd). Agents with a `CharacterController` walk through the physics character mover, so gravity, steps and pushing still apply. An optional `NavMeshSurface` sets the agent size, step height, slope and voxel size the mesh is built for. Scripts also get `entity:stop()`, `entity:nav_state()` and `nav.find_path/nearest/raycast/random_point/rebuild/available`. View > Navigation Mesh draws the navmesh (and agent paths in play mode) in the editor viewport. The 3D template's companion robot follows you around the obstacles.

### AI

AI: add `BehaviorTree` with a `.vbt` tree (JSON: sequence/selector/parallel, guards, cooldowns, timeouts, waits, blackboard checks and sets, `move_to` on the navmesh, `random_point`, C++ actions or Luau script methods as tasks) and `Perception` (sight cone with line of sight, hearing, memory) to make NPCs patrol, notice `Perceptible` entities and chase them. Window > AI Debugger shows sight cones and a window with the live tree and blackboard; scripts read and write blackboards (`entity:bb_get/bb_set`), read `entity:perception()` and call `ai.report_noise`, and trees hot reload during play. The 3D template's sentry is a working example. See [docs/AI.md](docs/AI.md).

### Cinematics

Cinematics: add `SequencePlayer` with a `.vseq` timeline (JSON tracks for transforms with step/linear/smooth/cubic keys, animation clips, audio one-shots, camera cuts and events) to animate doors, lifts and cutscenes. Scripts call `entity:play_sequence/seek_sequence/stop_sequence`, receive `on_sequence_event`, and sequences hot reload during play. Author them in the editor's Window > Sequencer timeline panel. See [docs/SEQUENCER.md](docs/SEQUENCER.md).

### Terrain

Terrain: Create > Terrain adds a generated `.vterrain` heightfield. Select it and use Window > Terrain Tools to raise, lower, smooth, flatten or paint layers in the viewport (one undo step per stroke; edits are saved with the scene). Scripts can query the ground with `terrain.height(x, z)`, `terrain.sample(x, z)` (height, normal, layer weights) and `terrain.raycast(origin, dir)`. "Add Foliage" in Terrain Tools creates a child whose mesh is scattered over the current paint layer (tune density, slope, height range and draw distance on its Foliage component).

### Input

Input: scripts read named actions instead of hard-coded keys, so keyboards, mice and gamepads (up to four, SDL3 standard layout) all work. Actions live in the `input` section of `victus.project` (created with defaults when absent: `move_x`, `move_y`, `look_x`, `look_y`, `jump`, `fire`, `interact`, `sprint`, `pause`, `help`); each has `bindings` (buttons) or `positive`/`negative` (axes) and an optional `dead_zone` (default 0.2). Controls are key names (`Space`, `W`, `Left`), `Mouse0`–`Mouse2`, gamepad buttons `Pad.South/East/West/North`, `Pad.LeftShoulder`, `Pad.Start`, `Pad.Back`, `Pad.DpadUp`… (aliases `A/B/X/Y/LB/RB/LT/RT/Select/Menu`), and half-axes `Pad.LeftX+`, `Pad.LeftY-`, `Pad.RightTrigger`. Scripts get `input.action/action_pressed/action_released(name)`, `input.value(name)` (−1..1), `input.vector(x_name, y_name)` (clamped to length 1), `input.actions()`, `input.bindings(name)`, `input.rebind(name, {...})` (for the running game), `input.gamepad_connected/gamepad_button/gamepad_axis(pad?)`; raw `input.down/pressed/axis` still work. `victus check` reports unknown controls and `victus simulate --hold Pad.LeftX+` holds a gamepad control headlessly. Gamepad input is carried over the network to the server like keys. In the editor, Edit > Input Actions lists every action: click + Listen and press a key, mouse button, gamepad button or push a stick to bind it (Escape cancels), or type a control name; changes are saved to `victus.project` and apply immediately, even while playing.

### Procedural meshes and helper processes

Procedural meshes and helper processes: `entity:set_mesh_data{positions=, indices=, normals=, colors=, uvs=}` builds a mesh from Luau tables, or from packed buffers with `{vertices=, indices=, layout="position:f32x3,normal:s8x4,color:u8x4", index="u32", encoding="base64"}`; `clear_mesh_data()` and `mesh_data()` complete it. A project can declare helper programs under `"bridges"` in `victus.project` (`{"sim": {"command": ["dotnet", "sim.dll", "serve"], "working_directory": "${project}"}}`); scripts call `local b = bridge.open("sim")` and `b:request({op = "frame"})` to exchange JSON lines. `json.encode/decode`, `require("scripts/lib/util")` and `input.mouse_wheel()` are also available.

## Regression captures

~~~bash
./build/bin/victus run MyGame --record walk.vcapture          # play normally; input, frame times and state digests are saved
./build/bin/victus simulate MyGame --replay walk.vcapture     # headless check: exit 5 at the first frame whose state differs
./build/bin/victus run MyGame --replay walk.vcapture          # watch the recording replay in the game window
./build/bin/victus simulate MyGame --hold D,Space --frames 600 --record bot.vcapture   # scripted capture for CI
~~~

A capture stores every frame's input (keys, mouse, gamepads), the frame times, the random seed scripts used for `math.random` and a digest of the scene every 60 frames: entity names and transforms, every script's own number, boolean, string and vector fields (fields starting with `_` are left out) and the velocities of moving bodies and characters. Replaying re-runs the game from its first frame with the same input and seed and compares the digests, so a change to scripts, physics or the engine that alters how the game plays fails with the frame where it diverged. When a replay diverges it names the entities that differ, for example `replay diverged at frame 60: Buddy: position (-0.843, 0, 1.343) expected (-1.2961, 0, 1.798); ...` or `Keeper: script.score 11 expected 10` (captures keep the per-entity state behind each digest for scenes of up to 4096 entities). In the editor, the toolbar's Record button (or Play > Record Capture) plays while recording and Stop saves the capture to `<project>/.victus/captures/`; Play > Replay Last Capture / Replay Capture runs one back in the viewport, pausing at the end (Matched) or at the first divergence (Diverged, with the entities in the tooltip and status bar). Captures work offline (not with `--connect`); determinism is only verified on one machine and compiler so far. Runs with a frame count (`victus run --frames N`, `victus simulate`) seed `math.random` with a fixed value unless you pass `--seed`, so they repeat exactly; interactive sessions use a time-based seed.

## Multiplayer

~~~bash
./build/bin/victus new Arena --template multiplayer
./build/bin/victus serve Arena --port 27015                     # dedicated headless server (Ctrl+C stops it)
./build/bin/victus run Arena --connect 127.0.0.1:27015 --name ann   # each player in their own window
./build/bin/victus run Arena --host --port 27015 --name host       # or: host a session and play in it (listen server)
./build/bin/victus simulate Arena --connect 127.0.0.1:27015 --name bot --hold D --frames 600   # headless test client
./build/bin/victus simulate Arena --connect 127.0.0.1:27015 --conditions latency=80,jitter=15,loss=0.05
~~~

### How sessions work

The server owns the game: it runs scripts and physics, spawns a copy of the scene's player template (an entity whose `NetworkIdentity` has `player_template` on) for every client, applies each client's input to that player only, and sends snapshots of every `NetworkIdentity` entity (transform, velocity, script-declared values) 30 times a second, each encoded as millimetre deltas against the last snapshot the client acknowledged (entities the client already knows cost nothing). Clients predict their own player and reconcile with the server, and interpolate everyone else 100 ms in the past. Scripts get `net.role/is_server/is_client/authority/id/connected/local_player/clients/stats`, `net.send(name, value, to?)` with `on_net_message(name, value, sender)`, and entity `set_net/get_net` (replicated values), `has_authority`, `is_local`, `owner`. Clients of a different game or engine build are refused. `--conditions` emulates latency, jitter and loss on either side. Traffic is encrypted and authenticated by default: `victus serve` creates a server key in `<project>/.victus/server.key` and prints its public half (`encrypted; server key 43ae...`); players can pin it with `--server-key <hex>` so nobody can pose as the server, and `victus serve --password <p>` makes players send `--password <p>` (only ever over the encrypted channel). Forged, altered and replayed packets are dropped. `--insecure` on both sides turns encryption off for trusted networks. With `--host` (also on exported games' `victus_player`) a player hosts the session and plays in it: the host's character is simulated by the server with no network delay, shows up as player 0 to everyone, and the session ends when the host quits; use `victus serve` for fair or long-running games. Hosting takes the same `--port`, `--max-clients`, `--key`, `--password` and `--insecure` options as `victus serve`. Dropped connections recover on their own: the server keeps a player's character for 30 s after the connection drops, and the client keeps trying to rejoin for 20 s and gets the same character, score and player number back. Scripts see `client_joined`, `client_dropped`, `client_rejoined` and `client_left` in `on_net_message` on the server.

In the editor, Play > Play as Host (or the toolbar's Play button with Play > Network Mode set to Host) runs Play mode as such a session with you as player 0; Window > Network shows the server key and a ready-to-copy join command, bandwidth graphs, connected players and every replicated entity with its owner and traffic. `victus_editor <project> --host --port 27960 --show network` opens straight into it. Set In-editor clients (Network window, or `--clients 2`) to add up to three more players inside the editor, each with its own world; the toolbar's View menu or Ctrl+1..4 picks which player you see and control.

## Editor

### Starter characters

The 3D and Multiplayer templates give the player a Vanguard robot: V-01 by default, V-02 with `victus new MyGame --template 3d --character v02`, or the plain capsule body with `--character none`. The launcher's New Project form has the same Character choice. Both robots share one skeleton, the clips Idle, Walk, Run, Jump, Fall, Land and Interact, the controller `assets/anim/characters/vanguard_locomotion.vanim` and the player script, so switching changes only the look; the companion uses the other one. Attachment nodes `Socket.Head`, `Socket.Hand.L/R`, `Socket.Back` and `Socket.Root` hold items. Details, swapping in an existing project and regenerating the models from Blender source: [CHARACTERS.md](CHARACTERS.md).

### The Studio window

When no project is open the Studio shows its start screen. The sidebar has four pages. New Project shows the four templates as cards with a one-line description of what each one plays like; pick one, type a name and the Studio checks the name and folder as you type (empty names, characters that do not work on every system such as `/ : ? *`, Windows reserved names like `CON`, and folders that already hold something are explained in plain words before you can press Create). Recent lists the projects you opened, marks those whose folder has moved, and lets you locate or remove them. Samples lists the bundled sample games; opening one copies it into your projects folder first, so the original stays as it is. Learn walks through the first ten minutes and lists the handy shortcuts. `victus_editor --page samples` (or `new`, `recent`, `learn`) opens a given page; `VICTUS_SAMPLES_DIR` points the Samples page at another folder.

The toolbar under the menu has Save (orange while the scene has unsaved changes), Undo and Redo on the left (their tooltips name the step they will undo), Play, Pause, Stop and Record in the middle, a coloured badge for what is running (Playing, Paused, Recording, Host, replay result) and, on the right, Search commands: click it or press Ctrl+Shift+P (or F1) to find and run any editor action by name. Every icon has a tooltip with its shortcut, including disabled ones, which say why they are disabled.

The status bar at the bottom shows the project, the open scene and whether it is saved, the entity count, the latest notification, and the number of errors and warnings in the Console; click a count to bring the Console forward.

The Problems panel (a tab next to the Console) lists mistakes in the project: scripts that do not compile, missing materials or textures, broken prefabs, behaviour trees, animation controllers, sequences and input actions. It runs the same checks as `victus check`, in the background, whenever the project opens and after every save, and Check Project runs it on demand. It reads the saved files, so save first to check your latest edits. Each row shows what is wrong and how to fix it; Select entity (or a double-click) jumps to the entity in the open scene, materials, visual scripts and sequences open in their editors, and the rest offer Copy path. Filter by errors, warnings or text. The status bar shows the count; click it to open the panel.

File > Export Game (or the export button next to Undo and Redo) builds a folder with your game that runs without VICTUS, the same as `victus export`. The dialog suggests `<project>/build/<GameName>`, says whether the project has errors (export is blocked until they are fixed, with a button to show them) or unsaved changes (Save and Export saves first), and exports in the background so the Studio keeps working. When it is done it shows the file count, size and folder; share the whole folder. It refuses folders that already hold other files, but replaces a previous export.

The Window menu lists every panel: the core ones (Viewport, Hierarchy, Inspector, Assets, Console, Problems), tools (Animation, Sequencer, Graph Editor, Material Editor, Tile Palette, Terrain Tools, Input Actions) and debug views (Profiler, Network, AI Debugger). Picking one opens it if you closed it and brings it to the front; the x on a tab closes it, except the Viewport, which always stays. Window > Layout switches between five presets, each opening the panels it needs and docking everything in place: Default, Level Design (large viewport), Animation (Animation and Sequencer under the viewport), Scripting (Graph Editor in the middle, Console below) and Debugging (Profiler, Console, Network, AI Debugger). Window > Reset Layout puts every panel of the current preset back where it belongs. The same actions are in the command search as "Window: ...". `victus_editor --layout scripting` (or `default`, `level-design`, `animation`, `debugging`) starts in a preset.

In the viewport the floating strip in the top-left corner picks the transform tool (Move W, Rotate E, Scale R), world or local axes (T), snapping (hold Ctrl while dragging to invert it) and, behind the gear, snap steps and gizmo visibility. Render stats sit under it. The Hierarchy shows an icon for each entity's kind (camera, light, mesh, sprite, script, prefab, terrain and so on) and the Assets panel shows type icons until a thumbnail is ready. The Tile Palette opens by itself when you select a tilemap.

### Basics

In the editor: Play/Stop runs the scene in place on a copy (edits made during play are discarded), Ctrl+S saves, Ctrl+Z/Ctrl+Y undo/redo, F frames the selection, right-drag orbits, middle-drag pans, the wheel zooms. Drop `.gltf`/`.glb`/`.png`/`.jpg` files into the project's `assets/` folder and press Reimport; drag a model from the asset table into the viewport to instantiate it, or a `.luau` script onto an entity's Script component (double-click attaches it to the selection).

### Prefabs

Prefabs: select an entity in the editor and choose Create Prefab (hierarchy context menu) to save it and its children as `assets/prefabs/<Name>.vprefab`; drag or double-click the prefab to place instances. Changes you make to an instance are kept as overrides (shown in the inspector's Prefab section) when the prefab changes; Apply writes them back to the prefab, Revert discards them and Unpack turns the instance into plain entities. Scripts spawn prefabs with `scene.instantiate("prefabs/crate.vprefab", { position = vector.create(0, 2, 0) })`; in the 3D template, E or the gamepad X button drops a crate.

### Scene transitions

Scene transitions: `scene.load("scenes/level2.vscene", { lives = 3 })` switches to another project scene at the end of the update (it returns `nil, error` for a missing or broken scene), `scene.reload()` restarts the current one, `scene.current()` returns its path and `scene.params()` the table passed to the load. Scripts restart in the new scene; module-level Luau variables carry over. `victus check` and `victus export` report `scene.load` calls that name scenes which do not exist.

### Animation controllers

Animation controllers: a `.vanim` file is a state machine for a character's clips — states play a clip or blend several by a parameter (`blend1d`: Idle at 0, Walk at 1.6, Run at 5; `blend2d` for strafing), and transitions switch states when conditions on parameters hold (`grounded` is true, `speed > 0.1`, the `jump` trigger was set), with a cross-fade. Set the Animator's `controller` field to the file (for example `anim/player.vanim`), then drive it by setting parameters — `entity:set_anim_param("speed", 3)`, `entity:anim_trigger("jump")`, or the Animation nodes in visual scripts — and the controller picks and blends the clips (`entity:anim_state()` tells you where it is). `victus check` reports mistakes with the element that has them, controllers hot reload while the game runs, and a broken controller falls back to the Animator's plain clip. The 3D and Multiplayer templates show it end to end: their Vanguard robot uses `assets/anim/characters/vanguard_locomotion.vanim` (Idle, Walk and Run blended by `speed`, then Jump, Fall, Land and Interact states), fed by a few lines in the player script. Window > Animation (or the command palette) opens the Animation window for the selected Animator: it lists the controller's states, the cross-fade progress, the current clip weights and every parameter; press Preview to run the controller in the edit scene, then drag sliders, tick bools, fire triggers or click a state to jump to it (End Preview restores the pose; previews also end on save, play and scene change). Clips that travel can drive the character instead: mark a state `"root_motion": true` and the root node's horizontal travel and turn move the Animator's entity (or, with `apply_root_motion` unticked, are reported by `entity:root_motion()` for the player script to pass to `move_character`). The full format is in [Animation controllers](ANIMATION.md).

### Sprite animation

Sprite animation: a `.vsheet` sprite sheet slices one texture into a grid (`columns`, `rows`) and names clips — `{"frames": [0, 1, 2, 1], "fps": 6}` or `{"from": 4, "to": 7, "fps": 12, "loop": false}`. Right-click a texture in the editor's Assets panel and choose "Create Sprite Sheet...", then double-click the sheet to animate the selected entity (a `SpriteAnimator` plays the clip on its Sprite; frames preview while editing). Scripts call `entity:play_sprite("run")`, `entity:sprite_animation()` (clip, frame, finished, ...) and `entity:sprite_clips()`. A Sprite's `uv` field draws one region of a texture by hand.

### Tilemaps

Tilemaps: a `.vtileset` tile set slices a texture into a grid, marks tiles as `solid` or `one_way` (default `collision` plus per-tile `tiles` overrides) and defines autotiling `terrains` — 16 tiles indexed by which side neighbours share the terrain (north 1, east 2, south 4, west 8). Add a `Tilemap` component that names the tile set; painted cells save with the scene, draw through the sprite batch and become merged static 2D colliders that the 2D character mover stands on. Scripts edit maps at run time with `entity:set_tile(x, y, tile|nil)`, `tile(x, y)`, `fill_tiles(x0, y0, x1, y1, tile|nil)`, `clear_tiles()`, `paint_terrain(x, y, "ground"|nil)`, `world_to_tile(point)`, `tile_to_world(x, y)` and `tile_bounds()`; `victus check` validates tile sets. In the editor, right-click a texture → Create Tile Set..., double-click the tile set to give an entity a Tilemap, then paint in a 2D viewport with the Tile Palette (B paint, E erase, R rectangle, T terrain, I pick, X/Y flip; Alt-drag pans; one undo step per stroke).

### Pixel art

Pixel art: right-click a texture and choose "Import Settings..." → Filter "nearest" to keep it crisp (sprites, 2D particles and materials using it sample point-filtered), and tick "Pixel Snap" on the 2D camera so the view moves in whole screen pixels.

Cutout foliage and fences: for a texture used by an alpha-tested material (`alpha_cutoff` > 0), tick "Preserve alpha coverage" in its Import Settings and set the same cutoff (`.meta` `"alpha_coverage_cutoff": 0.5`). The importer rescales the alpha of each mip so the same share of texels passes the alpha test as in the full-size image, and leaves and railings no longer fade away as the camera moves back or zooms out. glTF models do this automatically for the base colour texture of `MASK` materials. Very thin (one-texel) features can still erode at a distance; that is a limit of alpha testing itself.

Texture sharpness at grazing angles: floors, roads and terrain seen at a shallow angle use anisotropic filtering, set by the rendering quality: Low 2x, Medium 4x, High 8x, Ultra 16x. Higher levels keep distant ground textures sharp instead of blurring them to a flat colour, at a small texture-bandwidth cost. Materials with nearest filtering (pixel art) are never filtered this way.

Overdraw view: View > Overdraw View (or "View: Toggle Overdraw View" in the command palette) replaces the 3D viewport with a heat map of how many surfaces are drawn into each pixel: black none, blue one, green two, yellow three, orange four, red five to seven, white eight or more. Opaque, cutout and transparent meshes are all counted, so dense foliage, stacked glass and overlapping walls show up as hot areas. It counts layers before depth testing, so it shows where geometry piles up rather than the exact shading cost; particles and decals are not counted. The setting stays in the editor and is not saved with the project.

### Save games

Save games: scripts store progress and settings with `save.write("profile", { volume = 0.8 })` / `save.read("profile")`, and capture the whole game with `save.checkpoint("quick")` / `save.restore("quick")`; each script keeps its own data across a checkpoint through `function M:save_state()` / `function M:load_state(state)`. Rigid bodies (3D and 2D) keep their linear and angular velocity across a checkpoint; character controllers, animation, particle and audio playback restart from rest. Files are checksummed, written atomically and recovered from a backup if damaged; raise `save_version` in `victus.project` and register `save.migrate(old_version, fn)` when the saved data changes shape. The 2D and 3D templates quick save with F5 (gamepad LB) and quick load with F9 (RB). Exported games keep saves in the per-user VICTUS folder; editor play mode, `victus run` and `victus simulate` use the project's `.victus/saves` (`--saves <dir>` overrides).

### Hot reload

Hot reload: while `victus run` (unless `--no-hot-reload`) or the editor is running, saving a texture, model (or its `.meta` settings, `.bin` or textures) or audio clip under `assets/` swaps it in within about a second, and scripts reload as before. If a file is saved half-way and cannot be imported, the previous version keeps running and the error is shown. Material (`.vmat`) and surface shader (`.vshader`) edits reload too. Scene and font changes appear the next time the game or project starts; the built-in shaders are part of the engine and are not reloaded.

### Material instances

Material instances: in the Material Editor, Create Instance makes a material that inherits everything from the open one; change only what differs (the `x` next to an overridden field goes back to the parent's value, Detach makes it standalone). In the file this is `"parent": "<uuid>"` plus just the overridden keys, and parents can themselves be instances.

### Surface shaders

Surface shaders: a `.vshader` file holds one GLSL function, `void surface(SurfaceInput i, inout Surface s)`, that runs per pixel before VICTUS's lighting, shadows and post-processing. Read `i.uv`, `i.mesh_uv`, `i.world_position`, `i.view_dir`, `i.vertex_normal`, `i.time` (seconds) and `i.params[0..3]`; change `s.base_color`, `s.alpha`, `s.normal`, `s.metallic`, `s.roughness`, `s.emissive`, `s.occlusion` or `s.unlit`; the material's textures can be sampled again with `texture(u_base_color, i.uv)` (also `u_metal_rough`, `u_normal_map`, `u_emissive`). Create one with Assets > New Surface Shader (or New in the Material Editor's Shader section), pick it for a material there and set its four `params` rows. Shaders compile when first used (glslang/SPIRV-Cross); an error is shown with its file and line, and the mesh keeps drawing with the built-in shader until you fix it. `victus check` and `victus export` compile every shader. The 3D template's spinning cube uses `shaders/energy.vshader`.

## Shipping

Before shipping, `victus check <project>` validates every scene, prefab, material, shader, sprite sheet, tile set, terrain, behaviour tree, sequence, animation controller and script and exits 1 on errors (warnings, such as unknown components or clips a model lacks, do not fail it). Tools and the editor get the same checks as structured diagnostics (severity, category, file, entity, suggested fix) from `player::validate_project` in `victus/player/validate.hpp`.

Ship a game with `victus export` (see [Getting started](GETTING_STARTED.md#export-a-game)); the result runs anywhere the player runs, with no editor or SDK installed. Your project files are plain JSON and assets you own; Apache-2.0 allows commercial use.

### Crash reports and diagnostics

If the editor, the player or `victus` crashes, it writes a plain-text crash report (reason, stack frames, the last log lines) to the per-user diagnostics folder and prints where it is; each run also keeps a session log there. `victus diagnose [project] --out bug.zip` collects the newest reports and logs, system facts and, with a project, `victus.project` and the `victus check` results into one zip for a bug report. Home folder, user name and project path are replaced by `<home>`, `<user>` and `<project>`; `--keep-paths` turns that off and `--no-logs`, `--no-crashes`, `--no-system`, `--no-project`, `--no-validation` leave sections out. Nothing is uploaded. See [Diagnostics](DIAGNOSTICS.md).

For Visual Studio/Xcode generators, binaries are under `build/bin/<Config>/`. See [CMake presets](CMakePresets.json).

