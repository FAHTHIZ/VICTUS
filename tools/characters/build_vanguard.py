# SPDX-License-Identifier: Apache-2.0
"""Build the VICTUS "Vanguard" starter characters (V-01 and V-02) with Blender's Python module.

Usage (Blender 5.x as a module, e.g. `pip install bpy==5.0.1` into a Python 3.11 venv):
  python tools/characters/build_vanguard.py [--out engine/project/data/characters] [--no-blend]

Everything is generated deterministically from this file: one shared 21-joint deform rig, two
hard-surface robot meshes rigidly skinned to it (one bone per part, weight 1), four shared
materials, seven clips stored as NLA strips, named socket nodes, and an editable .blend source.
Outputs:
  <out>/victus_vanguard_01.glb, <out>/victus_vanguard_02.glb, <out>/source/victus_vanguard.blend

Conventions (docs/CHARACTERS.md): 1.70 m tall, feet on the ground plane, facing +Z after the
glTF Y-up conversion (Blender -Y), character left on +X. Bone rest orientations are arbitrary
Blender bone frames; animation is authored as rotations expressed in armature axes about each
joint and converted to pose-bone space, so clips read like the procedural template robot's.

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
from mathutils import Matrix, Quaternion, Vector

FPS = 60
ARM_REST_OUT = 40.0  # A-pose: upper arms 40 degrees out from hanging straight down

# ----------------------------------------------------------------------------------------------
# Rig (Blender armature space: X = character left, -Y = forward, Z = up)

L_UPPER_ARM, L_FOREARM, L_HAND = 0.28, 0.255, 0.17
SHOULDER = Vector((0.20, 0.0, 1.38))
HIP = Vector((0.10, 0.0, 0.90))
KNEE_Z, ANKLE_Z = 0.48, 0.09


def arm_dir(side: float) -> Vector:
    a = math.radians(ARM_REST_OUT)
    return Vector((side * math.sin(a), 0.0, -math.cos(a)))


def bone_table():
    """(name, parent, head, tail) for every deform bone, parents first."""
    bones = [
        ("Hips", None, (0, 0, 0.92), (0, 0, 1.02)),
        ("Spine", "Hips", (0, 0, 1.02), (0, 0, 1.17)),
        ("Chest", "Spine", (0, 0, 1.17), (0, 0, 1.43)),
        ("Neck", "Chest", (0, 0, 1.45), (0, 0, 1.52)),
        ("Head", "Neck", (0, 0, 1.52), (0, 0, 1.70)),
    ]
    for tag, side in (("L", 1.0), ("R", -1.0)):
        d = arm_dir(side)
        s = Vector((side * SHOULDER.x, 0.0, SHOULDER.z))
        e = s + d * L_UPPER_ARM
        w = e + d * L_FOREARM
        h = w + d * L_HAND
        hip = Vector((side * HIP.x, 0.0, HIP.z))
        knee = Vector((side * HIP.x, 0.0, KNEE_Z))
        ankle = Vector((side * HIP.x, 0.0, ANKLE_Z))
        ball = Vector((side * HIP.x, -0.12, 0.03))
        toe = Vector((side * HIP.x, -0.20, 0.03))
        bones += [
            (f"Shoulder.{tag}", "Chest", (side * 0.05, 0, 1.39), (side * 0.17, 0, 1.39)),
            (f"UpperArm.{tag}", f"Shoulder.{tag}", tuple(s), tuple(e)),
            (f"LowerArm.{tag}", f"UpperArm.{tag}", tuple(e), tuple(w)),
            (f"Hand.{tag}", f"LowerArm.{tag}", tuple(w), tuple(h)),
            (f"UpperLeg.{tag}", "Hips", tuple(hip), tuple(knee)),
            (f"LowerLeg.{tag}", f"UpperLeg.{tag}", tuple(knee), tuple(ankle)),
            (f"Foot.{tag}", f"LowerLeg.{tag}", tuple(ankle), tuple(ball)),
            (f"Toe.{tag}", f"Foot.{tag}", tuple(ball), tuple(toe)),
        ]
    return bones


# ----------------------------------------------------------------------------------------------
# Materials. COLOR_0 multiplies base colour, so the one "Structure" metal serves both graphite
# and titanium parts.

GRAPHITE = (0.030, 0.036, 0.046, 1.0)
TITANIUM = (0.42, 0.43, 0.45, 1.0)
WHITE = (1.0, 1.0, 1.0, 1.0)
SHELL, STRUCT, VISOR, GLOW = 0, 1, 2, 3


def make_materials():
    specs = [
        ("Vanguard_Shell", (0.80, 0.79, 0.755, 1), 0.0, 0.38, None),
        ("Vanguard_Structure", (1, 1, 1, 1), 0.65, 0.42, None),
        ("Vanguard_Visor", (0.012, 0.016, 0.022, 1), 0.0, 0.12, None),
        ("Vanguard_Glow", (0.05, 0.12, 0.14, 1), 0.0, 0.5, ((0.55, 0.93, 1.0, 1), 3.0)),
    ]
    mats = []
    for name, base, metallic, rough, emission in specs:
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        nt = m.node_tree
        bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = rough
        if emission:
            bsdf.inputs["Emission Color"].default_value = emission[0]
            bsdf.inputs["Emission Strength"].default_value = emission[1]
        # Structure is white; its graphite/titanium tint lives in the "Col" attribute (COLOR_0).
        bsdf.inputs["Base Color"].default_value = base
        mats.append(m)
    return mats


# ----------------------------------------------------------------------------------------------
# Geometry helpers. Every part is a closed bmesh built in a local frame, transformed, then turned
# into an object with one vertex group (its bone) and joined per character.


def bevel_sharp(bm, width, segments, angle=50.0):
    edges = [e for e in bm.edges if e.is_manifold and e.calc_face_angle(0.0) > math.radians(angle)]
    if edges and width > 0:
        bmesh.ops.bevel(bm, geom=edges, offset=width, offset_type="OFFSET", segments=segments,
                        profile=0.5, affect="EDGES", clamp_overlap=True)


def slice_axis(bm, axis, cuts, size):
    for i in range(1, cuts + 1):
        co = Vector((0, 0, 0))
        co[axis] = -size / 2 + size * i / (cuts + 1)
        no = Vector((0, 0, 0))
        no[axis] = 1
        geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=co, plane_no=no)


def rbox(sx, sy, sz, bevel=0.02, segs=3, taper_top=(1.0, 1.0), taper_bottom=(1.0, 1.0), cuts=(0, 0, 0)):
    """Rounded box centred at the origin. taper_* scale X/Y at the top/bottom faces."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0, calc_uvs=True)
    bmesh.ops.scale(bm, vec=(sx, sy, sz), verts=bm.verts)
    for axis, n in enumerate(cuts):
        if n:
            slice_axis(bm, axis, n, (sx, sy, sz)[axis])
    for v in bm.verts:
        t = v.co.z / sz + 0.5
        v.co.x *= taper_bottom[0] + (taper_top[0] - taper_bottom[0]) * t
        v.co.y *= taper_bottom[1] + (taper_top[1] - taper_bottom[1]) * t
    bevel_sharp(bm, min(bevel, 0.49 * min(sx, sy, sz)), segs)
    return bm


