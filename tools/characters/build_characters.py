# SPDX-License-Identifier: Apache-2.0
"""Build the VICTUS starter characters, Orion and Nova, with Blender's Python module.

Usage (Blender 5.x as a module in a Python 3.11 venv, plus NumPy and scikit-image):
  python -m venv /tmp/bpyenv && /tmp/bpyenv/bin/pip install bpy==5.0.1 scikit-image
  /tmp/bpyenv/bin/python tools/characters/build_characters.py [--out engine/project/data/characters]
                                                             [--voxel 0.0025] [--no-blend]

The figures are sculpted as signed-distance fields (anatomy.py on sdf.py) and polygonised with
marching cubes (mesher.py); this script decimates every piece to its triangle budget, sets smooth
normals from the field gradient, writes skin weights derived from the anatomy, builds the shared
21-joint rig, bakes the seven clips and exports one self-contained GLB per character.
Outputs:
  <out>/victus_orion.glb, <out>/victus_nova.glb, <out>/source/victus_characters.blend

Conventions (docs/CHARACTERS.md): 1.70 m tall, feet on the ground plane, facing +Z after the glTF
Y-up conversion (Blender -Y), character left on +X. Four material slots (Shell, Structure, Visor,
Glow), so a character costs four skinned draws per pass.

This script is licensed Apache-2.0 (GPL-3.0 compatible, as Blender's licence requires for bpy
scripts that are distributed). Its outputs, including the .blend, are VICTUS-owned assets.
"""
from __future__ import annotations

import argparse
import math
import os
import sys

import bpy  # noqa: I001 - bpy must load before bmesh/mathutils
import bmesh
import numpy as np
from mathutils import Matrix, Quaternion, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anatomy  # noqa: E402
import mesher  # noqa: E402
import sdf  # noqa: E402
from anatomy import ANKLE_Z, ARM_REST_OUT, HIP, KNEE_Z, L_FOREARM, L_HAND, L_UPPER_ARM, SHOULDER, arm_dir, bone_table  # noqa: E402,F401

FPS = 60
SHELL, STRUCT, VISOR, GLOW = 0, 1, 2, 3
TRIANGLE_BUDGET = 26000  # highest LOD, per character (tests: 20k-35k)
# Detail density per piece relative to its area share: faces and hands carry the eye.
DENSITY = {"Head": 2.6, "Hand.L": 2.4, "Hand.R": 2.4, "Eye.L": 1.5, "Eye.R": 1.5, "Emblem": 2.5, "Hair": 1.2}
MIN_TRIS = 60

# ----------------------------------------------------------------------------------------------
# Materials (metallic-roughness; the importer reads base colour, metallic, roughness, emissive and
# KHR_materials_emissive_strength).

PALETTES = {
    # shell base, structure base, glow colour
    "orion": ((0.86, 0.85, 0.82, 1), (0.045, 0.050, 0.060, 1), (0.45, 0.88, 1.0, 1)),
    "nova": ((0.88, 0.86, 0.84, 1), (0.060, 0.050, 0.062, 1), (1.0, 0.62, 0.38, 1)),
}


def make_materials(prefix, palette):
    shell, struct, glow = palette
    specs = [
        (f"{prefix}_Shell", shell, 0.0, 0.34, None),
        (f"{prefix}_Structure", struct, 0.55, 0.46, None),
        (f"{prefix}_Visor", (0.010, 0.012, 0.016, 1), 0.0, 0.10, None),
        (f"{prefix}_Glow", (0.04, 0.06, 0.07, 1), 0.0, 0.5, (glow, 4.0)),
    ]
    mats = []
    for name, base, metallic, rough, emission in specs:
        m = bpy.data.materials.new(name)
        m.use_backface_culling = True  # exported as single-sided: closed shells, half the fragments
        bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
        bsdf.inputs["Base Color"].default_value = base
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = rough
        if emission:
            bsdf.inputs["Emission Color"].default_value = emission[0]
            bsdf.inputs["Emission Strength"].default_value = emission[1]
        mats.append(m)
    return mats


# ----------------------------------------------------------------------------------------------
# Mesh assembly


def area(v, f):
    a = v[f[:, 1]] - v[f[:, 0]]
    b = v[f[:, 2]] - v[f[:, 0]]
    return 0.5 * float(np.linalg.norm(np.cross(a, b), axis=1).sum())


