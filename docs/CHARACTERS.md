# VICTUS Starter Characters — Orion and Nova

The 3D and multiplayer templates ship two original VICTUS characters, **Orion** and **Nova**: a
pair of sculpted android mannequins in the spirit of Unreal Engine's Manny and Quinn (two
realistic-proportion mannequins on one shared skeleton,
[Epic: Third Person template](https://dev.epicgames.com/documentation/unreal-engine/third-person-template-in-unreal-engine)).
They share one skeleton, one clip library, one animation controller and one set of gameplay
scripts. Swapping one for the other changes the look, not the behaviour. They replace the boxy
Vanguard V-01/V-02 robots (ADR-0121).

| | Orion | Nova |
|---|---|---|
| Id (CLI / launcher) | `orion` (default) | `nova` |
| Model | `assets/models/characters/victus_orion.glb` | `assets/models/characters/victus_nova.glb` |
| Stable UUID (`.meta`) | `8e84eb12-63c8-4aed-aae2-517c038d3071` | `44037d0d-1627-44a8-a225-633971f196b2` |
| Build | athletic: broad deltoids and trapezius, V-taper torso, squarer jaw, cropped sculpted hair | slender: narrower shoulders and waist, wider hips, softer jaw, swept sculpted hair |
| Signature glow | cyan | amber |
| Triangles (highest LOD, as exported) | 26 129 | 26 096 |
| Vertices | 19 248 | 18 699 |

Both are 1.70 m tall to the crown of the hair, stand on Y = 0, face +Z with their left on +X, and
are authored in a neutral A-pose (arms 40° from the body).

## Shared identity

- A stylised mannequin face built on classical facial thirds: brow ridge, nose, lips and chin are
  sculpted, the eyes are dark gloss almonds. Hair is a sculpted cap, not cards, so it costs nothing
  extra to shade or sort.
- White satin shell plates over a dark graphite undersuit: a V-cut chest cuirass with the glowing
  VICTUS "V" chevron, three rows of segmented abdominal plates, shoulder caps, forearm and shin
  guards, a brief-cut pelvis plate. The seams between plates read as panel lines at any distance.
- The glow is a small accent; the design reads with it off.

## Files and materials

Each GLB is self-contained (no external buffers or textures) with four material slots, so a
character costs four skinned draws per shadow/depth/main pass. Materials are single-sided (the
shells are closed), which halves the fragments compared with double-sided armour:

| Material | Use |
|---|---|
| `<Name>_Shell` | satin white shell plates, roughness 0.34 |
| `<Name>_Structure` | graphite undersuit, metallic 0.55, roughness 0.46 |
| `<Name>_Visor` | dark gloss eyes, roughness 0.10 |
| `<Name>_Glow` | emissive chevron (strength 4, `KHR_materials_emissive_strength`) |

The importer optimises the meshes and generates three coarser LODs (`ModelImportSettings::lods`,
meshoptimizer simplification), the same as any other imported model.

## How they are made

The figures are sculpted as code. `tools/characters/anatomy.py` describes the body as a
signed-distance field (round cones, ellipsoids and boxes joined with smooth unions; the field is
evaluated mirror-symmetric) plus the plate regions cut from it. `tools/characters/sdf.py` samples
the field sparsely and polygonises it with marching cubes (scikit-image); `mesher.py` removes hidden
faces and derives skin weights; `build_characters.py` decimates every piece to its triangle budget
(faces and hands get a higher density), sets smooth normals from the field gradient, builds the
rig, bakes the clips and exports the GLBs. The editable source is
`engine/project/data/characters/source/victus_characters.blend`.

```
python -m venv /tmp/bpyenv && /tmp/bpyenv/bin/pip install bpy==5.0.1 scikit-image
/tmp/bpyenv/bin/python tools/characters/test_sdf.py
/tmp/bpyenv/bin/python tools/characters/build_characters.py --out engine/project/data/characters
```

The run takes a few minutes on two cores. Exports are not bit-for-bit reproducible across runs (the
exporter's float output varies in the last bits), so commit the GLBs that were reviewed. The GLBs
under `engine/project/data/characters` are embedded into the engine at build time
(`cmake/EmbedBinary.cmake`) and written into new projects by the templates.

## Skeleton and sockets

21 deform joints, identical names, order, rest transforms and inverse bind matrices in both GLBs (the same rig the
earlier Vanguard robots used, so clips, controller, scripts and ragdolls carried over unchanged):

`Hips > Spine > Chest > Neck > Head`, and per side `Chest > Shoulder > UpperArm > LowerArm > Hand`
and `Hips > UpperLeg > LowerLeg > Foot > Toe` (`.L` / `.R` suffixes). The original template robot's
joint names (Hips, Spine, Head, UpperArm, LowerArm, UpperLeg) are kept, so scripts that look up
those nodes keep working. Skinning is smooth: weights come from the distance to each bone's sculpt primitives
and are limited to the dominant bone, its parent and its children (at most four influences), so
shoulders, elbows, hips and knees bend without the left side ever pulling on the right.

Attachment points are named child nodes (VICTUS has no formal socket component yet):

| Node | Parent | Use |
|---|---|---|
| `Socket.Head` | Head | hats, lights |
| `Socket.Hand.L`, `Socket.Hand.R` | Hand.L / Hand.R | held items |
| `Socket.Back` | Chest | backpacks, weapons |
| `Socket.Root` | model root | ground effects |

Attach something with `entity:find_child("Socket.Hand.R")` in Luau or by dragging an entity onto
the node in the Hierarchy.

## Clips and controller

| Clip | Length | Loop |
|---|---|---|
| Idle | 4.0 s | yes, breathing and a head glance |
| Walk | 0.8 s | yes |
| Run | 0.55 s | yes, phase-synced with Walk |
| Jump | 0.6 s | one-shot |
| Fall | 0.8 s | yes |
| Land | 0.4 s | one-shot |
| Interact | 0.9 s | one-shot hand reach |

`assets/anim/characters/victus_locomotion.vanim` drives both variants. Parameters: `speed`
(m/s), `grounded` (default true), triggers `jump` and `interact`.

- `Locomotion`: 1D blend of Idle (0), Walk (2.5) and Run (5).
- `jump` from Locomotion, Land or Interact enters `Jump`; after it, `Fall` loops while airborne.
- Leaving the ground without jumping also enters `Fall`.
- Touching down enters `Land`, which returns to `Locomotion`.
- `interact` (E / gamepad West in the templates) plays `Interact` and returns to `Locomotion`.

## Choosing a character

- CLI: `victus new MyGame --template 3d --character nova` (`orion`, `nova` or `none`).
- Launcher: the New Project form shows a Character choice (Orion, Nova, None) for templates that
  have a player character.
- Editor: Add > Characters (or the command palette's "Entity: Add Orion/Nova") adds either one at
  the camera focus; Ragdoll Lab adds both as ragdolls.
- `none` keeps the capsule body and writes no character files.
- In an existing project both GLBs are present. To swap: instantiate the other GLB (drag it from
  the Assets panel into the viewport, or the command palette's "Instantiate Model"), drag it onto
  `Player` in the Hierarchy, set its position to 0 and its uniform scale to 0.853, rename it
  `Robot`, set its Animator controller to `anim/characters/victus_locomotion.vanim`, and delete
  the old `Robot`. Scripts, the capsule and the controller stay the same.

The template player is 1.45 m (the capsule is unchanged: radius 0.7, height 1.4) and the companion
Buddy uses the other character at 0.82 m.

## Licensing

All geometry, materials, insignia and animation are original VICTUS work generated by the
repository scripts; no third-party assets are included. Blender, NumPy and scikit-image are build-time tools only. Its
licence states that what you create with it is your sole property, and that published scripts
using `bpy` must carry a GPL-compatible licence
([Blender license](https://www.blender.org/about/license/)). The generator is Apache-2.0, which the
FSF lists as compatible with GPLv3 ([FSF licence list](https://www.gnu.org/licenses/license-list.html)).
See ADR-0069 and ADR-0121.
