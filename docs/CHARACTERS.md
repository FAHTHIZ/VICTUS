# VICTUS Characters — the character pipeline, Orion and Nova

The 3D and multiplayer templates ship two VICTUS characters, **Orion** and **Nova**: human pilots
in VICTUS flight suits, built by the VICTUS character pipeline (`tools/characters`, ADR-0179) from
the CC0 MakeHuman base mesh. They share one skeleton layout, one clip library, one animation
controller and one set of gameplay scripts; swapping one for the other changes the look and
proportions, not the behaviour. They replace the signed-distance-field android mannequins of
ADR-0121 (which replaced the Vanguard robots of ADR-0069). Asset ids, file paths and UUIDs are
unchanged, so existing projects keep their references.

| | Orion | Nova |
|---|---|---|
| Id (CLI / launcher) | `orion` (default) | `nova` |
| Model | `assets/models/characters/victus_orion.glb` | `assets/models/characters/victus_nova.glb` |
| Stable UUID (`.meta`) | `8e84eb12-63c8-4aed-aae2-517c038d3071` | `44037d0d-1627-44a8-a225-633971f196b2` |
| Skeleton profile | `VictusHumanoid.Orion` | `VictusHumanoid.Nova` |
| Build | masculine, athletic (MakeHuman macro: gender 1.0, muscle 0.62, weight 0.48) | feminine, slender (gender 0.0, muscle 0.55, weight 0.42) |
| Look | cropped hair, stubble, graphite suit with cyan piping | hair in a low bun, freckles, ivory suit with amber piping |
| Height (floor to top of hair) | 1.77 m | 1.68 m |
| Triangles (LOD 0, as exported) | 125,184 | 128,816 |
| GLB size | 15.9 MB | 15.8 MB |

Both stand on Y = 0, face +Z with their left on +X, and are authored in the A-pose the earlier
characters used (arms 40° from vertical), so existing clips and ragdoll setups line up.

## What a character contains

Each GLB is self-contained (no external buffers or images). One skinned mesh node
`<Name>_Body` carries five primitives, one per material, so a character costs five skinned draws
per pass:

