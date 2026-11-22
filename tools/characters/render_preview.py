# SPDX-License-Identifier: Apache-2.0
"""Blender (Cycles) turnaround renders of the starter characters, the way docs/CHARACTERS.md shows
them: each GLB is imported into Blender, lit by a dark studio (key, two rim lights, a soft fill
and a graphite backdrop, like the ORION & NOVA design sheet) and rendered from several views.

Usage (the bpy environment of docs/CHARACTERS.md):
  python tools/characters/render_preview.py --glb engine/project/data/characters/victus_orion.glb
         --out /tmp/orion [--views front,side,back,three,head] [--samples 48] [--res 720]
  python tools/characters/render_preview.py --sheet docs/images/characters/blender_render_bodies.jpg
         --glb orion.glb --glb nova.glb      (one row of front / three-quarter / side / back views)

The renders are documentation and review aids; they are not part of the shipped characters.
"""
from __future__ import annotations

import argparse
import math
import os
import sys

import bpy
from mathutils import Vector

VIEWS = {
    # name: (azimuth degrees, 0 = in front of the character; camera height fraction; framing)
    "front": (0, 0.55, "body"),
    "three": (35, 0.58, "body"),
    "side": (90, 0.55, "body"),
    "back": (180, 0.55, "body"),
    "head": (22, 0.93, "head"),
    "headfront": (0, 0.93, "head"),
    "headside": (90, 0.93, "head"),
    "headback": (160, 0.93, "head"),
    "torso": (20, 0.75, "torso"),
    "torsoback": (180, 0.75, "torso"),
    "legs": (25, 0.28, "legs"),
}


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.use_denoising = True
    sc.view_settings.view_transform = "AgX"
    sc.view_settings.look = "AgX - Medium High Contrast"
    sc.render.film_transparent = False
    w = bpy.data.worlds.new("World")
    sc.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.012, 0.013, 0.015, 1)
    bg.inputs[1].default_value = 1.0
    return sc


def area(name, loc, rot, size, power, color=(1, 1, 1)):
    d = bpy.data.lights.new(name, "AREA")
    d.size = size
    d.energy = power
    d.color = color
    o = bpy.data.objects.new(name, d)
    o.location = loc
    o.rotation_euler = rot
    bpy.context.scene.collection.objects.link(o)
    return o


def look_at(obj, target):
    d = Vector(target) - obj.location
    obj.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


def studio(height):
    h = height
    k = area("Key", (1.8, -2.6, h * 1.35), (0, 0, 0), 1.6, 260)
    look_at(k, (0, 0, h * 0.6))
    f = area("Fill", (-2.8, -1.6, h * 0.9), (0, 0, 0), 3.0, 70, (0.85, 0.9, 1.0))
    look_at(f, (0, 0, h * 0.55))
    for s in (1, -1):
        r = area(f"Rim{s}", (s * 1.6, 2.2, h * 1.2), (0, 0, 0), 0.8, 300, (0.85, 0.92, 1.0))
        look_at(r, (0, 0, h * 0.6))
    top = area("Top", (0, 0.3, h + 1.6), (0, 0, 0), 1.2, 60)
    look_at(top, (0, 0, h * 0.5))
    # Floor and a curved backdrop in graphite.
    bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, 0))
    floor = bpy.context.active_object
    m = bpy.data.materials.new("Floor")
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (0.03, 0.032, 0.036, 1)
    b.inputs["Roughness"].default_value = 0.85
    floor.data.materials.append(m)
    bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 6, 0), rotation=(math.radians(90), 0, 0))
    wall = bpy.context.active_object
    wm = bpy.data.materials.new("Wall")
    wm.use_nodes = True
    wb = wm.node_tree.nodes["Principled BSDF"]
    wb.inputs["Base Color"].default_value = (0.05, 0.052, 0.058, 1)
    wb.inputs["Roughness"].default_value = 0.8
    wall.data.materials.append(wm)


def strip_to_suit(objs):
    """Deletes the Armor and Visor faces: the undersuit layer on its own."""
    import bmesh

    for o in objs:
        if o.type != "MESH":
            continue
        keep = {i for i, m in enumerate(o.data.materials) if m and m.name.endswith("_Frame")}
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index not in keep], context="FACES")
        bm.to_mesh(o.data)
        bm.free()


