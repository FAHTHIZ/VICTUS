# Animation controllers (`.vanim`)

An animation controller is a state machine that decides which clips an `Animator` plays and how they blend. States play one clip or a blend space of clips; transitions move between states when parameter conditions hold, with a cross-fade. Scripts set the parameters (`speed`, `grounded`, `jump`, ...) and the controller does the rest. Decision record: ADR-0068 in [TECHNOLOGY_DECISIONS.md](TECHNOLOGY_DECISIONS.md).

Point an `Animator` at a controller with its `controller` field (a path relative to `assets/`, for example `anim/player.vanim`). The controller names clips; they resolve against the Animator's model when it plays, so one controller can drive every model with clips of those names. While a controller is set, the Animator's `clip` field shows the current state, and writing a state name to it (or calling `play`) enters that state.

The 3D and Multiplayer templates ship a complete example, `assets/anim/characters/victus_locomotion.vanim`, shared by both starter characters, Orion and Nova ([CHARACTERS.md](CHARACTERS.md)): a `Locomotion` blend of Idle/Walk/Run by `speed`, a one-shot `Jump` that hands over to a looping `Fall` while `grounded` is false, a `Land` one-shot on touch-down and an `Interact` one-shot from the `interact` trigger. The characters carry 41 clips in all (strafes, sprint, starts and stops, turns, crouch, actions, grip poses and five facial clips; the full list is in [CHARACTERS.md](CHARACTERS.md)), so a project's own controller can use more than the template's seven.

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
| `mask` | Node names; each includes its descendants. Empty or absent (and no `mask_weights`): every node |
| `mask_weights` | Weighted mask (ADR-0159): `{ "Spine": 0.3, "Chest": 0.6, "Neck": 1 }` gives each listed subtree that share of the layer weight, so an aim layer can turn the lower spine a little and the chest fully. With no `mask`, the weighted names are the mask. A node may appear in only one of `mask`, `exclude` and `mask_weights` |
| `exclude` | Node names whose subtrees the layer never touches. The nearest listed ancestor decides, so `"mask": ["Spine"], "exclude": ["LeftArm"]` covers the spine and right arm |
| `sync` | `true`: the layer's looping states take the base layer's normalized time instead of their own (ADR-0160), so a limp or carry upper-body cycle stays in step with locomotion at any speed. One-shot states keep their own clock |
| `default_state`, `states`, `transitions` | As at the top level. A layer state with no `clip` or blend space is empty: the layer has no influence while it is there, and fades out of and into it blend with the base pose |

Layers cannot declare parameters, nested layers, `root_node` or root-motion states; errors name the element (`layers[0].transitions[1]: unknown state "Shot"`). A trigger consumed by any layer's transition is consumed for all. Nodes outside the mask, and nodes the layer's clips do not animate, keep the pose from the layers below. `victus check` checks layer clips like the base layer's. Unknown mask node names are logged once and ignored. The Studio Animation window's Layers section shows each layer's state, a weight slider and its mask with the weight of every listed bone.

Luau: `entity:set_anim_layer_weight(name, weight)` → `true` when the layer exists, `entity:anim_layer_weight(name)` → number or `nil`, and `entity:anim_controller().layers` → `{{name, blend, weight, effective_weight, state, previous_state, normalized_time, blend_progress}}`. Visual scripts: Set Anim Layer Weight. C++: `AnimationSystem::set_layer_weight`, `layer_weight`, `ControllerStatus::layers`.

## Twist drivers

A controller can list twist drivers after its layers (ADR-0182). Each makes a twist joint take a
share of another joint's roll about the twist joint's bone axis, so skin weighted across a
forearm twist bone keeps its volume when a clip rolls the wrist instead of collapsing at the
wrist ("candy wrapper"):

```json
"twist": [
  { "joint": "LowerArmTwist.L", "source": "Hand.L", "weight": 0.5 },
  { "joint": "LowerArmTwist.R", "source": "Hand.R", "weight": 0.5 }
]
```

