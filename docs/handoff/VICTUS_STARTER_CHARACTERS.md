# VICTUS Signature Starter Characters — Agent Handoff

**Priority:** High — 3D starter-template identity and usability  
**Status:** Ready for implementation  
**Target:** Commit directly to main; do not create a branch or PR.

## Mission

Create and integrate two original, recognizable VICTUS starter robot characters: a masculine-presenting V-01 and feminine-presenting V-02. They are two variants of one coherent character family, not generic mannequins and not simple recolours. Deliver real editable source, engine-ready assets, animations, integration and tests. Do not stop at concept art, blockouts or this document.

Working family name: **Vanguard**. V-01/V-02 are provisional identifiers and may be renamed if a better VICTUS-owned identity emerges. Never copy Unreal Manny/Quinn, Unity/Godot mascots, third-party marketplace assets or their logos.

## 1. Shared recognizable identity

Both characters must be identifiable at gameplay distance and attractive in close-up editor previews.

- Height target: 1.70 m, feet at Y=0, facing local +Z (verify conventions before export).
- Premium stylized hard-surface robot: large readable forms, clean bevels, restrained panel seams, no noisy greebles.
- Primary recognition anchor: one dark, gently curved horizontal visor across the front of the head, with two restrained cyan-white eye indicators. No human face.
- Secondary recognition anchor: a recessed rounded-hexagonal status/energy window high on the chest, centered and modest in size.
- Signature silhouette: split-shell shoulder caps with one distinctive diagonal step/cut, mirrored left and right.
- Original geometric V insignia on upper chest or shoulder; make it VICTUS-owned, not derivative of another engine mark.
- Shells over darker mechanical understructure, with visible articulation at shoulders, elbows, wrists, hips, knees and ankles.
- Palette: pearl/off-white ceramic shell; deep graphite/blue-grey structure; muted satin titanium accents; restrained cyan emissive. Optional tiny amber service indicator only.
- Identity must still read with emissive disabled. Avoid bloom-dependent design, mirror chrome, excessive lights and baked lighting.

### V-01 — masculine-presenting

Slightly broader shoulder span and upper torso, sturdy balanced proportions, angular two-piece chest shell tapering modestly toward waist, broader shoulder caps, robust upper arms, articulated forearms, sturdy thighs, tapered shins and stable feet. Head keeps the shared visor/head family but has a slightly squared crown and one subtle rear shell fin/vent. Add one small asymmetrical service panel on the left upper arm. Communicate dependable athletic utility, not exaggerated bulk or aggression.

### V-02 — feminine-presenting

Slightly narrower shoulder span and more tapered torso/limb armour, while preserving overall height, joint centres, foot contact, gameplay reach and shared rig compatibility. Use a cleaner continuous chest shell with gently curved side transitions, same chest-core placement, subtly narrower/swept split-shell shoulders, tapered forearms and streamlined thigh/shin shells. Head keeps the same visor and core silhouette with a softer crown transition and paired side/rear shell fins or vents. Add a small paired status-light motif at rear sides of head. Functional robot design only: no sexualized anatomy, exposed human-like body surfaces, high heels or impractical proportions. It must look like a distinct engineered variant, not just a recolour.

## 2. Asset deliverables

Deliver:
- Editable Blender source with named collections for both characters, shared rig, meshes, materials, sockets, LODs (if supported) and animation actions.
- One engine-ready GLB per character with embedded textures or correctly packaged references.
- Stable asset metadata sidecars and stable UUIDs; never silently change IDs already referenced by scenes.
- Suggested paths, adapt to actual repository conventions:
  - assets/models/characters/victus_vanguard_01.glb
  - assets/models/characters/victus_vanguard_02.glb
  - assets/anim/characters/vanguard_locomotion.vanim (or current controller convention)
  - assets/characters/source/victus_vanguard.blend
- Apply transforms; consistent scale, origin, forward axis, normals, tangents and UVs. No negative-scale rig hacks.
- Clean deformation topology around shoulders, elbows, hips and knees. No non-manifold geometry, inverted normals, accidental loose geometry or duplicate internal faces.
- Prefer shared material/texture resources. 1K textures by default; use 2K only with visual justification. Prefer packed ORM only if the importer supports the exact packing convention.
- Keep material slots and draw calls low. Target 3–4 material slots or fewer per character where practical.
- Provide LODs only if VICTUS actually supports them; if not, record the concrete blocker rather than inventing a feature.
- Use only supported glTF core features unless the importer is explicitly extended and tested.

## 3. Rig and animation

Repository facts to preserve:
- Current robot is generated procedurally in engine/project/src/robot.cpp.
- Current skeleton has 11 joints: Hips, Spine, Head, UpperArm.L/R, LowerArm.L/R, UpperLeg.L/R.
- Current tests expect two meshes, one 11-joint skin, four clips (Idle, Walk, Run, Jump), 1.67 m bounds and matching template hierarchy.
- Current importer supports glTF 2.0 skinning, top-four JOINTS/WEIGHTS influences, inverse bind matrices, translation/rotation/scale animation and PBR materials. Morph-target weights are skipped with a warning.
- Current animation system supports .vanim state machines and blend spaces. Retargeting, IK, additive layers, root motion and morph-target animation are not currently documented as supported.

