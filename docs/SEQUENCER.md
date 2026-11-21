# Timeline sequencer

`engine/sequencer` (`VICTUS::Sequencer`, `victus/sequencer/sequencer.hpp`) plays timelines
(`.vseq` sequences) for cutscenes, scripted doors and lifts, intros and camera moves. The player,
the CLI `simulate`/`run` commands, editor play sessions and in-editor clients all run a
`SequencerSystem` after scripts and AI.

## Component

| Component | Fields | Purpose |
|---|---|---|
| `SequencePlayer` (category Cinematics) | `sequence` (asset path, e.g. `seq/intro.vseq`), `play_on_start`, `loop`, `speed` (negative plays backwards), `enabled` | Plays a sequence; track targets resolve below this entity |

An empty `sequence` is allowed: scripts assign one with `entity:play_sequence(path)`.

## Sequence files (`.vseq`)

JSON with an optional `version` (1), an optional `duration` in seconds (defaults to the last key
time) and a `tracks` array. Every track has a `type`, an optional `name`, `target` and `muted`.

```json
{
  "duration": 4,
  "tracks": [
    { "type": "transform", "target": "Door", "space": "relative",
      "position": [ { "t": 0, "v": [0, 0, 0] }, { "t": 2, "v": [0, 3, 0], "interp": "smooth" } ],
      "rotation": [ { "t": 0, "v": [0, 0, 0] }, { "t": 2, "v": [0, 90, 0] } ] },
    { "type": "animation", "target": "Robot", "keys": [ { "t": 0.5, "clip": "Wave", "fade": 0.2, "loop": false } ] },
    { "type": "audio", "target": "Door", "keys": [ { "t": 0, "clip": "audio/door.wav", "bus": "Sfx", "spatial": true } ] },
    { "type": "camera", "keys": [ { "t": 0, "camera": "Cams/Wide" }, { "t": 2.5, "camera": "Cams/Close" } ] },
    { "type": "event", "keys": [ { "t": 3, "name": "door_open", "payload": "north" } ] }
  ]
}
```

