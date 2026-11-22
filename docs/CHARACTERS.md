# VICTUS Characters — the character pipeline, Orion and Nova

The 3D and multiplayer templates ship two VICTUS characters, **Orion** and **Nova**: Frame
mannequins - armoured sci-fi test dummies in the spirit of Unreal's Manny and Quinn - built by the
VICTUS character pipeline (`tools/characters`, ADR-0198; helmeted redesign ADR-0204) from the CC0 MakeHuman base mesh. They
share one skeleton layout, one clip library, one animation controller and one set of gameplay
scripts; swapping one for the other changes the look and proportions, not the behaviour. They
replace the realistic human pilots of ADR-0179 (which replaced the signed-distance-field androids
of ADR-0121 and the Vanguard robots of ADR-0069). Asset ids, file paths, UUIDs, joint names,
sockets and clips are unchanged, so existing projects keep their references.

| | Orion | Nova |
|---|---|---|
| Id (CLI / launcher) | `orion` (default) | `nova` |
| Model | `assets/models/characters/victus_orion.glb` | `assets/models/characters/victus_nova.glb` |
| Stable UUID (`.meta`) | `8e84eb12-63c8-4aed-aae2-517c038d3071` | `44037d0d-1627-44a8-a225-633971f196b2` |
| Skeleton profile | `VictusHumanoid.Orion` | `VictusHumanoid.Nova` |
| Build | masculine, athletic (MakeHuman macro: gender 1.0, muscle 0.62, weight 0.48) | feminine, slender (gender 0.0, muscle 0.55, weight 0.42) |
| Design | Frame 01 | Frame 02 |
| Look | graphite composite undersuit, glazed white ceramic plates, brushed-metal fasteners, black glass visor, cyan light | graphite composite undersuit, glazed pearl ceramic plates, silver hardware, black glass visor, violet light |
| Height (floor to top of the helmet) | 1.78 m | 1.69 m |
| Triangles (LOD 0, as exported) | 182,096 | 186,048 |
| GLB size | 12.27 MB | 12.41 MB |

Both stand on Y = 0, face +Z with their left on +X, and are authored in the A-pose the earlier
characters used (arms 40 degrees from vertical), so existing clips and ragdoll setups line up.

Why a mannequin: a realistic human needs skin, eye and hair shading (subsurface scattering, strand
hair) the renderer does not have yet, and falls into the uncanny valley without it. A mannequin
shows what the engine does well - layered PBR (clear-coated ceramic, composite fabric, brushed
metal, glass), emissive trims with bloom, normal-mapped detail, skinning and blend shapes - and reads as
a deliberate design, the way engine sample characters usually are.

## What a character contains

Each GLB is self-contained (no external buffers or images). One skinned mesh node
`<Name>_Body` carries three primitives, one per material, so a character costs three skinned draws
per pass:

| Material | Part | Maps | Notes |
|---|---|---|---|
| `<Name>_Frame` | undersuit body (neck to ankles, both hands) | base colour, normal, ORM, emissive | 2048²; the MakeHuman body smoothed into an idealised form (Taubin smoothing, the fingers keep their shape); a hexagonal micro-weave split into composite panels by seams, ribbed flex zones at the neck, shoulders, elbows, wrists, waist and knees, recessed piping down the outside of every limb carrying a thin light line |
| `<Name>_Armor` | helmet, 25 hard-surface plates, boots, ear pods, back core | base colour, normal, ORM, emissive; clear coat 0.65 (roughness 0.06) | 2048²; the helmet is the convex hull of the head's blurred volume, puffed out and shrink-wrapped by a 30x30-per-face quad cube-sphere, smoothed, with an open visor window, graphite side panels with a lit front edge, a crest ridge and vents; plates are shells grown from the body inside planes (a wide chest, back, three abdominal plates, lumbar, pelvis, hips, large shoulders, upper arms, forearms, backs of the hands, outer thighs, knees, shins, calves), 4.5-18 mm thick per plate (Nova's 20% thinner), with straight cut lines and edges rolled under; panel lines from a jittered Voronoi pattern, graphite inserts and metal fasteners; rounded bevels and an inset groove 1 cm in from every edge, a light strip in the groove of the larger plates, a contrasting stripe down the limb plates, a chevron emblem on the chest; boots with a moulded lug sole and accent toe caps; machined-metal ear pods with light rings and a back core with a glowing cell behind vents |
| `<Name>_Visor` | wraparound visor and face display | base colour, normal, ORM, emissive; clear coat 1.0 (roughness 0.02) | 1024²; black glass filling the helmet's visor window (eye line to jaw, 64 degrees to each side) with a fine scan-line grid in its reflection; two eye glyphs and a mouth bar lying on the glass, driven by blend shapes |

