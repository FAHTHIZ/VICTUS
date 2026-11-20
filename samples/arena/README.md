# Robot Arena (VICTUS sample)

A third-person 3D arena fight built only from engine features. Hostile robots walk out of the
four corner gates in five growing waves (3, 4, 5, 6 and 7 robots) and chase you over the
navmesh. Swing at them to break them apart, pick up the green health orbs some of them drop, and
clear every wave before your health runs out.

```sh
victus run samples/arena                 # play
victus_editor samples/arena              # open it in the editor
victus simulate samples/arena --scene scenes/autoplay.vscene --frames 12000 --seed 1 --save final.vscene
```

`scenes/autoplay.vscene` is the same game with the player's `autopilot` property on: it walks
navmesh paths (`nav.find_path`) to the nearest robot, or to a health orb when hurt, and swings
when a robot is in reach. The `victus_sample_arena` CTest runs it headless and checks that the
final scene shows "Arena cleared!" and "Wave 5 / 5" (seed 1 clears it in about 70 s of game
time, 8500 points).

## Controls

| Action | Keyboard / mouse | Gamepad |
| --- | --- | --- |
| Move (camera relative) | WASD or arrows | left stick / d-pad |
| Sprint | Left Shift | left stick press |
| Swing | left mouse or J | X / right trigger |
| Turn the camera | drag with the right mouse button | right stick |
| Jump | Space | A |
| Quick save / quick load | F5 / F9 | LB / RB |
| Restart after a win or loss | R | Start |
| Show / hide the controls panel | H | View |

## What it uses

| Engine feature | Where |
| --- | --- |
| Skinned starter characters with `Idle`/`Walk` clips cross-faded from scripts (`play_animation`) | Orion is the player, red-tinted Nova copies are the enemies: `assets/models/characters/` |
| `CharacterController` movement, jumping, sliding along walls and cover | `Player`, `assets/scripts/player.luau` |
| Navmesh baked from the static level, `NavAgent` steering with avoidance, `nav.find_path` | enemy robots, autopilot |
| Prefabs instantiated at run time (`scene.instantiate`) | `assets/prefabs/enemy_robot.vprefab`, `health_orb.vprefab` |
| Shared script state through `require` | `assets/scripts/arena_state.luau` |
| Orbit camera with layer-masked raycasts that pull in behind pillars | `assets/scripts/third_person_camera.luau` |
| PBR materials, a custom surface shader for glowing gates and orbs, point and directional lights with shadows, fog, bloom | `assets/materials`, `assets/shaders/energy.vshader`, `scenes/main.vscene` |
| Particle bursts (`emit_particles`) for swings and destroyed robots; emissive hit flashes (`set_emissive`) | `Slash`, `Hit Burst` |
| Positional audio (`audio.play`) | `assets/audio/impact.wav` |
| Game UI: status bar, wave banner, controls panel, save toast | `HUD`, `assets/scripts/hud.luau` |
| Checkpoints: every script's `save_state`/`load_state`, entity references stored by id | F5 / F9 |
| Input actions with keyboard, mouse and gamepad bindings | `victus.project` |
| Seeded `math.random` for reproducible runs | `--seed` |

## How it is built

The game rules are four hand-written Luau scripts in `assets/scripts`. Everything else (level,
prefabs, scenes, project file, the copied template art) comes from `tools/generate_arena.py`,
which takes a fresh 3D template project and rebuilds the sample deterministically:

```sh
victus new /tmp/arena_template --template 3d
victus check /tmp/arena_template              # writes the template's .meta files
python3 samples/arena/tools/generate_arena.py /tmp/arena_template samples/arena
```

## Art

The robot model, ground texture, energy shader, impact sound and HUD script come from the
engine's 3D project template and are part of this repository under the same licence
(Apache-2.0). The arena itself is made of built-in primitives.

## Limitations

- No title menu or cinematic sequence yet; the game starts straight into wave 1.
- Enemy robots are driven by the arena script rather than a behaviour tree or visual logic.
- Melee hits are resolved with distance and angle tests, not physics hit boxes.