def decimate(v, f, target):
    """Collapse-decimates (v, f) to about `target` triangles with Blender's Decimate modifier."""
    me = bpy.data.meshes.new("tmp")
    me.from_pydata(v.tolist(), [], f.tolist())
    ob = bpy.data.objects.new("tmp", me)
    bpy.context.scene.collection.objects.link(ob)
    if len(f) > target:
        mod = ob.modifiers.new("dec", "DECIMATE")
        mod.decimate_type = "COLLAPSE"
        mod.ratio = target / len(f)
        mod.use_collapse_triangulate = True
    dg = bpy.context.evaluated_depsgraph_get()
    em = ob.evaluated_get(dg).to_mesh()
    em.calc_loop_triangles()
    out_v = np.array([vv.co[:] for vv in em.vertices])
    out_f = np.array([t.vertices[:] for t in em.loop_triangles], dtype=np.int64)
    ob.evaluated_get(dg).to_mesh_clear()
    bpy.data.objects.remove(ob)
    bpy.data.meshes.remove(me)
    return out_v, out_f


def weights_for(field, P, bone_parent):
    """Skin weights from the anatomy, limited to the dominant bone and its neighbours so the left
    leg never pulls on the right."""
    names, idx, w = mesher.skin_weights(field, P)
    children = {}
    for b, p in bone_parent.items():
        children.setdefault(p, set()).add(b)
    out = []
    for row_i, row_w in zip(idx, w):
        dom = names[row_i[0]]
        allowed = {dom, bone_parent.get(dom)} | children.get(dom, set())
        pairs = [(names[i], x) for i, x in zip(row_i, row_w) if x > 0 and names[i] in allowed]
        s = sum(x for _, x in pairs)
        out.append([(n, x / s) for n, x in pairs])
    return out


def build_character(key, variant, mats, rig, voxel):
    prefix = variant.name
    field = anatomy.body_field(variant)
    plates = {p.name: p for p in anatomy.plates(variant)}
    pieces = mesher.build(field, list(plates.values()), voxel=voxel)
    areas = [area(p["verts"], p["faces"]) * DENSITY.get(p["name"], 1.0) for p in pieces]
    total = sum(areas)
    bone_parent = {b[0]: b[1] for b in bone_table()}
    col = bpy.data.collections.new(prefix)
    bpy.context.scene.collection.children.link(col)
    objects = []
    for p, a in zip(pieces, areas):
        target = max(MIN_TRIS, int(TRIANGLE_BUDGET * a / total))
        v, f = decimate(p["verts"], p["faces"], target)
        if len(f) == 0:
            continue
        fn = field.eval if p["bone"] is None else mesher.plate_fn(field, plates[p["name"]])
        normals = gradient(fn, v)
        me = bpy.data.meshes.new(f"{prefix}_{p['name']}")
        me.from_pydata(v.tolist(), [], f.tolist())
        for m in mats:
            me.materials.append(m)
        for poly in me.polygons:
            poly.material_index = p["material"]
        me.shade_smooth()
        box_uvs(me)
        me.normals_split_custom_set_from_vertices([tuple(n) for n in normals])
        ob = bpy.data.objects.new(me.name, me)
        col.objects.link(ob)
        for vi, ws in enumerate(weights_for(field, v, bone_parent)):
            for bone, x in ws:
                g = ob.vertex_groups.get(bone) or ob.vertex_groups.new(name=bone)
                g.add([vi], x, "REPLACE")
        objects.append(ob)
    with bpy.context.temp_override(active_object=objects[0], selected_editable_objects=objects,
                                   selected_objects=objects):
        bpy.ops.object.join()
    body = objects[0]
    body.name = body.data.name = f"{prefix}_Body"
    body.parent = rig
    mod = body.modifiers.new("Armature", "ARMATURE")
    mod.object = rig
    return body


def gradient(fn, P, eps=3e-4):
    g = np.zeros_like(P)
    for i in range(3):
        o = np.zeros(3)
        o[i] = eps
        g[:, i] = fn(P + o) - fn(P - o)
    return g / np.maximum(np.linalg.norm(g, axis=1, keepdims=True), 1e-12)


def box_uvs(me):
    """Box projection on the dominant normal axis (1 UV unit = 0.25 m). The materials are
    untextured; UVs exist so tangent generation and later texturing have valid input."""
    uv = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        n = poly.normal
        axis = max(range(3), key=lambda i: abs(n[i]))
        a, c = [(1, 2), (0, 2), (0, 1)][axis]
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uv.data[li].uv = (co[a] * 4.0, co[c] * 4.0)


# ----------------------------------------------------------------------------------------------
# Animation. Rotations are authored in armature axes about the joint (like the procedural template
# robot); `pose()` converts them into pose-bone space.


def rx(deg):
    return Quaternion((1, 0, 0), math.radians(deg))