Emissive strengths (`KHR_materials_emissive_strength`): Frame 3, Armor 4, Visor 6, so the trims
bloom in the post stack. All maps are baked from the actual surface (per-texel world position and
normal, a Cycles ambient-occlusion bake against the whole character) - there are no flat material
colours and no placeholder textures. Every material is opaque (no alpha). Armor and Visor carry
`KHR_materials_clearcoat` factors, which the engine imports as a clear coat layer (ADR-0203): a
sharp glossy reflection over the satin ceramic and the glass. The engine's texture
cooker builds mips and GPU block compression (BC7/ASTC) at import like any other model.

Blend shapes: every primitive lists the same 26 face-display targets (`eyeBlink_L/R`,
`eyeWide_L/R`, `eyeSquint_L/R`, `browDown_L/R`, `browInnerUp_L/R`, `browOuterUp_L/R`,
`cheekSquint_L/R`, `jawOpen`, `mouthSmile_L/R`, `mouthFrown_L/R`, `mouthStretch_L/R`,
`mouthPucker`, `mouthRollUpper`, `mouthRollLower`, `mouthUpperUp`, `mouthLowerDown`); only the
Visor's glyphs move (the eye glyphs close, widen, squint and tilt like brows; the mouth bar opens,
curves and purses). The names are the ARKit ones the human characters used, so facial clips and
scripts keep working. Morph deltas are sparse and stored as normalised integers
(`KHR_mesh_quantization`), which the importer reads natively.

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
limited to four influences (stored as normalised bytes); every armour vertex keeps the weights of
the body vertex it was grown from, so plates follow the body they cover. The helmet, visor
and ear pods are rigid on `Head`; `Jaw` and `Eye.L/R` remain for retargeting and attachments. Clips store rotations plus the Hips
translation, so one clip drives both profiles.

Extra bones (facial, toes, roll, IK targets and poles, secondary motion, equipment) are added with
a skeleton migration rather than by changing the GLBs (docs/ANIMATION.md, "Extending skeletons";
ADR-0206). Adding 19 such bones to Orion leaves every existing joint index, socket and clip in
place and moves no vertex of any of the 41 clips by more than 0.001 mm, and the extended model
still passes character validation.

## Sockets

Attachment points are named child nodes. Their frames follow one convention so equipment
authored for one character fits the other:

| Node | Parent | Frame |
|---|---|---|
| `Socket.Root` | `Rig` | ground, character facing |
| `Socket.Head` | Head | top of the helmet, character facing (hats, crests, antennae) |
| `Socket.Face` | Head | 3 cm in front of the eyes (visors, glasses) |
| `Socket.Back` | Chest | 2 cm behind the back plate (backpacks, weapons) |
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
and jaw/eye channels only, for an upper or face layer; on the mannequins they animate the face
display, `jawOpen` following the jaw angle). The GLB's `extras.victus.clips` lists each
clip with its length, loop flag and layer.

`assets/anim/characters/victus_locomotion.vanim` drives both characters. Parameters: `speed`
(m/s), `grounded` (default true), triggers `jump` and `interact`.

- `Locomotion`: 1D blend of Idle (0), Walk (2.5) and Run (5).
- `jump` from Locomotion, Land or Interact enters `Jump`; after it, `Fall` loops while airborne.
- Leaving the ground without jumping also enters `Fall`.
- Touching down enters `Land`, which returns to `Locomotion`.
- `interact` (E / gamepad West in the templates) plays `Interact` and returns to `Locomotion`.

## Equipment