- `source` is a sibling of `joint` (the twist the sibling's rotation adds in the shared parent's
  frame, about the sibling's rest offset: the forearm case) or its parent (the parent's twist
  relative to its own rest, about the joint's rest offset; negative weights counter-rotate).
- `weight` is from -1 to 1 (default 0.5). Only the twist part counts (swing-twist decomposition);
  bends of the source do not move the twist joint.
- Drivers run after the layers and before IK, multiply the twist joint's own pose, and are
  skipped for models without the named joints, so one controller serves skeletons with and
  without twist bones. The starter characters' shared controller drives both forearm twist
  joints at 0.5.

## Skinning methods

`SkinnedMeshComponent::method` picks how joints blend on each vertex (ADR-0206):

- `linear` (default): linear blend skinning. Cheapest; twisted forearms collapse past about
  120 degrees of roll and bent elbows and knees lose some volume.
- `dual_quaternion`: dual quaternion skinning. Keeps the width of twisted limbs and the volume
  of bent joints; slight bulging on the outside of sharp bends; scale is blended per joint.

Both run in every skinned vertex shader (lit, depth, shadow, motion vectors) from the same
64-byte palette entry, so switching needs no reimport. `render/skinning.hpp` also has
`skin_vertices` (the CPU skinner used for fallbacks and diagnostics), `analyze_skin_weights`,
`normalize_skin_weights`, `limit_skin_influences`, `weight_heatmap_colors` and
`measure_deformation` (volume, triangle area extremes, collapsed and flipped triangles).

Measured (docs/STATUS.md, ADR-0206): a tube twisted 170 degrees keeps 100% of its mid width with
DQS against 9% with LBS (3 px against 14 px wide on screen); Orion's body in an extreme pose has 4
collapsed triangles with DQS against 22 with LBS.

## Extending skeletons

`assets/skeleton.hpp` edits cooked skeletons without breaking what already uses them:
`describe_skeleton` lists bones with inferred roles, `apply_skeleton_edits` adds, renames,
reparents and removes bones (transactionally), and `check_skeleton_compatibility` compares the
result with the original. Rules:

- Added bones are appended: every existing node and joint index stays valid. A deforming bone can
  take a share of a parent's weights (`weights_from`, `weight_share`, `weight_start/end` along the
  parent bone), except on vertices already using four influences.
- Reparenting keeps the bone's world rest transform and re-bakes its clip channels.
- Removing a bone hands its weights to the nearest ancestor joint (or a named one) and keeps its
  skin slot (`compact_joints` renumbers instead, which the compatibility check reports).
- Moving (`"op": "move"`, ADR-0208) sets the rest translation, rotation and scale of a socket, IK
  target, pole or helper. A node that deforms, carries a deforming joint or carries meshes is
  refused; a clip that animates the node is reported because it overrides the new rest transform.
- The 21 core starter joints are protected by passing them as `protected_bones`; `force` overrides.

A `SkeletonMigration` is a versioned JSON list of edits (`"format": "victus.skeleton_migration"`)
that names the layout hash it applies to and refuses any other skeleton:

```json
{ "format": "victus.skeleton_migration", "profile": "VictusHumanoid", "from_version": 1, "to_version": 2,
  "requires_layout": "9f3c...", "edits": [
    { "op": "add", "bone": "ForeArmRoll.R", "parent": "LowerArm.R", "translation": [0, 0.12, 0],
      "weights_from": "LowerArm.R", "weight_share": 0.5, "weight_start": 0.2, "weight_end": 1 },
    { "op": "add", "bone": "IK.Hand.R", "parent": "Hips", "deform": false } ] }
```

Using migrations:

- Import: list migration files in the model's `.meta` settings, relative to the model:
  `"skeleton_migrations": ["orion.glb.rig2.vrig"]`. They are applied in order after every import
  (the source file is never modified); editing one re-imports the model, and a file written for
  another layout fails the import with its name in the error.