def yaw(deg):
    return Quaternion((0, 0, 1), math.radians(deg))


def roll_out(side, deg):
    """Rotates a hanging left (side=+1) or right (side=-1) limb outward by deg."""
    return Quaternion((0, 1, 0), math.radians(-side * deg))


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def ramp(u, a, b):
    return smooth((u - a) / (b - a))


def arm(side, pitch, out, elbow=-15.0, hand=-6.0, twist=0.0):
    """Upper arm pitch (+ = back) and outward angle from hanging straight down."""
    t = "L" if side > 0 else "R"
    return {
        f"UpperArm.{t}": rx(pitch) @ roll_out(side, out - ARM_REST_OUT) @ yaw(side * twist),
        f"LowerArm.{t}": rx(elbow),
        f"Hand.{t}": rx(hand),
    }


def leg(side, thigh, knee, foot_net, toe=0.0):
    """Thigh pitch (+ = back), knee flexion, and the foot's pitch relative to the ground."""
    t = "L" if side > 0 else "R"
    return {
        f"UpperLeg.{t}": rx(thigh),
        f"LowerLeg.{t}": rx(knee),
        f"Foot.{t}": rx(foot_net - thigh - knee),
        f"Toe.{t}": rx(toe),
    }


THIGH_LEN, SHIN_LEN = HIP[2] - KNEE_Z, KNEE_Z - ANKLE_Z


def leg_drop(thigh, knee):
    """How far the hips must drop for a foot to stay at its rest height."""
    a, b = math.radians(thigh), math.radians(thigh + knee)
    return THIGH_LEN * (1 - math.cos(a)) + SHIN_LEN * (1 - math.cos(b))


def foot_walk(u):
    """Foot pitch over a stride, u = 0 at heel strike: flat stance, heel-off, toe-off, swing."""
    if u < 0.08:
        return -8 * (1 - smooth(u / 0.08))
    if u < 0.33:
        return 0.0
    if u < 0.5:
        return 25 * smooth((u - 0.33) / 0.17)
    if u < 0.65:
        return 25 * (1 - smooth((u - 0.5) / 0.15))
    return -8 * smooth((u - 0.65) / 0.35)


def idle(u):
    p = 2 * math.pi * u
    breath = math.sin(2 * p)
    glance = ramp(u, 0.52, 0.62) * (1 - ramp(u, 0.80, 0.92))
    q = {
        "Hips": rx(0) @ yaw(1.5 * math.sin(p)),
        "Spine": rx(1.0),
        "Chest": rx(-1.2 * breath),
        "Neck": rx(1.0 + 0.6 * breath),
        "Head": yaw(16 * glance) @ rx(2 + 3 * glance),
        "Shoulder.L": roll_out(1, 1.5 * breath),
        "Shoulder.R": roll_out(-1, 1.5 * breath),
    }
    q.update(arm(1, 3 + 1.2 * breath, 9, elbow=-12, hand=-5))
    q.update(arm(-1, 3 + 1.2 * breath, 9, elbow=-12, hand=-5))
    q.update(leg(1, -4, 8, 0))
    q.update(leg(-1, -4, 8, 0))
    drop = leg_drop(-4, 8)
    loc = {"Hips": Vector((0.006 * math.sin(p), 0, -drop - 0.004 * (1 + breath) / 2))}
    return q, loc


def gait(u, run):
    p = 2 * math.pi * u
    c = math.cos(p)
    if run:
        sw, kn, lean, arm_sw, elbow, bob, base = 45.0, 75.0, 8.0, 45.0, -80.0, 0.05, 0.06
    else:
        sw, kn, lean, arm_sw, elbow, bob, base = 28.0, 40.0, 2.0, 24.0, -20.0, 0.025, 0.035
    q = {
        "Hips": yaw((-9 if run else -6) * c),
        "Spine": rx(lean) @ yaw((5 if run else 3) * c),
        "Chest": rx(6 if run else 2) @ yaw((12 if run else 6) * c),
        "Neck": rx(-4 if run else 0),
        "Head": rx(-8 if run else -2),
    }
    for side, ph in ((1, 0.0), (-1, 0.5)):
        uu = (u + ph) % 1.0
        pp = 2 * math.pi * uu
        thigh = (-10 if run else 0) - sw * math.cos(pp)
        knee = (15 if run else 6) + kn * max(0.0, -math.sin(pp))
        net = foot_walk(uu) * (1.3 if run else 1.0)
        q.update(leg(side, thigh, knee, net, toe=-0.7 * max(net, 0.0) if uu < 0.5 else 0.0))
        q.update(arm(side, arm_sw * math.cos(pp), 10 if run else 8, elbow=elbow,
                     hand=-10 if run else -6))
    loc = {"Hips": Vector((0, 0, -base - bob * math.cos(2 * p)))}
    return q, loc