The mannequins carry no separate equipment nodes (the human pilots' boots, gloves, belt and
backpack of ADR-0179 are gone; the boots and hand plates are part of the armour). Gear attaches
through the [sockets](#sockets); `Socket.Back` sits behind the back plate, clear of the back core.
To hide armour in a script, untick Visible on `<Name>_Body[1]` (the Armor primitive) or call
`set_visible(false)` on it.

## LODs and budgets

LOD 0 is one Catmull-Clark subdivision of the quad meshes (about 185k triangles: the high-end PC
budget, with clean edge loops at the shoulders, elbows, knees and fingers and smooth plate
bevels). The importer generates three coarser LODs with meshoptimizer
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

The full build takes about 11 minutes on two cores (most of it is per-texel baking and the Cycles
AO bake); `--only orion --size 0.25 --no-ao` gives a quick preview in under a minute. Output is
deterministic for one environment. Stages:

| Module | Stage |
|---|---|
| `mh.py` | MakeHuman loaders: base mesh (`hm08`), macro targets (gender, muscle, weight, ethnic mix), proxy fitting (`.mhclo`), the reference rig and weights, facial pose units (BVH) |
| `fit.py` | shapes the body from the character's `Spec`, poses it into the A-pose, collapses the reference rig onto the 60-joint layout, converts to VICTUS space |
| `mannequin.py` | the Frame mannequin: smooths the body, builds the helmet from the convex hull of the head's blurred occupancy volume (`HullSurface`), grows the armour plates (`plate_regions`: planes plus a bone-segment radius per plate; `gear.shell` offsets them along the rest normal and rolls the cut edges under), boots, ear pods and back core, the visor and the face-display glyphs with their blend shapes, and repacks UV islands; `Style` holds each character's colours |
| `assemble.py`, `gear.py` | shared helpers: `Part`, joint maps, shells, boots, rounded boxes (the human pilots' skin/suit/groom assembly of ADR-0179 is kept for reference but no longer built) |
| `meshops.py` | Catmull-Clark subdivision as a sparse operator (weights and morphs subdivide with the positions), GPU vertices and tangents |
| `mannequin_materials.py`, `bake.py` | per-texel material functions (Frame, Armor, Visor), height → normal conversion, Cycles AO bake, encoding |
| `anim.py` | the clip library |
| `gltf.py`, `build_characters.py` | skeleton frames, sockets, clips and the GLB writer; `characters.build.json` records each output's size, SHA-256 and counts |

To make a new character, add a `Spec` (macro values) in `fit.py` and a `Style` (undersuit, plate,
accent, metal, light and glass colours, plate and undersuit roughness) in `mannequin.py`, then
build it. Plate layout changes go in `mannequin.plate_regions`.

The GLBs under `engine/project/data/characters` are embedded into the engine at build time
(`cmake/EmbedBinary.cmake`, assembler `.incbin` on GCC/Clang, byte arrays on MSVC) and written
into new projects by the templates.

## Validation

- `tools/characters/test_pipeline.py`: subdivision operator, tangent handedness, quaternions,
  quantised sparse morphs and byte weights, the skeleton layout's backward compatibility, the clip
  library's original durations.
- `victus_asset_tests` "starter characters" cases: zero importer warnings, three opaque materials
  each with albedo, normal, ORM and emissive textures, 140k-190k triangles, 26 blend shapes on
  every part, 60 joints
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
VICTUS renderer): Orion and Nova, each front and three-quarter.

![Blender render: Orion and Nova full body](images/characters/blender_render_bodies.jpg)

![Blender render: Orion and Nova heads](images/characters/blender_render_heads.jpg)

VICTUS runtime (the engine's own renderer: `victus run --frames 40 --quality ultra --screenshot` on a fresh 3D template, with the follow camera replaced by a fixed camera 2.6 m in front, the HUD removed and the brazier moved out of frame; lavapipe):

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
MakeHuman application code (AGPL-3.0) is not used or included. Everything else - the mannequin
geometry, armour, visor, materials, textures, sockets, clips and the pipeline - is original
VICTUS work, Apache-2.0.
Blender (`bpy`, GPL) is a build-time tool for the AO bake only; its output belongs to the user
([Blender license](https://www.blender.org/about/license/)). See ADR-0069, ADR-0121, ADR-0179, ADR-0198 and ADR-0204.