| Material | Part | Maps | Notes |
|---|---|---|---|
| `<Name>_Skin` | head, neck, hands (the bare skin) | base colour, normal, ORM (AO/roughness/metal) | 2048² atlas; the head and hands are repacked from MakeHuman's UV layout at 2.3× texel density. Skin tone, lip tint, pores, stubble/freckles, nail plates, knuckle creases, skin lines and redness over the cheeks, nose and ears are baked per texel (no subsurface scattering yet; the engine's PBR shader is a standard metal/rough BRDF) |
| `<Name>_Suit` | flight suit | base colour, normal, ORM, emissive | 2048²; panel seams, stitching, knee and shoulder pads, collar and wrist cuffs, piping on the emissive map (strength 4, `KHR_materials_emissive_strength`) |
| `<Name>_Eye` | eyeballs | base colour, normal, ORM | 512²; MakeHuman high-poly eye proxy; iris stroma and collarette, limbal ring, sclera veins |
| `<Name>_Mouth` | teeth, gums, tongue | base colour, normal, ORM | 512²; separate upper teeth (Head) and lower teeth/tongue (Jaw) |
| `<Name>_Groom` | scalp hair, brows, lashes (and Nova's bun) | base colour with alpha, normal, ORM | 2048², alpha mask 0.5, double-sided; strand patterns are line-integral-convolved along a hair flow field |

All maps are baked from the actual surface (per-texel world position and normal, a Cycles
ambient-occlusion bake against the whole character) — there are no flat material colours and no
placeholder textures. Albedo and ORM are JPEG, the groom is PNG (it needs alpha). The engine's
texture cooker builds mips and GPU block compression (BC7/ASTC) at import like any other model.

Blend shapes: every primitive carries the same 44 ARKit-style facial targets (`browDown_L`,
`eyeBlink_L`, `eyeWide_L`, `cheekPuff`, `mouthSmile_L`, `mouthFrown_R`, `mouthPucker`, `tongueOut`,
...), derived from MakeHuman's facial pose units. Jaw opening is the `Jaw` joint, eye gaze the
`Eye.L` / `Eye.R` joints. Morph deltas are sparse and stored as normalised integers
(`KHR_mesh_quantization`: 0.03 mm position resolution), which the importer reads natively.

## Skeleton

60 joints, identical names, order and hierarchy in both characters (the bind poses differ, one
per skeleton profile):

- Joints 0-20 are the original 21-joint humanoid, in the original order: `Hips, Spine, Chest,
  Neck, Head, Shoulder.L, UpperArm.L, LowerArm.L, Hand.L, Shoulder.R, ... Hand.R, UpperLeg.L,
  LowerLeg.L, Foot.L, Toe.L, UpperLeg.R, ... Toe.R` (`project::kCharacterCoreJointCount`). Scripts,
  ragdoll maps, foot IK and retargeting written for that layout keep working.
- Per side: `UpperArmTwist`, `LowerArmTwist`, `UpperLegTwist` (twist joints that take part of the
  roll so forearms and thighs do not candy-wrap), and `Thumb1-3`, `Index1-3`, `Middle1-3`,
  `Ring1-3`, `Pinky1-3`.
- `Jaw`, `Eye.L`, `Eye.R`.

Skin weights come from MakeHuman's 163-bone reference weights, collapsed onto this layout and
limited to four influences (stored as normalised bytes). Clips store rotations plus the Hips
translation, so one clip drives both profiles.

## Sockets

Attachment points are named child nodes. Their frames follow one convention so equipment
authored for one character fits the other:

| Node | Parent | Frame |
|---|---|---|
| `Socket.Root` | `Rig` | ground, character facing |
| `Socket.Head` | Head | top of the skull, character facing (hats, helmets) |
| `Socket.Face` | Head | 3 cm in front of the eyes (visors, glasses) |
| `Socket.Back` | Chest | 2 cm behind the upper back (backpacks, weapons) |
| `Socket.Hand.L`, `Socket.Hand.R` | Hand.L / Hand.R | grip centre; +Y along the fingers, +Z out of the palm |
| `Socket.Hip.L`, `Socket.Hip.R` | Hips | outside of each hip (holsters, pouches) |

Attach something with `character:attach(item, "Socket.Hand.R")` in Luau (it snaps the item to the
socket and returns the socket node, or `nil` when the character has no such socket; pass `true` as
a third argument to keep the item's world placement), `Scene::attach_to_socket` in C++, or by
dragging an entity onto the node in the Hierarchy. `entity:find_descendant(name)` finds any nested
node; `find_child` only looks at direct children, and sockets sit inside the skeleton.

## Clips and controller

The library has 41 clips sampled at 30 fps, generated from parametric gait and pose code
in `tools/characters/anim.py`. The original seven keep their names, lengths and phases:

| Clip | Length | Loop |
|---|---|---|
| Idle | 4.0 s | yes, breathing, a head glance, blinks |
| Walk | 0.8 s | yes |
| Run | 0.55 s | yes, phase-synced with Walk |
| Jump | 0.6 s | one-shot |
| Fall | 0.8 s | yes |
| Land | 0.4 s | one-shot |
| Interact | 0.9 s | one-shot hand reach |

Added: `Idle_Look`, `Idle_Shift`; strafes and backpedals `Walk_Back/Left/Right`,
`Run_Back/Left/Right`; `Sprint`; transitions `Walk_Start/Stop`, `Run_Start/Stop`,
`Turn_Left_90`, `Turn_Right_90`, `Land_Hard`; crouch `Crouch_Idle`, `Crouch_Walk(_Back/_Left/_Right)`,
`Crouch_Enter/Exit`; actions `Pickup`, `Wave`, `Point`, `Aim`; hand poses `Grip_L`, `Grip_R`; and
facial clips `Face_Blink`, `Face_Talk`, `Face_Smile`, `Face_Frown`, `Face_Surprise` (blend-shape
and jaw/eye channels only, for an upper or face layer). The GLB's `extras.victus.clips` lists each
clip with its length, loop flag and layer.

`assets/anim/characters/victus_locomotion.vanim` drives both characters. Parameters: `speed`
(m/s), `grounded` (default true), triggers `jump` and `interact`.

- `Locomotion`: 1D blend of Idle (0), Walk (2.5) and Run (5).
- `jump` from Locomotion, Land or Interact enters `Jump`; after it, `Fall` loops while airborne.
- Leaving the ground without jumping also enters `Fall`.
- Touching down enters `Land`, which returns to `Locomotion`.
- `interact` (E / gamepad West in the templates) plays `Interact` and returns to `Locomotion`.

## LODs and budgets

LOD 0 is one Catmull-Clark subdivision of the MakeHuman topology (about 125k triangles): the
high-end PC budget, with clean edge loops at the eyes, mouth, shoulders, elbows, knees and
fingers. The importer generates three coarser LODs with meshoptimizer
(`ModelImportSettings::lods`), and per-cascade shadow LOD applies as for any model. Templates
scale the character to the gameplay capsule (player 1.45 m) uniformly, keeping Orion taller than
Nova.

## How they are made

```
python3.11 -m venv /tmp/bpyenv
/tmp/bpyenv/bin/pip install bpy==5.0.1 numpy scipy pillow
/tmp/bpyenv/bin/python tools/characters/test_pipeline.py
/tmp/bpyenv/bin/python tools/characters/build_characters.py --out engine/project/data/characters
```

The full build takes about 12 minutes on two cores (most of it is per-texel baking and the Cycles
AO bake); `--only orion --size 0.25 --no-ao` gives a quick preview in under a minute. Output is
deterministic for one environment. Stages:

| Module | Stage |
|---|---|
| `mh.py` | MakeHuman loaders: base mesh (`hm08`), macro targets (gender, muscle, weight, ethnic mix), proxy fitting (`.mhclo`), the reference rig and weights, facial pose units (BVH) |
| `fit.py` | shapes the body from the character's `Spec`, poses it into the A-pose, collapses the reference rig onto the 60-joint layout, derives the 44 facial blend shapes, converts to VICTUS space |
| `assemble.py` | cuts the body into skin and suit (suit offset outward with cut-plane cuffs, skin overlapping under it so nothing gaps or clips while posing), builds eyes, mouth, scalp hair shell, brow shells, lash cards and Nova's bun, and repacks UV islands |
| `meshops.py` | Catmull-Clark subdivision as a sparse operator (weights and morphs subdivide with the positions), GPU vertices and tangents |
| `materials.py`, `bake.py` | per-texel material functions, height → normal conversion, Cycles AO bake, encoding |
| `anim.py` | the clip library |
| `gltf.py`, `build_characters.py` | skeleton frames, sockets, clips and the GLB writer; `characters.build.json` records each output's size, SHA-256 and counts |

To make a new character, add a `Spec` (macro values and skull height) in `fit.py` and a `Style`
(skin, lips, iris, hair colour and style, suit and trim colours) in `assemble.py`, then build it.

The GLBs under `engine/project/data/characters` are embedded into the engine at build time
(`cmake/EmbedBinary.cmake`, assembler `.incbin` on GCC/Clang, byte arrays on MSVC) and written
into new projects by the templates.

## Validation

- `tools/characters/test_pipeline.py`: subdivision operator, tangent handedness, quaternions,
  quantised sparse morphs and byte weights, the skeleton layout's backward compatibility, the clip
  library's original durations.
- `victus_asset_tests` "starter characters" cases: zero importer warnings, five materials each with
  albedo, normal and ORM textures, 100k-140k triangles, 44 blend shapes on every part, 60 joints
  with the original 21 first, the per-character height, the original seven clips with their
  lengths, every socket under its parent, and the shared controller driving Idle, Walk, Jump,
  Fall, Land and Interact on the imported model.
- `victus_asset_tests` "character validation": both starters pass `project::validate_character`
  with no errors or warnings; a renamed core joint, reordered skin joints, a missing socket, a
  missing controller clip and centimetre units are each reported.
- `victus_scene_tests` "find_descendant and attach_to_socket" and `victus_script_tests` "scene,
  entity and input API": nested socket lookup and attachment from C++ and Luau.
- `victus_gpu_tests` "gpu character lods": Orion rendered by the VICTUS renderer in four poses at
  2.5, 8 and 20 m, LOD image against LOD 0 image.

### Renders

Blender render (Cycles, the shipped GLBs imported into Blender 5.0, studio lighting; not the
VICTUS renderer): Orion front and three-quarter, Nova front and back three-quarter.

![Blender render: Orion and Nova full body](images/characters/blender_render_bodies.jpg)

![Blender render: Orion and Nova heads](images/characters/blender_render_heads.jpg)

VICTUS runtime (the engine's own renderer, `victus run --screenshot` on a fresh 3D template):

![VICTUS runtime: Orion in the 3D template](images/characters/victus_runtime_orion.jpg)

![VICTUS runtime: Nova in the 3D template](images/characters/victus_runtime_nova.jpg)

### Validating a character

`victus character-check <model>` imports a `.glb`, `.gltf` or `.fbx` (or `orion` / `nova`, the
embedded starters) and checks that it can replace a starter character without script, controller
or template changes:

- errors: no skin; a core joint missing, out of the original order or under the wrong parent; a
  socket missing; one of the controller clips (Idle, Walk, Run, Jump, Fall, Land, Interact)
  missing or empty; weights pointing outside the skin; a bind-pose height outside 1.2-2.4 m
  (usually centimetre units or a Z-up export);
- warnings: a joint count other than 60 (`--any-joints` silences it), a socket under an
  unexpected parent, unskinned meshes, weights that do not sum to 1, feet off the ground, more
  than 20k triangles without LODs, materials without a base colour texture or normal map, and
  importer warnings.

It prints a summary line, one line per issue and `OK` or `FAILED` (exit code 1 on errors, or on
any issue with `--strict`); `--json` prints the same report as JSON for CI. The checks are
`project::validate_character` in `engine/project/src/character_validate.cpp`.

## Choosing a character

- CLI: `victus new MyGame --template 3d --character nova` (`orion`, `nova` or `none`).
- Launcher: the New Project form shows a Character choice (Orion, Nova, None) for templates that
  have a player character.
- Studio: Add > Characters (or the command palette's "Entity: Add Orion/Nova") adds either one at
  the camera focus; Ragdoll Lab adds both as ragdolls.
- `none` keeps the capsule body and writes no character files.

## Licensing and provenance

The base mesh, targets, reference rig, weights, facial pose units, eye and eyelash geometry come
from MakeHuman's asset library, released under CC0 1.0 (vendored unmodified, gzip-compressed, in
`third_party/makehuman`, commit `a8bc2d54ff0ac92e78ff71431b1023eda42bf482`; see its README). The
MakeHuman application code (AGPL-3.0) is not used or included. Everything else — the suit,
groom, materials, textures, sockets, clips and the pipeline — is original VICTUS work, Apache-2.0.
Blender (`bpy`, GPL) is a build-time tool for the AO bake only; its output belongs to the user
([Blender license](https://www.blender.org/about/license/)). See ADR-0069, ADR-0121 and ADR-0179.
