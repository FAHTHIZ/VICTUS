# Animation controllers (`.vanim`)

An animation controller is a state machine that decides which clips an `Animator` plays and how they blend. States play one clip or a blend space of clips; transitions move between states when parameter conditions hold, with a cross-fade. Scripts set the parameters (`speed`, `grounded`, `jump`, ...) and the controller does the rest. Decision record: ADR-0068 in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

Point an `Animator` at a controller with its `controller` field (a path relative to `assets/`, for example `anim/player.vanim`). The controller names clips; they resolve against the Animator's model when it plays, so one controller can drive every model with clips of those names. While a controller is set, the Animator's `clip` field shows the current state, and writing a state name to it (or calling `play`) enters that state.

The 3D and Multiplayer templates ship a complete example, `assets/anim/characters/vanguard_locomotion.vanim`, shared by both Vanguard starter characters ([CHARACTERS.md](CHARACTERS.md)): a `Locomotion` blend of Idle/Walk/Run by `speed`, a one-shot `Jump` that hands over to a looping `Fall` while `grounded` is false, a `Land` one-shot on touch-down and an `Interact` one-shot from the `interact` trigger.

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
| `root_motion` | `true` makes the state move the character instead of animating its root node in place (see Root motion). Default `false`. |

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

### Root motion

Clips that travel (a lunge, a dodge roll, a walk authored moving forward) can move the character by exactly the distance the animation covers instead of sliding by script. Mark the state `"root_motion": true` and, if the moving node is not the model's first root node, name it with a top-level `"root_node": "Hips"`.

While a root-motion state plays, that node's horizontal (X/Z) travel and its turn about the vertical axis are removed from the pose (it stays at its position and heading at the start of the clip) and applied to the Animator's own entity instead, in its parent's space and scaled by its transform. Vertical motion (bobbing, a jump arc) stays in the pose. Loops keep moving forward across the wrap, blend spaces blend the travel with their weights, and a cross-fade blends the travel of both states. States without `root_motion` keep animating the root in place.

