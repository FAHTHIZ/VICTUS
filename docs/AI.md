# Gameplay AI

`engine/ai` (`VICTUS::Ai`, `victus/ai/ai.hpp`) provides behaviour trees, a per-entity blackboard
and perception. The player, the CLI `simulate`/`run` commands, editor play sessions and in-editor
clients all run an `AiSystem` after navigation.

## Components

| Component | Fields | Purpose |
|---|---|---|
| `BehaviorTree` | `tree` (asset path, e.g. `ai/guard.vbt`), `tick_interval` (s, 0 = every frame), `enabled` | Runs a tree on the entity |
| `Perception` | `sight_range`, `fov` (full cone around +Z), `eye_height`, `hearing_range`, `memory`, `target_mask`, `occlusion_mask`, `line_of_sight`, `target_key` | Sees and hears |
| `Perceptible` | `team` (bits), `height` (aim point), `enabled` | Can be noticed |

## Tree files (`.vbt`)

JSON with an optional `blackboard` object (defaults written when the tree starts, existing keys
are kept) and a `root` node. Every node has a `type` and an optional `name` shown in the
debugger. Children go in `children` (composites) or `child` (decorators).

| Type | Parameters | Result |
|---|---|---|
| `sequence` | `reactive` | Runs children in order; fails on the first failure |
| `selector` | `reactive` | Runs children in order; succeeds on the first success |
| `parallel` | `threshold` (successes needed, 0 = all) | Ticks all children every tick; remaining children are aborted when it finishes |
| `inverter`, `succeeder`, `failer` | | Flip / force the child's result |
| `repeat` | `count` (0 = forever) | Re-runs a succeeding child, one run per tick; fails when it fails |
| `cooldown` | `seconds` | Fails without running the child until `seconds` after it last finished |
| `timeout` | `seconds` | Aborts the child and fails after `seconds` |
| `guard` | `key`, `op`, `value` | Runs the child while the condition holds; re-checked every tick |
| `wait` | `seconds`, `deviation` | Running until the time passed (random +- deviation) |
| `check` | `key`, `op`, `value` | Success when the condition holds |
| `set` / `clear` | `key`, `value` | Writes / removes a blackboard key |
| `move_to` | `key` (vector or entity), `acceptance`, `speed` | Paths with the NavAgent (re-paths when an entity target moves); entities without one move straight at `speed` |
| `random_point` | `key`, `radius` | Writes a random navmesh point near the entity |
| `action` | `action` | Calls a C++ action registered with `AiSystem::register_action` |
| `succeed`, `fail`, `log` | `message` (log) | |

Conditions: `op` is one of `set`, `unset`, `true`, `false`, `==`, `!=`, `<`, `<=`, `>`, `>=`
(default: `==` when a `value` is given, else `set`). Values are numbers, booleans, strings or
`[x, y, z]` vectors.

Execution: the tree is ticked from the root. Sequences and selectors remember their running child
and resume it; with `"reactive": true` earlier children are re-evaluated every tick and pre-empt
later ones. A running node that is not reached on a tick is aborted (`move_to` stops its agent if
the destination is still the one it set; actions receive a final call with `aborted = true`). When
the root finishes the tree starts again on the next tick.

## Perception

Each update, perceivers look for `Perceptible` entities whose `team` shares a bit with
`target_mask`, within `sight_range` and the `fov` cone from the eye, with a physics ray (layers in
`occlusion_mask`) reaching the target. The current target is kept while visible, otherwise the
nearest visible one is chosen; a lost target is remembered for `memory` seconds. Noises from
`AiSystem::report_noise(position, loudness, source)` are heard within `hearing_range + loudness`.
Blackboard keys written: `<target_key>` (entity, removed when forgotten),
`<target_key>_position` (last seen position), `<target_key>_visible`, `heard`, `noise_position`.

## Debugging

View > AI Debug draws sight cones (yellow), lines to visible (red) or remembered (orange)
targets and heard noises (blue) during play, and opens the AI Debugger window with the selected
entity's tree (last status per node), blackboard and perception state.

## Limits

No visual tree editor yet, no Luau leaf tasks or blackboard access from scripts (planned), no
EQS-style queries, no team/faction relationships beyond bit masks.
