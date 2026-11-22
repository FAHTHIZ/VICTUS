#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Builds the Robot Arena sample from a fresh VICTUS 3D template project.

    victus new /tmp/arena_template --template 3d
    python3 samples/arena/tools/generate_arena.py /tmp/arena_template samples/arena

Copies the template's art (the Orion and Nova starter characters, ground texture and material, energy shader, impact
sound, HUD script) with their .meta files so asset UUIDs stay stable, reuses the template's
Player, Sun, Environment and HUD entities, and writes the arena level, the enemy robot and
health orb prefabs, scenes/main.vscene, scenes/autoplay.vscene and victus.project. The
hand-written game scripts in assets/scripts are left untouched. UUIDs are derived from names
(uuid5) so regenerating produces identical files.
"""
import copy
import json
import shutil
import sys
import uuid
from pathlib import Path

NS = uuid.UUID("0b8f7c52-3a5e-4c0c-9a51-7e1f2a6d4b10")
GROUND_MATERIAL = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f003"
ENERGY_SHADER = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f004"
GROUND_TEXTURE = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f002"
# CC0 starter materials the 3D template writes (engine/project/src/starter_materials.cpp, ADR-0197).
STARTER = {"rock": "41d1b30b-56c2-4579-aa04-8db688955042", "planks": "d2db09c1-abda-4fb5-9ab7-65355bad2577",
           "paving": "126386bf-b6d7-49b8-a925-f68faa031016"}


def plain_clips(entities):
    for e in entities:
        animator = e["components"].get("Animator")
        if animator:
            animator["controller"] = None
            animator["clip"] = "Idle"


def uid(name):
    return str(uuid.uuid5(NS, name))


def transform(pos=(0, 0, 0), scale=(1, 1, 1), rot=(0, 0, 0, 1)):
    return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}


def collider(shape, layer=0):
    return {"shape": shape, "size": [1, 1, 1], "radius": 0.5, "height": 1, "offset": [0, 0, 0],
            "friction": 0.5, "restitution": 0, "is_trigger": False, "layer": layer, "mask": 65535}


def static_body():
    return {"type": "static", "mass": 1, "linear_damping": 0.05, "angular_damping": 0.05, "gravity_scale": 1,
            "continuous": False, "lock_rotation": False, "start_awake": True, "linear_velocity": [0, 0, 0],
            "angular_velocity": [0, 0, 0]}


def mesh(primitive, color, metallic=0.0, roughness=0.8, material=None):
    m = {"primitive": primitive, "base_color": list(color) + [1] * (4 - len(color)), "metallic": metallic,
         "roughness": roughness, "cast_shadows": True, "visible": True}
    if material:
        m["material"] = material
    return m


def solid(name, primitive, pos, scale, color, metallic=0.0, roughness=0.8, material=None):
    shape = {"cube": "box", "cylinder": "cylinder"}[primitive]
    return {"uuid": uid(name), "name": name, "components": {
        "Collider": collider(shape), "MeshRenderer": mesh(primitive, color, metallic, roughness, material),
        "RigidBody": static_body(), "Transform": transform(pos, scale)}}


def point_light(name, pos, color, intensity, rng, shadows=False):
    return {"uuid": uid(name), "name": name, "components": {
        "Light": {"type": "point", "color": list(color) + [1], "intensity": intensity, "range": rng, "shadows": shadows},
        "Transform": transform(pos)}}


def emitter(template, name, parent=None, pos=(0, 0, 0), **overrides):
    e = copy.deepcopy(template)
    e["uuid"] = uid(name)
    e["name"] = name
    if parent:
        e["parent"] = parent
    else:
        e.pop("parent", None)
    p = e["components"]["ParticleEmitter"]
    p.update(overrides)
    e["components"]["Transform"] = transform(pos)
    return e


def subtree(entities, root_uuid):
    """The entity with `root_uuid` followed by all its descendants, in file order."""
    keep = {root_uuid}
    out = []
    for e in entities:
        if e["uuid"] in keep or e.get("parent") in keep:
            keep.add(e["uuid"])
            out.append(copy.deepcopy(e))
    return out


def remap(entities, prefix):
    """Gives every entity of a subtree a fresh name-derived UUID, fixing parent and skeleton links."""
    table = {e["uuid"]: uid(f"{prefix}/{i}/{e['name']}") for i, e in enumerate(entities)}

    def fix(v):
        if isinstance(v, str) and v in table:
            return table[v]
        if isinstance(v, list):
            return [fix(x) for x in v]
        if isinstance(v, dict):
            return {k: fix(x) for k, x in v.items()}
        return v

    return [fix(e) for e in entities]


def by_name(entities, name):
    return next(e for e in entities if e["name"] == name)


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


def write_meta(path, guid):
    meta = Path(str(path) + ".meta")
    if not meta.exists():
        write_json(meta, {"victus_meta": 1, "uuid": guid, "importer": ""})


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    a_src, a_dst = src / "assets", dst / "assets"

    for rel in ["models/characters/victus_orion.glb", "models/characters/victus_nova.glb", "materials/ground.vmat", "shaders/energy.vshader", "textures/ground_grid.png",
                "audio/impact.wav", "scripts/hud.luau"]:
        for suffix in ["", ".meta"]:
            # Newer templates leave some .meta files to the editor: keep the sample's own then.
            if suffix and not (a_src / (rel + suffix)).exists():
                continue
            (a_dst / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a_src / (rel + suffix), a_dst / (rel + suffix))

    # Starter materials (triplanar, so they tile per metre on the scaled walls) and the HDR sky.
    starter = [f"textures/starter/{m}_{k}.jpg" for m in STARTER for k in ("color", "normal", "orm")]
    starter += [f"materials/starter/{m}.vmat" for m in STARTER] + ["textures/sky/partly_cloudy.hdr"]
    for rel in starter:
        for suffix in ["", ".meta"]:
            (a_dst / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a_src / (rel + suffix), a_dst / (rel + suffix))

    # The arena floor is larger than the template's 20 m ground: keep the grid at 2 m cells.
    ground = json.loads((a_src / "materials/ground.vmat").read_text())
    ground["uv_scale"] = [16, 16]
    ground["base_color"] = [0.78, 0.8, 0.86, 1]
    write_json(a_dst / "materials/ground.vmat", ground)

    # Glowing materials driven by the template's energy shader (params: glow colour + strength, pulse).
    energy = json.loads((a_src / "materials/energy_cube.vmat").read_text())
    orb = copy.deepcopy(energy)
    orb["base_color"] = [0.15, 0.85, 0.35, 1]
    orb["shader_params"] = [[0.3, 1.0, 0.45, 5], [3.0, 2, 0.35, 0]]
    write_json(a_dst / "materials/health_orb.vmat", orb)
    write_meta(a_dst / "materials/health_orb.vmat", uid("material/health_orb"))
    gate = copy.deepcopy(energy)
    gate["base_color"] = [0.75, 0.12, 0.08, 1]
    gate["shader_params"] = [[1.0, 0.2, 0.08, 4], [1.5, 2, 0.4, 0]]
    write_json(a_dst / "materials/gate_glow.vmat", gate)
    write_meta(a_dst / "materials/gate_glow.vmat", uid("material/gate_glow"))
    orb_material = json.loads((a_dst / "materials/health_orb.vmat.meta").read_text())["uuid"]
    gate_material = json.loads((a_dst / "materials/gate_glow.vmat.meta").read_text())["uuid"]

    scene = json.loads((src / "scenes/main.vscene").read_text())
    ents = scene["entities"]

    # ---- player (template character + slash sparks + arena script) ----------------------------
    player = subtree(ents, by_name(ents, "Player")["uuid"])
    player[0]["components"]["Script"]["script"] = "scripts/player.luau"
    player[0]["components"]["Transform"]["position"] = [0, 0.05, 3]
    player[0]["components"]["CharacterController"]["radius"] = 0.5
    # The arena scripts pick Idle/Walk themselves (entity:play_animation), so the characters run
    # their clips directly instead of the template's locomotion controller.
    plain_clips(player)
    sparks = by_name(ents, "Sparks")
    player.append(emitter(sparks, "Slash", parent=player[0]["uuid"], pos=(0, 0.9, -1.1), emitting=False, loop=False,
                          rate=0, max_particles=96, shape="sphere", radius=0.35, speed_min=1.5, speed_max=4.0,
                          lifetime_min=0.18, lifetime_max=0.35, gravity=[0, -2, 0], size_start=0.08,
                          color_start=[0.7, 0.9, 1.0, 1], color_end=[0.2, 0.5, 1.0, 0], intensity=8))

    camera = copy.deepcopy(by_name(ents, "Main Camera"))
    camera["components"]["Script"] = {"script": "scripts/third_person_camera.luau", "enabled": True, "properties": {}}
    camera["components"]["Transform"]["position"] = [0, 6, 11]

    sun = copy.deepcopy(by_name(ents, "Sun"))
    env = copy.deepcopy(by_name(ents, "Environment"))
    env["components"]["Environment"].update({
        "sky_zenith": [0.1, 0.13, 0.28, 1], "sky_horizon": [0.62, 0.42, 0.36, 1], "ambient_intensity": 0.45,
        "bloom_intensity": 0.08, "vignette": 0.35, "shadow_distance": 45, "fog": "exponential",
        "fog_color": [0.45, 0.33, 0.32, 1], "fog_density": 0.012})

    # ---- level -------------------------------------------------------------------------------
    level = []
    floor = by_name(ents, "Ground")
    floor = copy.deepcopy(floor)
    floor["name"] = "Arena Floor"
    floor["uuid"] = uid("Arena Floor")
    floor["components"]["Transform"]["scale"] = [32, 1, 32]
    floor["components"]["MeshRenderer"]["material"] = STARTER["paving"]
    floor["components"]["MeshRenderer"]["base_color"] = [0.9, 0.88, 0.86, 1]
    level.append(floor)
    stone = (0.8, 0.78, 0.82)
    for name, pos, scale in [("Wall North", (0, 1, -15.5), (32, 2, 1)), ("Wall South", (0, 1, 15.5), (32, 2, 1)),
                             ("Wall West", (-15.5, 1, 0), (1, 2, 30)), ("Wall East", (15.5, 1, 0), (1, 2, 30))]:
        level.append(solid(name, "cube", pos, scale, stone, roughness=0.9, material=STARTER["rock"]))
    for i, (x, z) in enumerate([(-5, -5), (5, -5), (5, 5), (-5, 5)]):
        level.append(solid(f"Pillar {i + 1}", "cylinder", (x, 1.5, z), (1.2, 3, 1.2), (1.0, 0.97, 0.92), 0.0, 0.8,
                           material=STARTER["rock"]))
    for i, (pos, scale) in enumerate([((0, 0.5, -8.5), (3, 1, 1)), ((0, 0.5, 8.5), (3, 1, 1)),
                                      ((-8.5, 0.5, 0), (1, 1, 3)), ((8.5, 0.5, 0), (1, 1, 3))]):
        level.append(solid(f"Cover {i + 1}", "cube", pos, scale, (1, 1, 1), roughness=0.75,
                           material=STARTER["planks"]))
    # Spawn gates in the corners: two glowing posts and a red light each.
    for i, (x, z) in enumerate([(-12, -12), (12, -12), (12, 12), (-12, 12)]):
        sx, sz = (1 if x > 0 else -1), (1 if z > 0 else -1)
        # Posts sit on the two walls' diagonal so the corner stays open for robots to walk out.
        for j, (px, pz) in enumerate([(x + sx * 2.2, z - sz * 0.6), (x - sx * 0.6, z + sz * 2.2)]):
            level.append(solid(f"Gate {i + 1} Post {j + 1}", "cube", (px, 1.25, pz), (0.5, 2.5, 0.5), (1, 1, 1),
                               roughness=0.4, material=gate_material))
        level.append(point_light(f"Gate {i + 1} Light", (x + sx, 2.2, z + sz), (1.0, 0.25, 0.12), 14, 7))
    level.append(point_light("Center Light", (0, 4, 0), (0.6, 0.75, 1.0), 10, 12, shadows=True))
    level.append(emitter(sparks, "Hit Burst", pos=(0, -10, 0), emitting=False, loop=False, rate=0, max_particles=256,
                         shape="sphere", radius=0.3, speed_min=2, speed_max=6, lifetime_min=0.3, lifetime_max=0.8,
                         gravity=[0, -6, 0], drag=1.2, size_start=0.1, color_start=[1, 0.7, 0.3, 1],
                         color_end=[1, 0.15, 0.05, 0], intensity=8))
    arena = {"uuid": uid("Arena"), "name": "Arena", "components": {
        "Script": {"script": "scripts/arena.luau", "enabled": True, "properties": {}},
        "Transform": transform()}}

    # ---- HUD ---------------------------------------------------------------------------------
    hud = subtree(ents, by_name(ents, "HUD")["uuid"])
    hud_root = hud[0]["uuid"]
    controls = by_name(hud, "Controls Text")
    controls["components"]["UiText"]["text"] = "\n".join([
        "WASD, arrows or left stick   move (Shift sprints)",
        "Left mouse, J or X button   swing",
        "Right mouse drag or right stick   turn the camera",
        "Space or A button   jump",
        "F5 / F9 or LB / RB   quick save / load",
        "R or Start   restart     H or View   this panel"])
    by_name(hud, "Controls Panel")["components"]["UiRect"]["size"] = [520, 196]
    by_name(hud, "Controls Panel")["components"]["UiRect"]["visible"] = False
    toast = by_name(hud, "Save Status")
    status = copy.deepcopy(toast)
    status["uuid"], status["name"] = uid("Status Text"), "Status Text"
    status["components"]["UiRect"].update({"anchor_min": [0, 0], "anchor_max": [0, 0], "pivot": [0, 0],
                                           "position": [24, 24], "size": [600, 52], "visible": True})
    status["components"]["UiText"].update({"text": "Wave 0 / 5", "font_size": 24})
    banner = copy.deepcopy(toast)
    banner["uuid"], banner["name"] = uid("Banner"), "Banner"
    banner["components"]["UiRect"].update({"anchor_min": [0.5, 0.5], "anchor_max": [0.5, 0.5], "pivot": [0.5, 0.5],
                                           "position": [0, -150], "size": [620, 84], "visible": True})
    banner["components"]["UiText"].update({"text": "Robot Arena", "font_size": 40})
    hud += [status, banner]

    main_scene = {"format": scene["format"], "version": scene["version"], "name": "Robot Arena",
                  "entities": [camera, sun, env, arena] + level + player + hud}
    write_json(dst / "scenes/main.vscene", main_scene)
    auto = copy.deepcopy(main_scene)
    auto["name"] = "Robot Arena (autoplay)"
    by_name(auto["entities"], "Player")["components"]["Script"]["properties"] = {"autopilot": True}
    write_json(dst / "scenes/autoplay.vscene", auto)

    # ---- prefabs -----------------------------------------------------------------------------
    enemy = remap(subtree(ents, by_name(ents, "Buddy")["uuid"]), "enemy_robot")
    root = enemy[0]
    root["name"] = "Enemy Robot"
    del root["components"]["Script"]
    root["components"]["Transform"]["position"] = [0, 0, 0]
    cc = root["components"]["CharacterController"]
    cc.update({"radius": 0.45, "height": 1.1, "layer": 2})
    root["components"]["NavAgent"].update({"speed": 3.1, "radius": 0.45, "height": 1.1, "stopping_distance": 1.0})
    # The template's companion (Nova), 1.28 m tall to fit the 1.1 m controller, tinted red.
    by_name(enemy, "Robot")["components"]["Transform"]["scale"] = [0.75, 0.75, 0.75]
    plain_clips(enemy)
    for e in enemy:
        if "_Body[" in e["name"]:
            # Tint the character's own colours red; its PBR inputs stay as authored.
            mr = e["components"]["MeshRenderer"]
            mr["base_color"] = [c * t for c, t in zip(mr["base_color"], (0.85, 0.16, 0.12, 1))]
    write_json(a_dst / "prefabs/enemy_robot.vprefab",
               {"format": "victus.prefab", "version": 1, "name": "Enemy Robot", "entities": enemy})
    write_meta(a_dst / "prefabs/enemy_robot.vprefab", uid("prefab/enemy_robot"))

    gem = copy.deepcopy(by_name(ents, "Floating Gem"))
    gem["uuid"], gem["name"] = uid("prefab/health_orb/root"), "Health Orb"
    gem["components"]["MeshRenderer"] = mesh("sphere", (1, 1, 1), 0, 0.3, orb_material)
    gem["components"]["Transform"] = transform(scale=(0.55, 0.55, 0.55))
    gem["components"]["Oscillator"]["amplitude"] = [0, 0.25, 0]
    gem["components"]["Oscillator"]["frequency"] = 0.8
    write_json(a_dst / "prefabs/health_orb.vprefab",
               {"format": "victus.prefab", "version": 1, "name": "Health Orb", "entities": [gem]})
    write_meta(a_dst / "prefabs/health_orb.vprefab", uid("prefab/health_orb"))

    # ---- project -----------------------------------------------------------------------------
    project = json.loads((src / "victus.project").read_text())
    project["name"] = "Robot Arena"
    project.pop("template", None)
    actions = [a for a in project["input"]["actions"] if a["name"] not in ("fire", "interact", "pause")]
    actions += [{"name": "attack", "bindings": ["Mouse0", "J", "Pad.West", "Pad.RightTrigger"]},
                {"name": "restart", "bindings": ["R", "Pad.Start"]}]
    project["input"]["actions"] = actions
    write_json(dst / "victus.project", project)


if __name__ == "__main__":
    main()