def ellipsoid(rx, ry, rz, seg=24, rings=14):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=rings, radius=1.0, calc_uvs=True)
    bmesh.ops.scale(bm, vec=(rx, ry, rz), verts=bm.verts)
    return bm


def cylinder(r, depth, seg=24, bevel=0.006, axis="Z"):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=seg, radius1=r, radius2=r, depth=depth, calc_uvs=True)
    bevel_sharp(bm, bevel, 2, angle=60.0)
    if axis == "X":
        bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.radians(90), 3, "Y"))
    elif axis == "Y":
        bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.radians(90), 3, "X"))
    return bm


def hex_prism(r, depth, bevel):
    """Rounded hexagon facing -Y (forward), flat top and bottom edges."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=6, radius1=r, radius2=r, depth=depth, calc_uvs=True)
    bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.radians(30), 3, "Z"))
    vertical = [e for e in bm.edges if abs((e.verts[0].co - e.verts[1].co).normalized().z) > 0.99]
    bmesh.ops.bevel(bm, geom=vertical, offset=bevel, offset_type="OFFSET", segments=3, profile=0.5,
                    affect="EDGES", clamp_overlap=True)
    bevel_sharp(bm, depth * 0.3, 1, angle=60.0)
    bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.radians(90), 3, "X"))
    return bm


def wrap_y(bm, radius, centre_y):
    """Bends a part lying in the XZ plane (front at -Y) around a vertical axis at y = centre_y."""
    for v in bm.verts:
        a = v.co.x / radius
        r = radius - v.co.y  # local -y is outward
        v.co.x = r * math.sin(a)
        v.co.y = centre_y - r * math.cos(a)


def split_shell(bm, plane_co, plane_no, gap):
    """Cuts a closed shell along a plane into two capped pieces; the negative side moves by -gap."""
    other = bm.copy()
    n = Vector(plane_no).normalized()
    for b, clear_inner in ((bm, True), (other, False)):
        geom = b.verts[:] + b.edges[:] + b.faces[:]
        res = bmesh.ops.bisect_plane(b, geom=geom, plane_co=plane_co, plane_no=n, clear_inner=clear_inner,
                                     clear_outer=not clear_inner)
        cut = [e for e in res["geom_cut"] if isinstance(e, bmesh.types.BMEdge)]
        bmesh.ops.holes_fill(b, edges=cut, sides=0)
    bmesh.ops.translate(other, vec=-n * gap, verts=other.verts)
    bmesh.ops.scale(other, vec=(0.985, 0.985, 0.985), verts=other.verts,
                    space=Matrix.Translation(-Vector(plane_co)))
    return bm, other


def T(x=0.0, y=0.0, z=0.0):
    return Matrix.Translation((x, y, z))


def R(deg, axis):
    return Matrix.Rotation(math.radians(deg), 4, axis)


class Builder:
    """Accumulates parts for one character, then joins them into one skinned mesh object."""

    MIRROR = Matrix.Scale(-1.0, 4, (1, 0, 0))

    def __init__(self, prefix, mats, collection):
        self.prefix, self.mats, self.collection = prefix, mats, collection
        self.objects = []

    def add(self, name, bm, bone, mat, matrix=Matrix.Identity(4), color=WHITE, mirror=False):
        """Adds `bm` transformed by `matrix`. mirror=True also adds the right-hand (".R") copy."""
        self._emit(name, bm.copy(), bone, mat, matrix, color)
        if mirror:
            r = bm.copy()
            self._emit(name.replace(".L", ".R"), r, bone.replace(".L", ".R"), mat, self.MIRROR @ matrix, color,
                       flip=True)
        bm.free()

    def _emit(self, name, bm, bone, mat, matrix, color, flip=False):
        bmesh.ops.transform(bm, matrix=matrix, verts=bm.verts)
        if flip:
            bmesh.ops.reverse_faces(bm, faces=bm.faces)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        uv = bm.loops.layers.uv.verify()
        for f in bm.faces:
            f.material_index = mat
            f.smooth = True
            # Box projection on the dominant normal axis: 1 UV unit = 0.25 m. The materials are
            # untextured; UVs exist so tangent generation and later texturing have valid input.
            n = f.normal
            axis = max(range(3), key=lambda i: abs(n[i]))
            a, c = [(1, 2), (0, 2), (0, 1)][axis]
            for loop in f.loops:
                loop[uv].uv = (loop.vert.co[a] * 4.0, loop.vert.co[c] * 4.0)
        me = bpy.data.meshes.new(f"{self.prefix}_{name}")
        bm.to_mesh(me)
        bm.free()
        for m in self.mats:
            me.materials.append(m)
        col = me.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
        for d in col.data:
            d.color = color
        ob = bpy.data.objects.new(f"{self.prefix}_{name}", me)
        self.collection.objects.link(ob)
        vg = ob.vertex_groups.new(name=bone)
        vg.add(list(range(len(me.vertices))), 1.0, "REPLACE")
        self.objects.append(ob)

    def join(self, rig):
        target = self.objects[0]
        with bpy.context.temp_override(active_object=target, selected_editable_objects=self.objects,
                                       selected_objects=self.objects):
            bpy.ops.object.join()
        target.name = f"{self.prefix}_Body"
        target.data.name = f"{self.prefix}_Body"
        target.data.color_attributes.active_color = target.data.color_attributes["Col"]
        target.parent = rig
        mod = target.modifiers.new("Armature", "ARMATURE")
        mod.object = rig
        wn = target.modifiers.new("WeightedNormals", "WEIGHTED_NORMAL")
        wn.keep_sharp = True
        wn.weight = 50
        return target


# ----------------------------------------------------------------------------------------------
# Character designs. `v` holds the per-variant proportions; joint centres never change.


def variant(masc: bool):
    return dict(
        masc=masc,
        chest_w=0.40 if masc else 0.335,
        chest_d=0.25 if masc else 0.23,
        chest_bevel=0.035 if masc else 0.062,
        chest_taper=0.84 if masc else 0.76,
        pelvis_w=0.30 if masc else 0.285,
        cap_w=0.150 if masc else 0.125,
        cap_tilt=18.0 if masc else 26.0,
        uarm=(0.108, 0.112, 0.86) if masc else (0.094, 0.100, 0.80),
        farm=(0.092, 0.100, 0.80) if masc else (0.082, 0.090, 0.70),
        thigh=(0.140, 0.150, 0.82) if masc else (0.126, 0.138, 0.76),
        shin=(0.118, 0.132, 0.76) if masc else (0.106, 0.122, 0.68),
        head_bevel=0.045 if masc else 0.07,
        limb_bevel=0.024 if masc else 0.034,
    )


def build_head(b: Builder, v):
    hz = 1.605  # head shell centre; shell spans 1.51..1.70
    head = rbox(0.205, 0.225, 0.19, bevel=v["head_bevel"], segs=5,
                taper_top=(0.93 if v["masc"] else 0.88, 0.95), taper_bottom=(0.9, 0.92), cuts=(1, 1, 1))
    b.add("HeadShell", head, "Head", SHELL, T(0, 0.005, hz))
    # Graphite jaw/underside block so the head reads as a shell over structure.
    b.add("HeadCore", rbox(0.16, 0.17, 0.07, bevel=0.02, segs=3), "Head", STRUCT, T(0, 0.01, 1.515), GRAPHITE)
    # Primary recognition anchor: one dark curved visor band across the face.
    radius, centre = 0.17, 0.058
    visor = rbox(0.235, 0.022, 0.074, bevel=0.009, segs=3, cuts=(14, 0, 0))
    wrap_y(visor, radius, centre)
    b.add("Visor", visor, "Head", VISOR, T(0, 0, 1.618))
    for side in (1.0, -1.0):
        eye = rbox(0.032, 0.008, 0.015, bevel=0.0035, segs=2)
        a = side * 0.042 / radius
        pos = Vector((radius * math.sin(a) * 1.07, centre - (radius + 0.022) * math.cos(a), 1.621))
        b.add("Eye.L" if side > 0 else "Eye.R", eye, "Head", GLOW, T(*pos) @ R(-math.degrees(a), "Z"))
    # Side pods (titanium) where ears would be.
    pod = cylinder(0.036, 0.022, seg=28, bevel=0.005, axis="X")
    b.add("EarPod.L", pod, "Head", STRUCT, T(0.108, 0.012, 1.598), TITANIUM, mirror=True)
    if v["masc"]:
        # Squared crown with a single rear fin/vent.
        fin = rbox(0.018, 0.11, 0.045, bevel=0.007, segs=3, taper_top=(0.7, 0.7))
        b.add("CrownFin", fin, "Head", STRUCT, T(0, 0.055, 1.695) @ R(-12, "X"), TITANIUM)
        vent = rbox(0.09, 0.012, 0.03, bevel=0.005, segs=2)
        b.add("RearVent", vent, "Head", STRUCT, T(0, 0.118, 1.60), GRAPHITE)
    else:
        # Softer crown with paired swept fins and a paired rear status-light motif.
        fin = rbox(0.014, 0.09, 0.036, bevel=0.006, segs=3, taper_top=(0.6, 0.6))
        b.add("SideFin.L", fin, "Head", STRUCT, T(0.066, 0.07, 1.676) @ R(-18, "X") @ R(-10, "Y"), TITANIUM,
              mirror=True)
        light = rbox(0.006, 0.026, 0.012, bevel=0.002, segs=1)
        b.add("StatusLight.L", light, "Head", GLOW, T(0.104, 0.075, 1.585), mirror=True)


def build_torso(b: Builder, v):
    w, d = v["chest_w"], v["chest_d"]
    cz = 1.31
    if v["masc"]:
        # Angular two-piece chest shell: two plates meeting in a centre seam.
        for side in (1.0, -1.0):
            plate = rbox(w / 2 - 0.004, d, 0.27, bevel=v["chest_bevel"], segs=4, taper_top=(1.0, 1.0),
                         taper_bottom=(v["chest_taper"], 0.92), cuts=(1, 1, 2))
            b.add("ChestPlate.L" if side > 0 else "ChestPlate.R", plate, "Chest", SHELL,
                  T(side * (w / 4 + 0.002), 0, cz))
        b.add("ChestSeam", rbox(0.012, d - 0.02, 0.25, bevel=0.004, segs=1), "Chest", STRUCT, T(0, 0, cz),
              GRAPHITE)
    else:
        # Continuous chest shell with gently curved side transitions.
        shell = rbox(w, d, 0.27, bevel=v["chest_bevel"], segs=6, taper_bottom=(v["chest_taper"], 0.9),
                     cuts=(2, 1, 2))
        b.add("ChestShell", shell, "Chest", SHELL, T(0, 0, cz))
    front = -d / 2
    # Recessed rounded-hexagon status window high on the chest.
    b.add("CoreFrame", hex_prism(0.046, 0.016, 0.012), "Chest", STRUCT, T(0, front - 0.002, 1.365), GRAPHITE)
    b.add("CoreGlow", hex_prism(0.032, 0.012, 0.008), "Chest", GLOW, T(0, front - 0.006, 1.365))
    # Original VICTUS insignia: a "V" of two unequal bars meeting at the bottom on the upper left chest.
    ix = 0.115 if v["masc"] else 0.088
    long_bar = rbox(0.011, 0.006, 0.052, bevel=0.002, segs=1)
    short_bar = rbox(0.011, 0.006, 0.038, bevel=0.002, segs=1)
    b.add("Insignia.A", long_bar, "Chest", STRUCT, T(ix - 0.010, front - 0.004, 1.405) @ R(-24, "Y"), TITANIUM)
    b.add("Insignia.B", short_bar, "Chest", STRUCT, T(ix + 0.009, front - 0.004, 1.398) @ R(24, "Y"), TITANIUM)
    # Back structure and the upper-back mount plate.
    b.add("BackFrame", rbox(w * 0.7, 0.06, 0.22, bevel=0.015, segs=2), "Chest", STRUCT, T(0, d / 2 - 0.01, 1.31),
          GRAPHITE)
    b.add("BackMount", rbox(0.11, 0.025, 0.12, bevel=0.01, segs=2), "Chest", STRUCT, T(0, d / 2 + 0.025, 1.33),
          TITANIUM)
    # Collar and neck.
    b.add("Collar", cylinder(0.075, 0.03, seg=32, bevel=0.008), "Chest", STRUCT, T(0, 0.005, 1.455), GRAPHITE)
    b.add("NeckCore", cylinder(0.042, 0.09, seg=24, bevel=0.004), "Neck", STRUCT, T(0, 0.005, 1.49), GRAPHITE)
    b.add("NeckRing", cylinder(0.05, 0.016, seg=32, bevel=0.004), "Neck", STRUCT, T(0, 0.005, 1.475), TITANIUM)
    # Abdomen: graphite core with two titanium bands.
    b.add("Abdomen", rbox(0.19, 0.15, 0.17, bevel=0.03, segs=3), "Spine", STRUCT, T(0, 0.005, 1.10), GRAPHITE)
    b.add("AbBand", rbox(0.205, 0.16, 0.022, bevel=0.009, segs=2), "Spine", STRUCT, T(0, 0.005, 1.075), TITANIUM)
    b.add("AbPlate", rbox(0.12, 0.03, 0.09, bevel=0.012, segs=3), "Spine", SHELL, T(0, -0.075, 1.125))
    # Pelvis shell.
    pelvis = rbox(v["pelvis_w"], 0.2, 0.12, bevel=0.035, segs=4, taper_bottom=(0.78, 0.9), cuts=(1, 0, 0))
    b.add("Pelvis", pelvis, "Hips", SHELL, T(0, 0.005, 0.955))
    b.add("PelvisCore", rbox(0.2, 0.16, 0.08, bevel=0.02, segs=2), "Hips", STRUCT, T(0, 0.005, 0.89), GRAPHITE)


def build_arm(b: Builder, v):
    """Left arm modelled hanging along -Z from the shoulder, then posed into the A-pose."""
    s = Vector((SHOULDER.x, 0, SHOULDER.z))
    # Shoulder mount and split-shell cap stay on the clavicle (Shoulder) bone.
    b.add("Clavicle.L", rbox(0.11, 0.09, 0.07, bevel=0.02, segs=2), "Shoulder.L", STRUCT, T(0.13, 0, 1.385),
          GRAPHITE, mirror=True)
    cap = rbox(v["cap_w"], 0.165, 0.10, bevel=0.042, segs=5, taper_bottom=(1.0, 0.86), cuts=(1, 1, 1))
    upper, lower = split_shell(cap, (0.012, 0, -0.004), (-0.62, 0.12, 0.78), 0.012)
    cap_m = T(s.x + 0.025, 0, s.z + 0.04) @ R(-v["cap_tilt"], "Y")
    b.add("ShoulderCap.L", upper, "Shoulder.L", SHELL, cap_m, mirror=True)
    b.add("ShoulderCapLower.L", lower, "Shoulder.L", SHELL, cap_m, mirror=True)
    cap.free()

    pose = T(*s) @ R(-ARM_REST_OUT, "Y")  # local -Z arm axis -> A-pose direction
    b.add("ShoulderBall.L", ellipsoid(0.052, 0.052, 0.052), "UpperArm.L", STRUCT, pose, GRAPHITE, mirror=True)
    ux, uy, ut = v["uarm"]
    b.add("UpperArmShell.L", rbox(ux, uy, 0.19, bevel=v["limb_bevel"], segs=4, taper_bottom=(ut, ut), cuts=(0, 0, 1)),
          "UpperArm.L", SHELL, pose @ T(0, 0, -0.145), mirror=True)
    b.add("UpperArmCore.L", cylinder(0.034, 0.08, seg=20), "UpperArm.L", STRUCT, pose @ T(0, 0, -0.25), GRAPHITE,
          mirror=True)
    if v["masc"]:
        # Asymmetric service panel on the left upper arm only.
        b.add("ServicePanel", rbox(0.012, 0.05, 0.07, bevel=0.004, segs=2), "UpperArm.L", STRUCT,
              pose @ T(ux * 0.5 * (1 + ut) / 2 + 0.001, 0, -0.14), TITANIUM)
    e = -L_UPPER_ARM
    b.add("Elbow.L", cylinder(0.040, 0.086, seg=24, bevel=0.006, axis="X"), "LowerArm.L", STRUCT, pose @ T(0, 0, e),
          GRAPHITE, mirror=True)
    fx, fy, ft = v["farm"]
    b.add("ForearmShell.L", rbox(fx, fy, 0.18, bevel=v["limb_bevel"] * 0.9, segs=4, taper_bottom=(ft, ft),
                                 cuts=(0, 0, 1)), "LowerArm.L", SHELL, pose @ T(0, 0, e - 0.115), mirror=True)
    b.add("Cuff.L", rbox(fx * ft + 0.006, fy * ft + 0.006, 0.024, bevel=0.008, segs=2), "LowerArm.L", STRUCT,
          pose @ T(0, 0, e - 0.215), TITANIUM, mirror=True)
    w = e - L_FOREARM
    b.add("Wrist.L", ellipsoid(0.03, 0.03, 0.03, seg=20, rings=12), "Hand.L", STRUCT, pose @ T(0, 0, w), GRAPHITE,
          mirror=True)
    b.add("Palm.L", rbox(0.036, 0.082, 0.085, bevel=0.012, segs=3), "Hand.L", STRUCT, pose @ T(0, 0, w - 0.05),
          GRAPHITE, mirror=True)
    b.add("HandPlate.L", rbox(0.012, 0.07, 0.06, bevel=0.005, segs=2), "Hand.L", SHELL,
          pose @ T(0.022, 0, w - 0.05), mirror=True)
    b.add("Fingers.L", rbox(0.03, 0.076, 0.07, bevel=0.01, segs=3, taper_bottom=(0.8, 0.95)), "Hand.L", STRUCT,
          pose @ T(-0.004, 0, w - 0.125) @ R(-8, "Y"), GRAPHITE, mirror=True)
    b.add("Thumb.L", rbox(0.024, 0.022, 0.05, bevel=0.008, segs=2), "Hand.L", STRUCT,
          pose @ T(-0.012, -0.045, w - 0.07) @ R(18, "X"), TITANIUM, mirror=True)


def build_leg(b: Builder, v):
    x = HIP.x
    b.add("HipBall.L", ellipsoid(0.06, 0.06, 0.06), "UpperLeg.L", STRUCT, T(x, 0, HIP.z), GRAPHITE, mirror=True)
    tx, ty, tt = v["thigh"]
    b.add("ThighShell.L", rbox(tx, ty, 0.30, bevel=v["limb_bevel"] + 0.006, segs=4, taper_bottom=(tt, tt),
                               cuts=(0, 0, 1)), "UpperLeg.L", SHELL, T(x, 0, 0.69), mirror=True)
    b.add("ThighCore.L", cylinder(0.042, 0.12, seg=20), "UpperLeg.L", STRUCT, T(x, 0.005, 0.53), GRAPHITE,
          mirror=True)
    b.add("Knee.L", cylinder(0.05, 0.10, seg=28, bevel=0.007, axis="X"), "LowerLeg.L", STRUCT, T(x, 0, KNEE_Z),
          GRAPHITE, mirror=True)
    b.add("KneeCap.L", rbox(0.075, 0.032, 0.085, bevel=0.012, segs=3), "LowerLeg.L", STRUCT,
          T(x, -0.058, KNEE_Z + 0.005), TITANIUM, mirror=True)
    sx, sy, st = v["shin"]
    b.add("ShinShell.L", rbox(sx, sy, 0.29, bevel=v["limb_bevel"] + 0.004, segs=4, taper_bottom=(st, st),
                              cuts=(0, 0, 1)), "LowerLeg.L", SHELL, T(x, -0.004, 0.285), mirror=True)
    b.add("ShinCore.L", cylinder(0.036, 0.09, seg=20), "LowerLeg.L", STRUCT, T(x, 0.004, 0.13), GRAPHITE,
          mirror=True)
    b.add("Ankle.L", ellipsoid(0.04, 0.04, 0.04, seg=20, rings=12), "Foot.L", STRUCT, T(x, 0, ANKLE_Z), GRAPHITE,
          mirror=True)
    b.add("FootShell.L", rbox(0.10, 0.17, 0.065, bevel=0.022, segs=3, taper_top=(0.9, 0.85)), "Foot.L", SHELL,
          T(x, -0.03, 0.058), mirror=True)
    b.add("Sole.L", rbox(0.106, 0.19, 0.026, bevel=0.008, segs=2), "Foot.L", STRUCT, T(x, -0.025, 0.013),
          GRAPHITE, mirror=True)
    b.add("ToeShell.L", rbox(0.096, 0.08, 0.05, bevel=0.018, segs=3, taper_top=(0.9, 0.8)), "Toe.L", SHELL,
          T(x, -0.16, 0.033), mirror=True)
    b.add("ToeSole.L", rbox(0.1, 0.082, 0.018, bevel=0.006, segs=2), "Toe.L", STRUCT, T(x, -0.16, 0.009),
          GRAPHITE, mirror=True)


def build_character(prefix, masc, mats, rig):
    col = bpy.data.collections.new(prefix)
    bpy.context.scene.collection.children.link(col)
    b = Builder(prefix, mats, col)
    v = variant(masc)
    build_head(b, v)
    build_torso(b, v)
    build_arm(b, v)
    build_leg(b, v)
    return b.join(rig), col


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


THIGH_LEN, SHIN_LEN = HIP.z - KNEE_Z, KNEE_Z - ANKLE_Z


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
    data = bpy.data.armatures.new("Vanguard")
    rig = bpy.data.objects.new("Vanguard", data)
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
        ("Socket.Hand.L", "Hand.L", tuple(Vector((SHOULDER.x, 0, SHOULDER.z)) + arm_dir(1) * (L_UPPER_ARM + L_FOREARM + 0.075))),
        ("Socket.Hand.R", "Hand.R", tuple(Vector((-SHOULDER.x, 0, SHOULDER.z)) + arm_dir(-1) * (L_UPPER_ARM + L_FOREARM + 0.075))),
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
    ap.add_argument("--no-blend", action="store_true")
    args = ap.parse_args(argv)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, "source"), exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    mats = make_materials()
    rig, rig_col = build_rig()
    build_sockets(rig, rig_col)
    sockets = [o for o in rig_col.objects if o.name.startswith("Socket.")]
    v01, _ = build_character("V01", True, mats, rig)
    v02, _ = build_character("V02", False, mats, rig)
    bake_clips(rig)
    for body in (v01, v02):
        tris = sum(len(p.vertices) - 2 for p in body.data.polygons)
        print(f"{body.name}: {len(body.data.vertices)} vertices, {tris} triangles")
    export(os.path.join(out, "victus_vanguard_01.glb"), rig, v01, sockets)
    export(os.path.join(out, "victus_vanguard_02.glb"), rig, v02, sockets)
    if not args.no_blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "source", "victus_vanguard.blend"), compress=True,
                                    copy=False)
        backup = os.path.join(out, "source", "victus_vanguard.blend1")
        if os.path.exists(backup):
            os.remove(backup)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]))
