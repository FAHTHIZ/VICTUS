#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Writes the perceptual reference scenes (ADR-0104) into samples/reference_scenes.

    python3 samples/reference_scenes/tools/generate_reference.py samples/reference_scenes

Two deterministic scenes for `victus run --capture-dir` + `victus compare` (ADR-0102) and the
rendering-path report (ADR-0103). Each one has a fixed camera, no scripts, no physics and no
particles, so a fixed-frame run renders the same pixels every time; one slowly rotating object
gives the sequence a temporal signal (flicker, ghosting, TAA trails).

- exterior.vscene: shadowed low sun, physical sky, height + volumetric fog, a gloss/metal
  sphere row (roughness ramp), thin posts and a fence (aliasing, shadow detail), stairs (contact
  shadows, AO), an emissive bar (bloom), a rotating cube.
- interior.vscene: closed room lit only by local lights: a shadowed point lamp, a shadowed spot,
  an emissive panel, a fog volume (light shafts), glossy floor, dark corners (AO, noise), a
  rotating metal cylinder.

UUIDs are derived from names (uuid5), so regenerating produces identical files.
"""
import json
import math
import sys
import uuid
from pathlib import Path

NS = uuid.UUID("7d1e4a90-2b6c-4f3e-8a57-1c9e0b2d6f44")


def uid(name):
    return str(uuid.uuid5(NS, name))


def quat_euler(pitch=0.0, yaw=0.0):
    """Rotation: yaw around +Y, then pitch around the local +X (degrees). Returns x, y, z, w."""
    p, y = math.radians(pitch) / 2, math.radians(yaw) / 2
    cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p), math.sin(p)
    return [round(cy * sp, 7), round(sy * cp, 7), round(-sy * sp, 7), round(cy * cp, 7)]


def transform(pos=(0, 0, 0), scale=(1, 1, 1), rot=(0, 0, 0, 1)):
    return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}


def entity(name, **components):
    return {"uuid": uid(name), "name": name, "components": components}


def mesh(primitive, color=(1, 1, 1), metallic=0.0, roughness=0.8, material=None, shadows=True):
    m = {"primitive": primitive, "base_color": list(color) + [1], "metallic": metallic, "roughness": roughness,
         "cast_shadows": shadows, "visible": True}
    if material:
        m["material"] = material
    return m


def solid(name, primitive, pos, scale=(1, 1, 1), rot=(0, 0, 0, 1), **kw):
    return entity(name, MeshRenderer=mesh(primitive, **kw), Transform=transform(pos, scale, rot))


def light(name, kind, pos, color, intensity, rng=10.0, shadows=True, rot=(0, 0, 0, 1), inner=20.0, outer=32.0):
    l = {"type": kind, "color": list(color) + [1], "intensity": intensity, "range": rng, "shadows": shadows}
    if kind == "spot":
        l["inner_angle"] = inner
        l["outer_angle"] = outer
    return entity(name, Light=l, Transform=transform(pos, rot=rot))


def camera(pos, pitch, yaw, fov=55):
    return entity("Reference Camera", Camera={"projection": "perspective", "fov_y": fov, "ortho_height": 10, "near": 0.1,
                                               "far": 300, "primary": True},
                  Transform=transform(pos, rot=quat_euler(pitch, yaw)))


def environment(**overrides):
    env = {"sky_zenith": [0.18, 0.36, 0.75, 1], "sky_horizon": [0.62, 0.72, 0.85, 1], "ground": [0.22, 0.2, 0.18, 1],
           "ambient_intensity": 0.6, "draw_sky": True, "clear_color": [0.02, 0.02, 0.025, 1], "tonemapper": "aces",
           "exposure": 1, "exposure_compensation": 0, "auto_exposure": False, "saturation": 1, "contrast": 1,
           "vignette": 0.0, "bloom_intensity": 0.06, "bloom_threshold": 1, "ao_intensity": 1, "ao_radius": 0.6,
           "shadow_distance": 40, "fog": "none"}
    env.update(overrides)
    return entity("Environment", Environment=env)


def material(name, color, roughness, metallic=0.0, emissive=(0, 0, 0), strength=1.0):
    return {"victus_material": 1, "base_color": list(color) + [1], "base_color_texture": None, "metallic": metallic,
            "roughness": roughness, "metallic_roughness_texture": None, "normal_texture": None, "normal_scale": 1,
            "emissive": list(emissive), "emissive_strength": strength, "emissive_texture": None, "alpha_cutoff": 0,
            "uv_scale": [1, 1], "uv_offset": [0, 0], "filter": "linear", "unlit": False, "double_sided": False,
            "vertex_alpha": "opacity", "coverage_response": 1, "blend": "opaque"}


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


def scene(name, entities):
    return {"format": "victus.scene", "version": 1, "name": name, "entities": entities}


def exterior(mats):
    e = [camera((0, 2.4, 9.5), -9, 0),
         light("Sun", "directional", (0, 10, 0), (1, 0.93, 0.82), 3.2, rot=quat_euler(-28, 35)),
         environment(physical_sky=True, fog="exponential", fog_density=0.004, fog_color=[0.62, 0.72, 0.85, 1],
                     volumetric_fog=True, volumetric_density=0.015, volumetric_height=0, volumetric_falloff=0.25,
                     volumetric_anisotropy=0.6, volumetric_distance=60),
         solid("Ground", "plane", (0, 0, 0), (40, 1, 40), color=(0.55, 0.53, 0.5), roughness=0.9)]
    # Roughness ramp: dielectric spheres in front, metal spheres behind.
    for i in range(6):
        r = round(0.05 + i * 0.18, 2)
        e.append(solid(f"Sphere Dielectric {i}", "sphere", (-3.75 + i * 1.5, 0.55, 2.0), (1, 1, 1),
                       color=(0.8, 0.2, 0.15), roughness=r))
        e.append(solid(f"Sphere Metal {i}", "sphere", (-3.75 + i * 1.5, 0.55, 0.2), (1, 1, 1),
                       color=(0.95, 0.75, 0.4), metallic=1.0, roughness=r))
    # Thin geometry: posts and fence rails (aliasing, shadow-map resolution).
    for i in range(13):
        e.append(solid(f"Post {i}", "cube", (-6 + i, 0.6, -2.5), (0.05, 1.2, 0.05), color=(0.3, 0.25, 0.2)))
    for j, h in enumerate((0.45, 0.95)):
        e.append(solid(f"Rail {j}", "cube", (0, h, -2.5), (12.2, 0.04, 0.04), color=(0.3, 0.25, 0.2)))
    # Stairs and a wall (AO, contact shadows), an emissive bar (bloom), a rotating cube (temporal).
    for s in range(5):
        e.append(solid(f"Step {s}", "cube", (5.5, 0.15 + s * 0.3, -0.5 - s * 0.4), (2, 0.3 + s * 0.6, 0.4),
                       color=(0.75, 0.75, 0.72)))
    e.append(solid("Wall", "cube", (-5.5, 1.25, -1.0), (0.4, 2.5, 4), color=(0.7, 0.68, 0.62)))
    e.append(solid("Emissive Bar", "cube", (-5.25, 2.0, -1.0), (0.1, 0.12, 3), material=mats["glow"], shadows=False))
    rot = solid("Rotating Cube", "cube", (4.2, 0.6, 3.6), (0.6, 0.6, 0.6), color=(0.2, 0.45, 0.85), roughness=0.35)
    rot["components"]["Rotator"] = {"axis": [0.3, 1, 0.2], "degrees_per_second": 30}
    e.append(rot)
    return scene("Reference Exterior", e)


def interior(mats):
    e = [camera((0, 1.6, 4.2), -6, 0, fov=65),
         environment(draw_sky=False, ambient_intensity=0.05, clear_color=[0, 0, 0, 1], bloom_intensity=0.08,
                     ao_radius=0.5, shadow_distance=20),
         solid("Floor", "cube", (0, -0.05, 0), (8, 0.1, 10), color=(0.35, 0.32, 0.3), roughness=0.25),
         solid("Ceiling", "cube", (0, 3.05, 0), (8, 0.1, 10), color=(0.8, 0.8, 0.78)),
         solid("Back Wall", "cube", (0, 1.5, -5), (8, 3, 0.1), color=(0.75, 0.72, 0.65)),
         solid("Left Wall", "cube", (-4, 1.5, 0), (0.1, 3, 10), color=(0.6, 0.18, 0.15)),
         solid("Right Wall", "cube", (4, 1.5, 0), (0.1, 3, 10), color=(0.15, 0.35, 0.6)),
         solid("Front Wall", "cube", (0, 1.5, 5), (8, 3, 0.1), color=(0.75, 0.72, 0.65)),
         solid("Table", "cube", (-1.2, 0.4, -1.5), (1.6, 0.8, 0.9), color=(0.45, 0.3, 0.18), roughness=0.6),
         solid("Box", "cube", (1.6, 0.35, -2.8), (0.7, 0.7, 0.7), color=(0.8, 0.78, 0.7)),
         solid("Ball", "sphere", (-1.2, 1.1, -1.5), (0.6, 0.6, 0.6), color=(0.9, 0.9, 0.9), metallic=1.0, roughness=0.15),
         solid("Emissive Panel", "cube", (0, 2.2, -4.93), (1.6, 0.4, 0.04), material=mats["panel"], shadows=False),
         light("Lamp", "point", (-1.0, 2.3, -0.5), (1, 0.7, 0.45), 14, rng=7),
         light("Spot", "spot", (2.4, 2.8, 0.5), (0.8, 0.9, 1), 40, rng=9, rot=quat_euler(-36, 14), inner=14, outer=24),
         entity("Dust", FogVolume={"size": [7.8, 2.9, 9.8], "density": 0.08, "albedo": [1, 1, 1, 1], "edge_fade": 0.05,
                                   "visible": True}, Transform=transform((0, 1.5, 0)))]
    env = e[1]["components"]["Environment"]
    env.update(volumetric_fog=True, volumetric_density=0.0, volumetric_distance=20, volumetric_anisotropy=0.5)
    rot = solid("Rotating Cylinder", "cylinder", (1.6, 1.15, -2.8), (0.25, 0.9, 0.25), color=(0.9, 0.9, 0.92), metallic=1.0,
                roughness=0.3)
    rot["components"]["Rotator"] = {"axis": [1, 0, 0], "degrees_per_second": 25}
    e.append(rot)
    return scene("Reference Interior", e)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    mats = {"glow": uid("material/glow"), "panel": uid("material/panel")}
    write_json(out / "assets/materials/glow.vmat", material("glow", (1, 0.6, 0.25), 0.5, emissive=(1, 0.55, 0.2), strength=6))
    write_json(out / "assets/materials/panel.vmat", material("panel", (0.9, 0.95, 1), 0.5, emissive=(0.85, 0.92, 1), strength=4))
    for key, file in (("glow", "glow.vmat"), ("panel", "panel.vmat")):
        write_json(out / f"assets/materials/{file}.meta", {"victus_meta": 1, "uuid": mats[key], "importer": ""})
    write_json(out / "scenes/exterior.vscene", exterior(mats))
    write_json(out / "scenes/interior.vscene", interior(mats))
    write_json(out / "victus.project", {"format": "victus.project", "version": 1, "name": "Reference Scenes",
                                         "startup_scene": "scenes/exterior.vscene", "engine_version": "0.2.0",
                                         "window": {"width": 1280, "height": 720, "vsync": True},
                                         "rendering": {"quality": "ultra"}, "input": {"actions": []}})


if __name__ == "__main__":
    main()