| Track | Keys | Behaviour |
|---|---|---|
| `transform` | `position`, `rotation`, `scale` channels of `{t, v, interp}`; rotation `v` is Euler degrees `[pitch, yaw, roll]` or a quaternion `[x, y, z, w]` | Sampled every frame into the target's local Transform. `space: "relative"` adds position, multiplies rotation and scale onto the transform the target had when playback started |
| `animation` | `clip`, `fade` (s, default: the Animator's crossfade), `speed`, `loop` | Plays the clip on the target's Animator when the playhead crosses the key |
| `audio` | `clip`, `volume`, `pitch`, `bus` (Master/Music/Sfx/Ui/Voice/Ambient), `spatial` | Fires a one-shot (at the target's position when spatial) |
| `camera` | `camera` (target path) | Camera cuts: the latest cut at or before the playhead is the only primary camera; the original flags are restored when playback stops, finishes or the player is removed |
| `event` | `name`, `payload` | Calls `on_sequence_event` in scripts and the C++ event handler |
| `property` | `component`, `property` and `keys` of `{t, v, interp}` where `v` is a number or an array of 2-4 numbers (the same arity on every key) | Sampled every frame into one field of the target's component (see Property tracks) |

Interpolation (`interp`, from a key to the next): `step`, `linear` (default; quaternions slerp),
`smooth` (ease in/out), `cubic` (Catmull-Rom through neighbouring keys) and `bezier` (cubic Hermite
using the key's tangents; rotations treat `cubic` and `bezier` as `smooth`).

### Tangents (bezier keys)

A `bezier` segment leaves its key with the key's out slope and arrives at the next key with that
key's in slope (value units per second, per component). `tangent` chooses the slopes:

| `tangent` | Slopes |
|---|---|
| `auto` (default, omitted when saving) | From the neighbouring keys, smooth through the key (shared in/out slope), flat at peaks, plateaus and the first and last key, limited so the curve never overshoots between keys (monotone Fritsch-Carlson). Moves with the keys |
| `flat` | Zero on both sides |
| `user` | `out` on both sides (handles kept in line). Needs `"out": [x, y, z]` (a number for 1-channel property tracks) |
| `broken` | `in` arriving and `out` leaving, independently. Needs `in` and `out` |

```json
{ "t": 1, "v": [0, 2, 0], "interp": "bezier", "tangent": "broken", "in": [0, 4, 0], "out": [0, -1, 0] }
```

Tangent fields are written only for `user`/`broken` keys, so older sequences round-trip unchanged.

### Property tracks

```json
{ "type": "property", "target": "Lamp", "component": "Light", "property": "intensity",
  "keys": [ { "t": 0, "v": 0 }, { "t": 2, "v": 12, "interp": "bezier" } ] }
```

Animatable properties (`sequencer::animatable_properties(scene, entity)` lists what an entity has):

- Built-in components: `Camera.fov_y`, `Camera.ortho_height`; `Light.color` (4), `intensity`,
  `range`, `inner_angle`, `outer_angle`; `MeshRenderer.base_color` (4), `metallic`, `roughness`;
  `Sprite.tint` (4), `size` (2).
- Every reflected component (`scene::Schema`, including gameplay, physics and script-registered
  ones): Float, Vec2, Vec3 and Color fields, plus Int/UInt (rounded) and Bool (on above 0.5)
  fields. Values clamp to the field's declared range.

A track whose target lacks the component or field is warned once and skipped (counted in
`stats().missing_targets`). Like transforms, property values keep their last sampled value when
playback stops; the Studio preview restores the values it changed.

Targets: `""` or `.` is the player entity, `A/B` walks children by name (`..` goes up), otherwise
the first descendant with that name, otherwise any entity in the scene with that name. Missing
targets are warned once per player and track.

Limits: 512 tracks, 65536 keys per channel. Parse errors name the track and key, e.g.
`sequence: tracks[1].keys[0]: needs a "clip"`. `victus check` and export validate every `.vseq` and
every `SequencePlayer` reference.

## Playback semantics

- Discrete keys (animation, audio, events, camera cuts) fire once when the playhead crosses them;
  a key exactly at the start fires on the first update. Reverse playback fires them in reverse
  order. Looping fires the keys of each pass (at most 8 wraps per update).
- `seek` moves the playhead and applies transforms, the camera cut and the active animation clip
  (at the matching clip time) without firing events or audio, so it can be used to preview or skip.
- `stop` rewinds and undoes camera cuts; transforms keep their current values.
- A finished sequence holds its last frame. Hot reload (`.vseq` edits during play) keeps the
  playhead of every player using the changed file.

## Scripting

```lua
function Cutscene:start()
    self.entity:play_sequence("seq/intro.vseq")
end
function Cutscene:on_sequence_event(name, payload, player)
    if name == "door_open" then print("open", payload) end
end
function Cutscene:update(dt)
    local st = self.entity:sequence_state()
    if st and st.finished then self.entity:stop_sequence() end
end
```

`entity:play_sequence(path?) -> bool`, `entity:stop_sequence()`, `entity:pause_sequence(paused?)`,
`entity:seek_sequence(t) -> bool`, `entity:sequence_state() -> {valid, sequence, error, time,
duration, playing, paused, finished, loops, camera}` (nil without a sequencer) and
`sequencer.available()`. Event keys call `on_sequence_event(name, payload, player)` on the track
target's script and then on the player's script.

## C++

```cpp
sequencer::SequencerSystem seq(sequencer::directory_loader(assets_dir));
sequencer::add_sequencer_systems(sim, seq);   // PreUpdate bind, Update playback
seq.set_event_handler([](scene::Scene&, const sequencer::SequenceEvent& ev) { /* ... */ });
seq.play(scene, entity, "seq/intro.vseq");
seq.seek(scene, entity, 1.5f);
auto doc = sequence->to_json();               // deterministic; Sequence::parse round-trips it
```

`set_sequence(scene, entity, sequence)` plays an in-memory sequence (editor previews, tests).

## Editor

Window > Sequencer (or the command palette entry "Sequencer") opens the timeline panel.

- New creates `sequences/Sequence.vseq`; Open or drop a `.vseq` from the asset browser onto the panel. Assign attaches the open sequence to the selected entity's `SequencePlayer` (undoable) and uses it as the preview player.
- + Track adds transform, animation, audio, camera-cut or event tracks. The track context menu sets the target entity name, toggles relative (offset) transforms and removes the track.
- Click or drag the ruler to scrub. Play previews in edit mode through `SequencerSystem::seek`; stopping or closing the preview restores the transforms, primary camera and animator state it touched. Saving the scene, switching scenes and entering play mode end the preview first, and autosave pauses during preview.
- Double-click a lane to add a key, drag diamonds to retime them, Delete Key removes the selection. Record Key captures the target's current transform at the playhead (as an offset for relative tracks). The key inspector edits per-type values; audio clips can be dropped from the asset browser.
- + Track > Property lists the animatable properties of the selection (or the player) grouped by component. Record Key and double-clicking a property lane key the target's current value; the key inspector edits the value (a colour picker for colours), interpolation and tangent mode with its slopes.
- Curves (toolbar toggle) swaps the lanes for a curve editor of one transform (position and scale x/y/z) or property track, chosen from its track list. Drag keys to change time and value, drag the tangent handles of the selected key to set its slopes (an auto or flat key becomes user; hold Alt to break it), double-click a curve to insert a key that keeps its shape, right-click a key for interpolation, tangent mode and Delete. The mouse wheel zooms values (Shift: time), middle-drag pans, Frame or F fits the visible curves. Every drag is one undo step and the preview follows the edit. Rotation keys stay in the timeline (quaternions have no per-component curves).
- Sequence edits have their own undo/redo stack; Save writes deterministic JSON via `Sequence::to_json`. Changes made outside the editor reload the open sequence unless it has unsaved edits.
