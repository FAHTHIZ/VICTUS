# Animation controllers (`.vanim`)

An animation controller is a state machine that decides which clips an `Animator` plays and how they blend. States play one clip or a blend space of clips; transitions move between states when parameter conditions hold, with a cross-fade. Scripts set the parameters (`speed`, `grounded`, `jump`, ...) and the controller does the rest. Decision record: ADR-0068 in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

Point an `Animator` at a controller with its `controller` field (a path relative to `assets/`, for example `anim/player.vanim`). The controller names clips; they resolve against the Animator's model when it plays, so one controller can drive every model with clips of those names. While a controller is set, the Animator's `clip` field shows the current state, and writing a state name to it (or calling `play`) enters that state.

## File format

```json
{
  "version": 1,
  "parameters": [
    { "name": "speed", "type": "float" },
    { "name": "grounded", "type": "bool", "default": true },
    { "name": "jump", "type": "trigger" }
  ],
  "default_state": "Locomotion",
  "states": [
    { "name": "Locomotion", "blend1d": { "parameter": "speed",
        "points": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.6 }, { "clip": "Run", "at": 5 } ] } },
    { "name": "Jump", "clip": "Jump", "loop": false }
  ],
  "transitions": [
    { "from": "*", "to": "Jump", "duration": 0.1, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Jump", "to": "Locomotion", "duration": 0.2, "exit_time": 0.7,
      "conditions": [ { "parameter": "grounded", "op": "true" } ] }
  ]
}
```

### Parameters

| Field | Meaning |
|---|---|
| `name` | Unique name used by blend spaces, conditions and scripts. |
| `type` | `float` (default), `int` (rounded when set), `bool`, or `trigger` (set until a transition that tests it fires, then cleared). |
| `default` | Starting value: a number, or `true`/`false` for bool and trigger. |

Parameter values are runtime state: they are not saved with the scene, and a loaded scene starts every controller with its defaults. Hot reloading a controller keeps values whose names and types still exist.

### States

| Field | Meaning |
|---|---|
| `name` | Unique state name (`*` is reserved). |
| `clip` | Plays one clip. |
| `blend1d` | `{ "parameter": "<float or int>", "points": [ { "clip", "at", "speed"? } ] }`. Weights are linear between the two thresholds around the parameter and clamp at the ends. Thresholds must be distinct. |
| `blend2d` | `{ "x": "<param>", "y": "<param>", "points": [ { "clip", "at": [x, y], "speed"? } ] }`. Gradient-band weights (see below). |
| `speed` | Playback rate of the state (default 1, must be ≥ 0). The Animator's `speed` multiplies it. |
| `speed_parameter` | Optional float/int parameter multiplied into the rate (for example a walk cycle that speeds up with movement). |
| `loop` | `true` (default) loops; `false` holds the last frame and marks the state finished. |

A state needs exactly one of `clip`, `blend1d` or `blend2d`. Blend spaces are phase-synced: every clip in the space plays at the same normalized time, and one cycle lasts the weighted sum of the clips' durations (each divided by its point `speed`), so a walk and a run blended together keep their footfalls aligned.

### Transitions

| Field | Meaning |
|---|---|
| `from` | Source state, or `*` for any state. Any-state transitions are checked first, then the current state's, each in file order; the first that can fire wins (one per frame). |
| `to` | Destination state. |
| `duration` | Cross-fade in seconds (default 0.2; 0 cuts). |
| `exit_time` | Optional normalized time of the source state (1 = one cycle). On looping states an exit time below 1 is checked every cycle on the frame that point is crossed; otherwise it holds once reached, and the conditions may become true later. Non-looping states cannot use an exit time above 1. |
| `offset` | Normalized start time in the destination state, 0–1 (default 0). |
| `allow_self` | Any-state transitions only: may restart the state they lead to. Otherwise an any-state transition never re-enters the current state. |
| `conditions` | All must hold. `{ "parameter", "op", "value" }` with `op` `true`/`false` for bools, `trigger` for triggers (the default for both), and `>`, `>=`, `<`, `<=`, `==`, `!=` with a `value` for float/int parameters. |

A transition without conditions must have an `exit_time`, and an any-state transition must have at least one condition. A transition that fires during a cross-fade freezes the current blended pose and fades from that snapshot into the new state, so interruptions never pop.

### Blend space weights

1D: for thresholds \(t_i < t_{i+1}\) around the parameter value \(x\), the weights are \(1 - f\) and \(f\) with \(f = (x - t_i)/(t_{i+1} - t_i)\).

2D: gradient-band interpolation in Cartesian space (Johansen 2009, sec. 6.3; the "Freeform Cartesian" mode of Unity). For point \(p_i\) and sample \(p\):
\[ h_i = \max\Big(0,\ \min_{j \ne i} \Big(1 - \frac{(p - p_i)\cdot(p_j - p_i)}{\lVert p_j - p_i \rVert^2}\Big)\Big), \qquad w_i = h_i / \textstyle\sum_k h_k. \]
Each point gets the full weight at its own position, weights change smoothly, and they sum to 1 everywhere (outside the points too). Coincident points share their weight.

Poses blend as weighted sums of translation and scale and a sign-aligned, renormalized sum of rotations.

## Errors

`victus check` and `victus export` parse every `.vanim` and fail on the first problem in each, naming its element (`transitions[2].conditions[0]: unknown parameter "speeed"`). `victus check` also warns when a controller names a clip the Animator's model lacks. At run time a controller that fails to load is logged once and the Animator falls back to playing its `clip` directly; a missing clip is logged once and contributes the rest pose.

## Runtime API

C++ (`victus/animation/animation.hpp`): `AnimationSystem::set_parameter`, `set_trigger`, `reset_trigger`, `parameter`, `controller_status` (state, previous state, fade progress, parameter values, current clip weights), `play` (enters a state by name), `status` (`controlled`, `state`, `normalized_time`), `add_controller` and `reload_controllers`. Hosts give `AnimationSettings::controllers` a loader; `animation::directory_loader(assets_root)` reads from the project's assets folder.

Luau (on an entity with an `Animator`; all return `false`/`nil` until the animation system has run on the scene and the controller loaded):

| Call | Result |
| --- | --- |
| `entity:set_anim_param(name, value)` | `true` when set. `value` is a number or boolean; `int` parameters round, `bool` and `trigger` treat non-zero as true. |
| `entity:anim_param(name)` | The value: a number for `float`/`int`, a boolean for `bool`/`trigger`, `nil` when there is no such parameter. |
| `entity:anim_trigger(name)` / `entity:reset_anim_trigger(name)` | Sets or clears a trigger (`false` for non-trigger parameters). The transition that fires on it consumes it. |
| `entity:anim_state()` | The current state's name, or `nil`. |
| `entity:anim_controller()` | `{valid, controller, error, state, previous_state, normalized_time, blend, states, parameters = {name = value}, types = {name = "float"\|"int"\|"bool"\|"trigger"}, weights = {{clip, weight, missing}}}` |
| `entity:play_animation(state, fade?)` | With a controller, jumps to a state by name. |
| `entity:animation()` | Also reports `controlled`, `state` and `normalized_time`. |

```lua
function Player:update(dt)
    local speed = vector.magnitude(self.entity:character_state().velocity * vector.create(1, 0, 1))
    self.entity:set_anim_param("speed", speed)
    self.entity:set_anim_param("grounded", self.entity:character_state().grounded)
    if input.action_pressed("jump") then self.entity:anim_trigger("jump") end
end
```

Visual scripts have matching nodes in the Animation category: Play Animation, Set Anim Parameter, Fire Anim Trigger, Anim State and Get Anim Parameter.