- Studio: select a character and open the Inspector's "Skeleton and skinning" card: the bone list
  with roles, joints and weighted vertex counts, the skinning method (one undo step), a
  deformation check (collapsed triangles with each method for a chosen roll and bend) and "Add
  child bone", which writes the next `.vrig` migration next to the model and re-imports it.
- Studio viewport (ADR-0208): View > Bones draws the selected character's bones from each bone to
  its parent in the current pose (sockets yellow with a cross, IK targets and poles cyan,
  mechanical orange, face pink, secondary green, the card's selected bone white); View > Skin
  Weights draws the selected bone's weights as blue-to-red ticks on the CPU-skinned vertices
  (at most 4,000). The card's Pose section rotates the selected bone on top of its rest pose
  (one undo step each) and Reset pose restores every bone. For a socket, IK target or helper,
  "Save transform to model" writes its scene transform as a `move` edit in the next `.vrig`;
  moving a deforming joint is refused because it would change the bind pose.
- CLI: `victus skeleton <model|orion|nova> [--migration a.vrig,b.vrig] [--deformation] [--json]`
  prints the bone tree, applies migrations and reports compatibility (exit 1 when broken).

## Reference rigs

`assets/reference_rigs.hpp` (ADR-0207) builds test rigs in code: `make_reference_rig(ReferenceRig::Quadruped)` (a 38-joint creature with a seven-joint tail, jaw, ears, four legs and a jiggle belly; clips Walk, Sit, Shake) and `ReferenceRig::Mech` (a 13-joint loader whose parts are each rigid on one joint; clips Work, Spin). `complex_humanoid_extension()` returns the 19 bone edits that turn Orion or Nova into a more complex humanoid without changing existing deformation. `victus skeleton quadruped --deformation` inspects them like any model.

They are what the deformation tests run on. `gpu_rig_deformation_tests` draws each pose GPU-skinned with linear and dual quaternion palettes and compares the image with the same pose skinned on the CPU and drawn as a static mesh; on Vulkan and GLES the images match pixel for pixel. Measured on them:

| Case | Linear blend | Dual quaternion |
|---|---|---|
| Quadruped Walk / Shake, worst volume change | 0.05% / 0.44% | 0.008% / 0.14% |
| Quadruped Sit (knees bent ~125 degrees), collapsed triangles | 0 | 2 |
| Tail rolled 120 degrees per joint, neck turned, leg twisted: collapsed triangles | 2 | 0 |
| Mech, any pose: triangle area change | 0 | 0 |

So neither method wins everywhere: use dual quaternions where limbs twist (forearms, tails, necks) and linear blend, or corrective shapes, where joints bend hard.

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
| `max_bend` | Chain: largest bend at each joint in degrees (a cone around the bone before it; the first bone around its animated direction). 180 = free. Stops tails and spines folding back on themselves; limited chains may miss a target outside the cones and converge more slowly (ADR-0161) |
| `max_extension` | Two-bone reach fraction (0.999 avoids a locked straight knee) |

Unreachable targets stretch the chain straight toward them. Chains are solved parents first, so a spine chain can feed an arm's two-bone constraint.

Visual scripts: Set IK Target. Luau: `entity:set_ik_target(vector | entity | nil, weight?)` (nil disables) and `entity:ik_state()` → `{enabled, weight, solver, target, reached}`. The fields are also settable through `entity:set("IKConstraint", field, value)`.

```lua
-- Hand-made ground contact (for full foot placement use the FootPlacement component).
function Feet:update(dt)
    for _, foot in self.feet do
        local p = foot.world_position
        local hit = physics.raycast(p + vector.create(0, 0.5, 0), vector.create(0, -1, 0), 1.2)
        foot:set_ik_target(hit and hit.point or nil)
    end
end
```

## Foot placement and foot locking

`FootPlacement` on a character root and `FootContact` on each foot bone (each with a `two_bone` `IKConstraint` that has no target entity) plant the feet on the ground under them and stop planted feet from sliding (ADR-0163). Studio: select a character and press Add Foot Placement on the Inspector's "Feet on the ground" card, or run "Entity: Add Foot Placement"; code: `animation::make_humanoid_foot_placement(scene, root)` (finds the Hips/Pelvis bone and both feet; returns the number of feet).

| `FootPlacement` field | Meaning |
| --- | --- |
| `pelvis` | Bone lowered so the lower foot reaches (nil = never move the body) |
| `probe_up`, `probe_down` | The ray starts this far above the animated foot and reaches this far below it |
| `max_pelvis_drop` | Largest pelvis drop in metres |
| `smoothing` | 1/s, how fast corrections follow the ground (0 = instant) |
| `align_to_ground`, `max_slope` | Tilt feet to the ground normal, at most this many degrees |
| `lock_feet`, `lock_speed`, `lock_height`, `release_distance` | Foot locking: a foot slower than `lock_speed` m/s within `lock_height` of its rest height stays where it planted until it lifts or is `release_distance` away from the animated foot |
| `mask` | Physics layers that count as ground |

`FootContact.lock` off lets a foot follow the ground without planting (a dragging foot, a tail tip). The ground comes from `AnimationSettings::ground`; the player, CLI and Studio play mode pass `runtime::physics_ground_probe(physics)`, which ignores the character's own bodies. With no probe (edit mode) the pose is unchanged. `AnimationStats::feet` reports characters, feet, grounded, locked and invalid (FootContact without a usable two-bone IK). View > IK and Foot Placement draws IK chains, targets, probe rays and ground points.

## Retargeting

Set an Animator's `clip_source` to another model to play that model's clips on this one (ADR-0075). The clips are converted once, the first time they are needed, and merged with the model's own clips; a clip the model already has keeps the model's version. Controllers, `play_animation` and the Animation window use the merged list.

Bones are matched by name: exact names first, then names compared without a namespace prefix (`mixamorig:`, `Armature|`), case, underscores, dots, dashes or spaces, so `mixamorig:Left_Arm` drives `LeftArm`. For every matched bone the clip's change of world-space rotation from the rest pose is applied to the target bone's own rest, so rigs whose bones point along different axes, or that sit under an extra rotated `Armature` node, still move the same way. Only the root bone (the first matched bone whose source moves) is translated, with its travel scaled by the ratio of the two rigs' root heights; every other bone keeps the target's own lengths. Bones with no match keep their rest pose.

Rest poses should be similar (both T-pose or both A-pose); there is no automatic pose correction, no IK for foot contact (add an `IKConstraint`) and no bone-map editor yet. `victus check` warns when no bone of the clip source matches the model. C++: `victus/animation/retarget.hpp` (`map_bones`, `retarget_clip`, `retarget_set`, `RetargetOptions::bone_map` for explicit mappings).

## Morph targets

glTF morph targets (blend shapes) import with their names (`mesh.extras.targetNames`, else `target0`, `target1`, …), default weights (`mesh.weights`, overridden per node by `node.weights`) and `weights` animation channels (ADR-0076). Position and normal deltas are applied; tangent deltas are ignored.

Animated weights blend like bones: each target becomes a weight node named `<node>/<target>` under its mesh node, so cross-fades, blend spaces, override and additive layers, and masks that include the mesh node all mix morph weights too. The Animator writes them into a `MorphWeights` component on the mesh node's entity. Weights are not clamped (glTF allows negative and above-1 values).

Set weights from code by name or index — they persist until an animation that animates that target overwrites them:

```lua
local face = scene.find("Head")
print(table.concat(face:morph_targets(), ", "))  -- "Smile, Blink, JawOpen"
face:set_morph_weight("Smile", 0.8)
face:set_morph_weight(2, 1.0)                    -- 1-based index: Blink
local open = face:morph_weight("JawOpen")         -- number, or nil for an unknown target
```

Visual scripts: Set Morph Weight. C++: `AnimationSystem::morph_targets`, `set_morph_weight`, `morph_weight` (0-based index or name), `render::apply_morphs`. Morphing runs on the CPU for each entity whose weights are not all zero and uploads a new vertex copy only when the weights change; keep morphing to faces and hero props rather than crowds (see KNOWN_LIMITATIONS).

## Corrective shapes

A corrective is a morph target whose weight follows one joint's angle (ADR-0220). Use it where linear blend skinning loses volume, such as a forearm that twists or an elbow or knee that bends, without switching the whole mesh to dual quaternion skinning. List correctives in a model's import settings and every re-import applies them; the source file is unchanged:

```json
"correctives": [
  {"joint": "LowerArm.R", "axis": [0, 1, 0], "degrees": 150, "steps": 3},
  {"joint": "LowerLeg.L", "axis": [1, 0, 0], "degrees": 115, "start_degrees": 20},
  {"joint": "UpperArm.L", "axis": [0, 0, 1], "degrees": 90, "morph": "ShoulderUp.L"}
]
```

- `axis` is in the joint's rest frame; Y runs along the bone, so Y is a twist. Only rotation about this axis counts (swing-twist decomposition).
- Without `morph`, the shapes are baked: `steps` (1 to 8) shapes named `Corrective.<joint>.<degrees>` are keyed evenly from `start_degrees` to `degrees`, each making LBS match `target` (`dual_quaternion`) exactly at its key, and they cross-fade in between.
- With `morph`, an existing shape key (for example one sculpted in Blender) is driven from 0 at `start_degrees` to 1 at `degrees`.

The renderer evaluates the drivers from the posed skeleton every frame and adds them to the mesh's morph weights, so animation, IK and Studio poses all trigger them. Studio: select the character, pick a joint in the Skeleton card and use Corrective shape > Bake corrective. CLI: `victus skeleton orion --corrective LowerArm.R:y:150` bakes on a copy and prints the collapsed triangles with LBS, LBS plus corrective and DQS. Morphing runs on the CPU over the whole mesh while a weight changes (see ADR-0220 for the measured cost).

## Skeleton LOD

Distant characters do not need finger curls, toe roll, eye and jaw motion or face shapes. When an animated entity also has a `SimulationLodComponent` (ADR-0165; tiers at 30/80/200 m by default), `AnimatorComponent::detail_lod_tier` (default 2, so beyond 80 m) chooses the tier at which detail nodes are eased back to their rest pose over `detail_fade` seconds (default 0.3) and then are no longer sampled. -1 never reduces; 0 always does. Coming back into range eases them in again, so nothing snaps.

- Detail nodes are tagged when the animation set is built from a model (`AnimationSet::Node::lod = 1`): joints whose bone roles are only finger, toe, face, eye, jaw, tongue or secondary, and morph-weight channels. Sockets, IK targets and poles, mechanical, spine, head, neck, hand, arm, leg, foot and root bones always stay, so attachments and IK keep their positions.
- Orion and Nova: 35 of 96 animation nodes are detail joints (30 finger joints, both toes, jaw and eyes) plus 26 face morph weights; the reference quadruped has 6 of 41, the mechanical loader none.
- Body joints are unchanged by the reduction; `AnimationStats::channels_sampled` counts only the channels actually sampled and `detail_reduced` the animators currently reduced.
- This is separate from mesh LOD (fewer vertices) and from the update-rate throttling of distant animators; all three combine.

## Editor

Window > Animation opens the Animation window for the selected entity's Animator. It shows the controller path, the states (the current one highlighted with its normalized time; click one to enter it), the previous state and fade progress during a cross-fade, the current clip weights as bars (missing clips flagged) and a control per parameter: float sliders spanning the blend thresholds, int drags, bool checkboxes and trigger buttons. Preview runs the controller on the edit scene through `AnimationSystem::update_entity`; Pause and Restart control it, End Preview restores the Animator and the skeleton transforms. A preview is ended (and restored) before saving, autosave skips while one runs, and play mode, scene switches and closing the project end it. Root-motion states are tagged `[root motion]`, and the window shows the root node and how far it moves per frame. Controller edits hot reload into the preview. `victus_editor <project> --select <entity> --show animation-preview` opens it from the command line.