When the Animator sits on a child of a physics character (the 3D template's `Robot` under `Player`), moving the child would separate it from its collider. Untick `apply_root_motion` on the Animator and let the player script apply the motion through the character controller:

```lua
-- Animation runs once per frame, character moves in fixed updates: turn the frame's travel into
-- a velocity and keep using it until the next frame.
function Player:update(dt)
    local m = self.robot:root_motion()          -- world-space travel of the latest animation update
    self.root_velocity = (m.active and dt > 0) and m.position / dt or nil
end

function Player:fixed_update(dt)
    if self.root_velocity then self.entity:move_character(self.root_velocity) end
end
```

`entity:root_motion()` returns `{active, applied, position, yaw}`; `active` is false when no root-motion state contributed. A full 360° turn inside one clip cannot be represented (each cycle's turn is taken as the shortest angle).

## Errors

`victus check` and `victus export` parse every `.vanim` and fail on the first problem in each, naming its element (`transitions[2].conditions[0]: unknown parameter "speeed"`). `victus check` also warns when a controller names a clip the Animator's model lacks, or takes root motion from a node it lacks. At run time a controller that fails to load is logged once and the Animator falls back to playing its `clip` directly; a missing clip is logged once and contributes the rest pose.

## Runtime API

C++ (`victus/animation/animation.hpp`): `AnimationSystem::set_parameter`, `set_trigger`, `reset_trigger`, `parameter`, `controller_status` (state, previous state, fade progress, parameter values, current clip weights), `play` (enters a state by name), `status` (`controlled`, `state`, `normalized_time`), `add_controller`, `reload_controllers` `update_entity(scene, entity, dt)` (advances one Animator only; the editor preview uses it) and `root_motion(scene, entity)` (`RootMotion{active, position, yaw, applied}` of the latest update). `ControllerStatus` lists `root_motion_states` and the resolved `root_node`. Hosts give `AnimationSettings::controllers` a loader; `animation::directory_loader(assets_root)` reads from the project's assets folder.

Luau (on an entity with an `Animator`; all return `false`/`nil` until the animation system has run on the scene and the controller loaded):

| Call | Result |
| --- | --- |
| `entity:set_anim_param(name, value)` | `true` when set. `value` is a number or boolean; `int` parameters round, `bool` and `trigger` treat non-zero as true. |
| `entity:anim_param(name)` | The value: a number for `float`/`int`, a boolean for `bool`/`trigger`, `nil` when there is no such parameter. |
| `entity:anim_trigger(name)` / `entity:reset_anim_trigger(name)` | Sets or clears a trigger (`false` for non-trigger parameters). The transition that fires on it consumes it. |
| `entity:anim_state()` | The current state's name, or `nil`. |
| `entity:anim_controller()` | `{valid, controller, error, state, previous_state, normalized_time, blend, states, parameters = {name = value}, types = {name = "float"\|"int"\|"bool"\|"trigger"}, weights = {{clip, weight, missing}}}` |
| `entity:root_motion()` | `{active, applied, position, yaw}`: world-space travel and turn of the latest update of a root-motion state (see Root motion). |
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

## Layers

`"layers"` adds state machines on top of the controller's own (the base layer). Each layer shares the controller's parameters and is applied in order after the base, before inverse kinematics (ADR-0074).

```json
"layers": [
  { "name": "UpperBody", "mask": ["Spine"], "weight_parameter": "aiming",
    "default_state": "None",
    "states": [ { "name": "None" }, { "name": "Shoot", "clip": "Shoot", "loop": false } ],
    "transitions": [
      { "from": "None", "to": "Shoot", "duration": 0.1, "conditions": [ { "parameter": "fire" } ] },
      { "from": "Shoot", "to": "None", "duration": 0.2, "exit_time": 1 } ] },
  { "name": "Breathing", "blend": "additive", "weight": 0.6,
    "states": [ { "name": "Breathe", "clip": "Breathe" } ] }
]
```

| Field | Meaning |
| --- | --- |
| `name` | Unique layer name (scripts set weights by it) |
| `weight` | 0..1, default 1; `entity:set_anim_layer_weight(name, w)` changes it at run time |
| `weight_parameter` | Optional float/int parameter, clamped to 0..1, multiplied into the weight |
| `blend` | `override` (default): replaces the masked nodes the layer's clips animate. `additive`: adds the layer clip's change from its reference pose |
| `reference` | Additive only: `first_frame` (default; each state's pose at normalized time 0, as in Unity) or `rest` (the model's rest pose) |
| `mask` | Node names; each includes its descendants. Empty or absent: every node |
| `exclude` | Node names whose subtrees the layer never touches. The nearest listed ancestor decides, so `"mask": ["Spine"], "exclude": ["LeftArm"]` covers the spine and right arm |
| `default_state`, `states`, `transitions` | As at the top level. A layer state with no `clip` or blend space is empty: the layer has no influence while it is there, and fades out of and into it blend with the base pose |

Layers cannot declare parameters, nested layers, `root_node` or root-motion states; errors name the element (`layers[0].transitions[1]: unknown state "Shot"`). A trigger consumed by any layer's transition is consumed for all. Nodes outside the mask, and nodes the layer's clips do not animate, keep the pose from the layers below. `victus check` checks layer clips like the base layer's. Unknown mask node names are logged once and ignored.

Luau: `entity:set_anim_layer_weight(name, weight)` → `true` when the layer exists, `entity:anim_layer_weight(name)` → number or `nil`, and `entity:anim_controller().layers` → `{{name, blend, weight, effective_weight, state, previous_state, normalized_time, blend_progress}}`. Visual scripts: Set Anim Layer Weight. C++: `AnimationSystem::set_layer_weight`, `layer_weight`, `ControllerStatus::layers`.

## Inverse kinematics

An `IKConstraint` component on the end of a chain (a hand, foot or head) bends its ancestors toward a target after the clips and controller have been sampled, every frame, before transforms propagate (ADR-0073).

| Field | Meaning |
| --- | --- |
| `solver` | `two_bone` (parent and grandparent, analytic: arms, legs), `chain` (`chain_length` ancestors, FABRIK: tails, spines, tentacles) or `look_at` (rotates this entity so `aim_axis` points at the target, at most `max_angle`) |
| `enabled`, `weight` | Off, or a 0..1 blend with the animated pose |
| `target` / `target_position` | An entity to follow, otherwise a world position |
| `pole` | Optional entity the middle joint bends toward (two_bone) |
| `match_rotation` | Also copy the target entity's rotation to the end |
| `iterations`, `tolerance` | FABRIK limits |
| `max_extension` | Two-bone reach fraction (0.999 avoids a locked straight knee) |

Unreachable targets stretch the chain straight toward them. Chains are solved parents first, so a spine chain can feed an arm's two-bone constraint.

Visual scripts: Set IK Target. Luau: `entity:set_ik_target(vector | entity | nil, weight?)` (nil disables) and `entity:ik_state()` → `{enabled, weight, solver, target, reached}`. The fields are also settable through `entity:set("IKConstraint", field, value)`.

```lua
-- Foot placement: plant each foot on the ground under it.
function Feet:update(dt)
    for _, foot in self.feet do
        local p = foot.world_position
        local hit = physics.raycast(p + vector.create(0, 0.5, 0), vector.create(0, -1, 0), 1.2)
        foot:set_ik_target(hit and hit.point or nil)
    end
end
```

## Editor

Window > Animation opens the Animation window for the selected entity's Animator. It shows the controller path, the states (the current one highlighted with its normalized time; click one to enter it), the previous state and fade progress during a cross-fade, the current clip weights as bars (missing clips flagged) and a control per parameter: float sliders spanning the blend thresholds, int drags, bool checkboxes and trigger buttons. Preview runs the controller on the edit scene through `AnimationSystem::update_entity`; Pause and Restart control it, End Preview restores the Animator and the skeleton transforms. A preview is ended (and restored) before saving, autosave skips while one runs, and play mode, scene switches and closing the project end it. Root-motion states are tagged `[root motion]`, and the window shows the root node and how far it moves per frame. Controller edits hot reload into the preview. `victus_editor <project> --select <entity> --show animation-preview` opens it from the command line.