Build one shared humanoid rig/rest pose for both characters:
- Preserve existing core joint names where practical to minimize controller/script churn.
- Add neck, shoulder, hand, foot and ankle/toe deform joints only after verifying importer, binding, evaluator and GPU joint-palette limits.
- Same joint names, hierarchy, axes, rest transforms and ordering in both variants.
- Neutral symmetric A/T-pose; four normalized influences maximum per vertex.
- Export deform bones only unless the engine genuinely needs control bones.
- Do not claim retargeting. Use the same actual skeleton and clips on both variants.

Required shared clips:
- Idle (loop; restrained breathing/status motion and occasional subtle head glance).
- Walk (loop; readable foot contacts and opposite arm swing).
- Run (loop; stronger lean/stride, phase-synchronized with Walk).
- Jump (one-shot).
- Fall (loop/state-compatible pose, if supported cleanly).
- Land (short transition/one-shot if supported).
- Interact/use (short hand reach; secondary priority).
- Turn-in-place only if the current controller can support it cleanly.

Keep existing speed, grounded and jump controller parameters unless code inspection proves a compatible change is required. Extend the existing controller; do not add a parallel animation architecture.

## 4. Engine integration

- The existing 3D project template should default to V-01. V-02 must be equally usable and swappable. Empty projects, 2D templates and simulation templates must not be forced to include a character.
- If the project launcher can support it without a broad redesign, offer V-01 / V-02 / Empty at 3D project creation. Otherwise default to V-01 and document the editor swap workflow.
- Both models go through the normal asset database, import and cook pipeline. No character-only renderer path.
- Keep gameplay root separate from visible mesh. Preserve CharacterController, capsule collision, input, camera and movement semantics.
- Integrate with relevant 3D player, companion and multiplayer templates without duplicating gameplay scripts.
- Add named attachment nodes/sockets for head, left/right hand, upper back and root. If a formal socket system does not exist, use named child nodes and document the limitation.
- Verify editor hierarchy/inspector, save/load, duplication, prefab, hot reload, export and packaged player.
- Keep both variants on the same controller and gameplay code.

## 5. Performance targets (measure; do not claim in advance)

- Readable at 720p and 1080p, without increasing render scale or post-processing cost.
- Aim for 20k–35k triangles per character at highest LOD, then reduce substantially for lower LOD if supported.
- Minimize skinned mesh/material splits; test two characters and a small crowd.
- Measure draw calls, triangles, CPU animation update, GPU skinning and memory at Low/Medium/High using existing VICTUS performance tools.
- No mobile FPS promise without real-device profiling. Report named hardware, driver, quality tier and actual frame-time percentiles.

## 6. Required acceptance checks

1. Both GLBs import/cook with zero warnings and stable metadata.
2. victus check and victus export validate both models, clips and controller.
3. Both render under normal VICTUS PBR, environment lighting, shadows and tone mapping.
4. Review front, 3/4, side, back, close-up and gameplay-distance screenshots; check with emissive on and off.
5. Every clip plays without missing joints, popping, inverted limbs, mesh separation or obvious foot sliding.
6. Test shoulder, elbow, wrist, hip, knee, ankle and foot deformation extremes; verify bounds/culling and joint-palette limits.
7. Swap V-01/V-02 while retaining the same controller, player script and collision behaviour.
8. Test run, jump, fall, land, save/load, duplicate, prefab, hot reload and packaged runtime.
9. Record real performance counters on a named machine; do not extrapolate desktop results to phones.
10. Add automated tests, update user guide, ROADMAP.md and docs/STATUS.md, commit to main and record exact SHA/commands/results.

## 7. Implementation sequence

1. Audit before editing: robot.cpp/.hpp, current generated model and controller, importer/cooker, skeleton binding, GPU joint-palette limits, template creation, editor asset workflows and every reference to the current robot UUID/path. Check latest main and working tree; do not overwrite unseen work.
2. Produce front/side/3-quarter blockout turntables for both and lock the shared identity.
3. Build shared rig plus V-01, import through real VICTUS pipeline and validate deformation/animation.
4. Build V-02 against the exact same rig/rest pose and animation library.
5. Produce/reuse clips and controller, then test both variants with current scripts.
6. Integrate template/editor choice and retain an Empty option.
7. Add tests, screenshots, measured performance, docs and commit. Continue to the next unblocked roadmap task afterward.

## 8. Verified repository context and research

Verified source files:
- engine/project/src/robot.cpp and robot.hpp
- tests/assets/template_robot_tests.cpp
- docs/STATUS.md
- ROADMAP.md (animation editor preview and optional per-state root motion remain open in priority 6)

Research references:
- Khronos glTF 2.0 specification: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- Blender 5.2 LTS glTF exporter documentation: https://docs.blender.org/manual/en/latest/addons/scene_gltf2.html
- Blender glTF exporter documentation, animation and skinning: https://docs.blender.org/manual/en/3.4/addons/import_export/scene_gltf2.html

All meshes, textures, insignia and animation must be original or explicitly redistributable. Record exact third-party licenses in the repository register before including any external asset.

**Important:** This brief is based on inspected source/status evidence, not a complete line-by-line audit of every file in VICTUS. The implementing agent must complete the targeted audit in step 1 before changing code.