def walk(u):
    return gait(u, False)


def run(u):
    return gait(u, True)


def jump(u):
    k = ramp(u, 0.1, 0.4)
    crouch = max(0.0, math.sin(min(u / 0.2, 1.0) * math.pi))
    q = {
        "Spine": rx(10 * crouch - 6 * k),
        "Chest": rx(4 * crouch - 4 * k),
        "Head": rx(-8 * k),
    }
    q.update(arm(1, 20 * crouch - 30 * k, 8 + 50 * k, elbow=-12 - 30 * k))
    q.update(arm(-1, 20 * crouch - 30 * k, 8 + 50 * k, elbow=-12 - 30 * k))
    q.update(leg(1, -25 * crouch - 40 * k, 40 * crouch + 70 * k, -10 * k))
    q.update(leg(-1, -25 * crouch - 20 * k, 40 * crouch + 45 * k, 5 * k))
    drop = leg_drop(-25 * crouch, 40 * crouch)
    loc = {"Hips": Vector((0, 0, -drop + 0.02 * k))}
    return q, loc


def fall(u):
    p = 2 * math.pi * u
    f = math.sin(p)
    q = {
        "Spine": rx(-4 + 1.5 * f),
        "Chest": rx(-3),
        "Head": rx(6 + 2 * f),
    }
    q.update(arm(1, -18 + 6 * f, 52 + 4 * f, elbow=-25, hand=4))
    q.update(arm(-1, -18 - 6 * f, 52 - 4 * f, elbow=-25, hand=4))
    q.update(leg(1, -28 + 4 * f, 38, -12))
    q.update(leg(-1, -8 - 4 * f, 22, -6))
    return q, {"Hips": Vector((0, 0, 0.0))}


def land(u):
    imp = smooth(u / 0.22) if u < 0.22 else 1 - 0.85 * smooth((u - 0.22) / 0.78)
    thigh, knee = -42 * imp - 4, 80 * imp + 8
    q = {
        "Spine": rx(16 * imp + 1),
        "Chest": rx(5 * imp),
        "Head": rx(-6 * imp + 2),
    }
    q.update(arm(1, -14 * imp + 3, 14 + 12 * imp, elbow=-12 - 25 * imp))
    q.update(arm(-1, -14 * imp + 3, 14 + 12 * imp, elbow=-12 - 25 * imp))
    q.update(leg(1, thigh, knee, 0))
    q.update(leg(-1, thigh, knee, 0))
    return q, {"Hips": Vector((0, 0, -leg_drop(thigh, knee)))}


def interact(u):
    e = ramp(u, 0.05, 0.38) * (1 - ramp(u, 0.62, 0.98))
    q, loc = idle(0.0)
    q.update({
        "Chest": rx(4 * e) @ yaw(-10 * e),
        "Head": rx(8 * e) @ yaw(-12 * e),
    })
    q.update(arm(-1, 3 - 72 * e, 9 + 6 * e, elbow=-12 - 20 * e, hand=-5 + 12 * e, twist=-10 * e))
    return q, loc


CLIPS = [  # name, seconds, sampler, loop
    ("Idle", 4.0, idle, True),
    ("Walk", 0.8, walk, True),
    ("Run", 0.55, run, True),
    ("Jump", 0.6, jump, False),
    ("Fall", 0.8, fall, True),
    ("Land", 0.4, land, False),
    ("Interact", 0.9, interact, False),
]


def bake_clips(rig):
    rest = {b.name: b.matrix_local.to_3x3() for b in rig.data.bones}
    rig.animation_data_create()
    prev = {}
    actions = []
    for name, seconds, sampler, loop in CLIPS:
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        rig.animation_data.action = act
        frames = round(seconds * FPS)
        for f in range(frames + 1):
            u = f / frames if loop else min(f / frames, 1.0)
            rots, locs = sampler(u % 1.0 if loop and f < frames else (0.0 if loop else u))
            for pb in rig.pose.bones:
                rm = rest[pb.name]
                d = rots.get(pb.name, Quaternion())
                basis = (rm.inverted() @ d.to_matrix() @ rm).to_quaternion()
                if pb.name in prev and prev[pb.name].dot(basis) < 0:
                    basis.negate()
                prev[pb.name] = basis
                pb.rotation_mode = "QUATERNION"
                pb.rotation_quaternion = basis
                pb.location = rm.inverted() @ locs.get(pb.name, Vector())
                pb.keyframe_insert("rotation_quaternion", frame=f)
                pb.keyframe_insert("location", frame=f)
        act.use_frame_range = True
        act.frame_start, act.frame_end = 0, frames
        act.use_cyclic = loop
        actions.append(act)
        prev.clear()
    rig.animation_data.action = None
    for act in actions:
        track = rig.animation_data.nla_tracks.new()
        track.name = act.name
        strip = track.strips.new(act.name, 0, act)
        strip.name = act.name
        track.mute = True
    for pb in rig.pose.bones:
        pb.rotation_quaternion = Quaternion()
        pb.location = Vector()