def import_glb(path, x=0.0, suit=False, turn=0.0):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    roots = [o for o in new if o.parent is None]
    for r in roots:
        r.location.x += x
        r.rotation_mode = "XYZ"
        r.rotation_euler.z += math.radians(turn)
    # Rest pose: no clip applied.
    for o in new:
        if o.animation_data:
            o.animation_data.action = None
        if o.type == "ARMATURE":
            for pb in o.pose.bones:
                pb.matrix_basis.identity()
    if suit:
        strip_to_suit(new)
    bpy.context.view_layer.update()
    zs = [ (o.matrix_world @ Vector(c)).z for o in new if o.type == "MESH" for c in o.bound_box]
    return new, max(zs)


def camera(sc, az, hfrac, framing, height, x=0.0, lens=85):
    cd = bpy.data.cameras.new("Cam")
    cd.lens = lens
    cam = bpy.data.objects.new("Cam", cd)
    sc.collection.objects.link(cam)
    sc.camera = cam
    span = {"body": height * 1.12, "head": 0.42, "torso": 0.85, "legs": 1.0}[framing]
    tz = {"body": height * 0.5, "head": height - 0.13, "torso": height * 0.68, "legs": height * 0.27}[framing]
    sensor = 36.0
    dist = span / (sensor / lens) * 1.0
    a = math.radians(az)
    cz = height * hfrac
    cam.location = (x + dist * math.sin(a), -dist * math.cos(a), cz)
    look_at(cam, (x, 0, tz))
    return cam


def render(sc, path, res_x, res_y, samples):
    sc.render.resolution_x = res_x
    sc.render.resolution_y = res_y
    sc.cycles.samples = samples
    sc.render.filepath = path
    sc.render.image_settings.file_format = "JPEG" if path.endswith(".jpg") else "PNG"
    bpy.ops.render.render(write_still=True)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--glb", action="append", required=True)
    ap.add_argument("--out", default="/tmp/preview")
    ap.add_argument("--views", default="front,three,side,back,head")
    ap.add_argument("--samples", type=int, default=48)
    ap.add_argument("--res", type=int, default=720)
    ap.add_argument("--suit", action="store_true", help="the undersuit only (Armor and Visor removed)")
    ap.add_argument("--lineup", help="render all GLBs side by side into this file (front and three-quarter)")
    a = ap.parse_args(argv)
    if a.lineup:
        sc = reset()
        n = len(a.glb)
        heights = []
        for i, g in enumerate(a.glb):
            _, h = import_glb(g, x=(i - (n - 1) / 2) * 1.5, suit=a.suit)
            heights.append(h)
        h = max(heights)
        studio(h)
        cd = bpy.data.cameras.new("Cam")
        cd.lens = 50
        cam = bpy.data.objects.new("Cam", cd)
        sc.collection.objects.link(cam)
        sc.camera = cam
        cam.location = (0, -4.6, h * 0.62)
        look_at(cam, (0, 0, h * 0.5))
        render(sc, a.lineup, int(a.res * 1.68), a.res, a.samples)
        return 0
    os.makedirs(a.out, exist_ok=True)
    for g in a.glb:
        stem = os.path.splitext(os.path.basename(g))[0]
        for v in a.views.split(","):
            sc = reset()
            az, hf, fr = VIEWS[v]
            # The character turns, not the camera: the studio lights stay where they flatter.
            _, h = import_glb(g, suit=a.suit, turn=-az)
            if a.suit:
                h = {"orion": 1.78, "nova": 1.69}.get(stem.split("_")[-1], h)
            studio(h)
            camera(sc, 0.0, hf, fr, h)
            ry = a.res
            rx = int(a.res * (0.6 if fr == "body" else 1.0))
            render(sc, os.path.join(a.out, f"{stem}_{v}.png"), rx, ry, a.samples)
    return 0


if __name__ == "__main__":
    i = sys.argv.index("--") + 1 if "--" in sys.argv else 1
    sys.exit(main(sys.argv[i:]))