# ----------------------------------------------------------------------------------------------


def build_rig():
    data = bpy.data.armatures.new("Rig")
    rig = bpy.data.objects.new("Rig", data)
    col = bpy.data.collections.new("Rig")
    bpy.context.scene.collection.children.link(col)
    col.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    for name, parent, head, tail in bone_table():
        eb = data.edit_bones.new(name)
        eb.head, eb.tail, eb.roll = head, tail, 0.0
        eb.use_deform = True
        if parent:
            eb.parent = data.edit_bones[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    return rig, col


def build_sockets(rig, col):
    """Named attachment nodes (docs/CHARACTERS.md): children of bones, exported as glTF nodes."""
    sockets = [
        ("Socket.Head", "Head", (0, 0, 1.715)),
        ("Socket.Hand.L", "Hand.L", tuple(Vector((SHOULDER[0], 0, SHOULDER[2])) + Vector(arm_dir(1)) * (L_UPPER_ARM + L_FOREARM + 0.075))),
        ("Socket.Hand.R", "Hand.R", tuple(Vector((-SHOULDER[0], 0, SHOULDER[2])) + Vector(arm_dir(-1)) * (L_UPPER_ARM + L_FOREARM + 0.075))),
        ("Socket.Back", "Chest", (0, 0.16, 1.33)),
    ]
    for name, bone, pos in sockets:
        e = bpy.data.objects.new(name, None)
        e.empty_display_size = 0.05
        col.objects.link(e)
        e.parent = rig
        e.parent_type = "BONE"
        e.parent_bone = bone
        bpy.context.view_layer.update()
        e.matrix_world = Matrix.Translation(pos)
    root = bpy.data.objects.new("Socket.Root", None)
    root.empty_display_size = 0.1
    col.objects.link(root)
    root.parent = rig


def export(path, rig, body, sockets):
    for ob in bpy.context.scene.objects:
        ob.select_set(False)
    for ob in [rig, body] + sockets:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
        export_texcoords=True, export_normals=True, export_tangents=False, export_materials="EXPORT",
        export_vertex_color="ACTIVE", export_all_vertex_colors=False, export_cameras=False, export_lights=False,
        export_extras=False, export_skins=True, export_influence_nb=4, export_def_bones=True,
        export_rest_position_armature=True, export_reset_pose_bones=True, export_animations=True,
        export_animation_mode="ACTIONS", export_force_sampling=True, export_optimize_animation_size=False,
        export_anim_slide_to_zero=True, export_morph=False, export_image_format="NONE")




def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "engine", "project", "data",
                                                  "characters"))
    ap.add_argument("--voxel", type=float, default=0.0025, help="sculpt resolution in metres")
    ap.add_argument("--only", choices=sorted(anatomy.VARIANTS), help="build one character")
    ap.add_argument("--no-blend", action="store_true")
    args = ap.parse_args(argv)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, "source"), exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    rig, rig_col = build_rig()
    build_sockets(rig, rig_col)
    sockets = [o for o in rig_col.objects if o.name.startswith("Socket.")]
    bodies = {}
    for key, variant in anatomy.VARIANTS.items():
        if args.only and key != args.only:
            continue
        print(f"{variant.name}:")
        bodies[key] = build_character(key, variant, make_materials(variant.name, PALETTES[key]), rig, args.voxel)
    bake_clips(rig)
    for key, body in bodies.items():
        tris = sum(len(p.vertices) - 2 for p in body.data.polygons)
        print(f"{body.name}: {len(body.data.vertices)} vertices, {tris} triangles")
        export(os.path.join(out, f"victus_{key}.glb"), rig, body, sockets)
    if not args.no_blend and not args.only:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "source", "victus_characters.blend"), compress=True,
                                    copy=False)
        backup = os.path.join(out, "source", "victus_characters.blend1")
        if os.path.exists(backup):
            os.remove(backup)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]))
