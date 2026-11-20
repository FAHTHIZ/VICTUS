# SPDX-License-Identifier: Apache-2.0
"""Anatomy and armour layout for the VICTUS starter characters, Orion and Nova (docs/CHARACTERS.md).

Pure NumPy (no Blender), built on tools/characters/sdf.py. Coordinates are Blender armature space:
X = character left, -Y = forward, Z = up, metres, feet on Z = 0, 1.70 m to the crown.

Each character has
  * a body field: a smooth anatomical figure (the dark flexible under-layer), every primitive tagged
    with the bone it moves with so skin weights fall out of the modelling;
  * plates: rigid shell pieces, each the body offset outward and clipped to a region, with a
    rounded edge, bound to one bone. Gaps between plates are the visible seams.

Proportions follow standard adult canons (head ~1/7.5 of height, shoulder line ~0.82 H, hip joint
~0.53 H); the joint centres are the shared 21-joint rig's and never change between variants.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from sdf import Field, Prim, ellipsoid, frame, rbox, round_cone, rot, smax, smin, sphere

# ----------------------------------------------------------------------------------------------
# Shared rig (identical to the earlier Vanguard robots' rig so clips, controller and scripts carry over).

ARM_REST_OUT = 40.0
L_UPPER_ARM, L_FOREARM, L_HAND = 0.28, 0.255, 0.17
SHOULDER = np.array([0.20, 0.0, 1.38])
HIP = np.array([0.10, 0.0, 0.90])
KNEE_Z, ANKLE_Z = 0.48, 0.09


def arm_dir(side):
    a = math.radians(ARM_REST_OUT)
    return np.array([side * math.sin(a), 0.0, -math.cos(a)])


def arm_points(side):
    d = arm_dir(side)
    s = np.array([side * SHOULDER[0], 0.0, SHOULDER[2]])
    e = s + d * L_UPPER_ARM
    w = e + d * L_FOREARM
    return s, e, w, w + d * L_HAND


def leg_points(side):
    x = side * HIP[0]
    return (np.array([x, 0.0, HIP[2]]), np.array([x, 0.0, KNEE_Z]), np.array([x, 0.0, ANKLE_Z]),
            np.array([x, -0.12, 0.03]), np.array([x, -0.20, 0.03]))


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
        s, e, w, h = arm_points(side)
        hip, knee, ankle, ball, toe = leg_points(side)
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


def lerp(a, b, t):
    return a + (b - a) * t


# ----------------------------------------------------------------------------------------------
# Variants


@dataclass
class Variant:
    name: str
    masc: bool
    # torso
    rib_r: tuple  # rib-cage semi-axes
    chest_z: float
    pec: tuple
    bust: float  # 0 = none
    waist_r: tuple
    pelvis_r: tuple
    glute_r: float
    trap: float
    lat: tuple
    delt: float
    # limbs (radii)
    upper_arm: tuple
    forearm: tuple
    thigh: tuple
    calf: tuple
    neck: tuple
    hand_scale: float
    # head
    cranium: tuple
    jaw: tuple
    chin: float


ORION = Variant(
    name="Orion", masc=True,
    rib_r=(0.150, 0.105, 0.165), chest_z=1.295, pec=(0.072, 0.032, 0.052), bust=0.0,
    waist_r=(0.122, 0.090, 0.11), pelvis_r=(0.150, 0.100, 0.095), glute_r=0.070,
    trap=0.034, lat=(0.050, 0.055, 0.115), delt=0.058,
    upper_arm=(0.047, 0.037), forearm=(0.041, 0.025), thigh=(0.083, 0.050), calf=(0.050, 0.030),
    neck=(0.056, 0.050), hand_scale=1.0,
    cranium=(0.075, 0.100, 0.098), jaw=(0.054, 0.068, 0.066), chin=0.024,
)

NOVA = Variant(
    name="Nova", masc=False,
    rib_r=(0.128, 0.095, 0.150), chest_z=1.300, pec=(0.060, 0.026, 0.045), bust=0.052,
    waist_r=(0.098, 0.078, 0.10), pelvis_r=(0.158, 0.102, 0.098), glute_r=0.074,
    trap=0.026, lat=(0.038, 0.045, 0.10), delt=0.046,
    upper_arm=(0.039, 0.031), forearm=(0.034, 0.021), thigh=(0.082, 0.046), calf=(0.046, 0.027),
    neck=(0.046, 0.042), hand_scale=0.92,
    cranium=(0.071, 0.096, 0.096), jaw=(0.049, 0.064, 0.062), chin=0.020,
)

VARIANTS = {"orion": ORION, "nova": NOVA}

# ----------------------------------------------------------------------------------------------
# Body field


def _head(f: Field, v: Variant):
    # Profile (y, forward is -y): forehead -0.094, brow -0.102, nose tip -0.122, mouth -0.094,
    # chin -0.098; crown 1.70, brow 1.605, nose base 1.540, mouth 1.512, chin 1.465 (facial
    # thirds). Width ~0.15 m, glabella-to-back ~0.20 m. Stylised mannequin face: no lips or lids.
    m = 1.0 if v.masc else 0.0
    cr = v.cranium
    f.add(ellipsoid((0, 0.006, 1.70 - cr[2]), cr, "Head", k=0.0))
    f.add(ellipsoid((0, -0.022, 1.536), v.jaw, "Head", k=0.03))
    for s in (1, -1):
        f.add(round_cone((s * (0.054 - 0.004 * (1 - m)), 0.016, 1.530), (s * 0.017, -0.072, 1.484),
                         0.011 + 0.002 * m, 0.010, "Head", k=0.018))  # jaw line
        f.add(ellipsoid((s * 0.045, -0.072, 1.566), (0.016, 0.014, 0.010), "Head", k=0.016))  # cheekbone
        f.add(ellipsoid((s * 0.032, -0.101, 1.585), (0.016, 0.008, 0.0095), "Head", k=0.008, sub=True))  # socket
        f.add(ellipsoid((s * 0.073, 0.006, 1.572), (0.009, 0.019, 0.029), "Head", k=0.008))  # ear
    f.add(sphere((0, -0.077, 1.491), v.chin, "Head", k=0.02))
    f.add(ellipsoid((0, -0.090, 1.606), (0.054, 0.012, 0.009 + 0.002 * m), "Head", k=0.016))  # brow
    f.add(round_cone((0, -0.097, 1.594), (0, -0.111, 1.548), 0.006, 0.0102, "Head", k=0.012))  # nose
    for s in (1, -1):
        f.add(sphere((s * 0.0105, -0.104, 1.544), 0.0068, "Head", k=0.006))  # nostril wing
    f.add(ellipsoid((0, -0.0965, 1.512), (0.016, 0.006, 0.0011), "Head", k=0.002, sub=True))  # mouth line


def _torso(f: Field, v: Variant):
    f.add(round_cone((0, 0.012, 1.430), (0, 0.004, 1.545), v.neck[0], v.neck[1], "Neck", k=0.02,
                     scale=(1.0, 0.95)))
    f.add(ellipsoid((0, 0.004, v.chest_z), v.rib_r, "Chest", k=0.04))
    f.add(ellipsoid((0, -0.026, 1.135), v.waist_r, "Spine", k=0.07))
    f.add(ellipsoid((0, 0.004, 0.955), v.pelvis_r, "Hips", k=0.06))
    for s in (1, -1):
        f.add(ellipsoid((s * 0.066, -0.070, 1.322), v.pec, "Chest", k=0.03, R=rot((0, 1, 0), s * 12)))
        if v.bust:
            f.add(sphere((s * 0.058, -0.088, 1.290), v.bust, "Chest", k=0.035))
        # Trapezius from neck root to the shoulder.
        f.add(round_cone((s * 0.03, 0.022, 1.468), (s * 0.165, 0.012, 1.415), v.trap, v.trap * 0.8, "Chest", k=0.03))
        f.add(ellipsoid((s * 0.105, 0.040, 1.255), v.lat, "Chest", k=0.04, R=rot((0, 1, 0), s * -14)))
        # Deltoid over the shoulder joint.
        sh = np.array([s * (SHOULDER[0] + 0.005), 0.0, SHOULDER[2] + 0.004])
        f.add(ellipsoid(sh, (v.delt, v.delt * 0.95, v.delt * 1.05), f"Shoulder.{'L' if s > 0 else 'R'}", k=0.035,
                        R=rot((0, 1, 0), s * 30)))
        f.add(sphere((s * 0.068, 0.060, 0.905), v.glute_r, "Hips", k=0.05))
        # Obliques and the iliac crest give the waist its V.
        f.add(ellipsoid((s * 0.090, -0.012, 1.045), (0.050, 0.070, 0.070), "Hips", k=0.05))
    # Six-pack hint: shallow vertical groove (linea alba) and spine furrow.
    f.add(ellipsoid((0, -0.118 + (0.012 if not v.masc else 0), 1.175), (0.004, 0.01, 0.075), "Spine", k=0.012, sub=True))
    f.add(ellipsoid((0, 0.108, 1.22), (0.006, 0.012, 0.13), "Chest", k=0.016, sub=True))


def _arm(f: Field, v: Variant, side):
    t = "L" if side > 0 else "R"
    s, e, w, h = arm_points(side)
    d = arm_dir(side)
    fw = np.array([0.0, -1.0, 0.0])  # thumb side
    inward = np.cross(fw, d) * side  # palm normal pointing at the thigh
    ua, la, hand = f"UpperArm.{t}", f"LowerArm.{t}", f"Hand.{t}"
    f.add(round_cone(s + d * 0.03, e, v.upper_arm[0], v.upper_arm[1], ua, k=0.03, scale=(1.0, 1.05)))
    # Biceps (front) and triceps (back) bellies.
    f.add(ellipsoid(lerp(s, e, 0.52) + fw * 0.017, (v.upper_arm[0] * 0.7, v.upper_arm[0] * 0.62, 0.075), ua,
                    k=0.025, R=frame(d).T))
    f.add(ellipsoid(lerp(s, e, 0.40) - fw * 0.016, (v.upper_arm[0] * 0.72, v.upper_arm[0] * 0.6, 0.085), ua,
                    k=0.025, R=frame(d).T))
    # Forearm: full near the elbow, tapering to an oval wrist.
    f.add(round_cone(e, lerp(e, w, 0.35), v.forearm[0] * 0.95, v.forearm[0], la, k=0.03, scale=(1.05, 0.95)))
    f.add(round_cone(lerp(e, w, 0.35), w, v.forearm[0], v.forearm[1], la, k=0.03, scale=(1.15, 0.85)))
    f.add(sphere(e - fw * 0.012, v.forearm[1] * 0.9, la, k=0.03))  # elbow point
    _hand(f, v, w, d, fw, inward, hand)


def _hand(f: Field, v: Variant, w, d, fw, inward, bone):
    k = v.hand_scale
    R = np.stack([inward, fw, d]).T  # local x = palm normal, y = thumb, z = along the hand
    palm_c = w + d * 0.050 * k
    f.add(rbox(palm_c, (0.0135 * k, 0.040 * k, 0.046 * k), 0.012 * k, bone, k=0.02, R=R))
    # Four fingers in a relaxed curl, index at the front.
    for i, (off, length) in enumerate(((0.029, 0.072), (0.010, 0.080), (-0.009, 0.075), (-0.027, 0.060))):
        base = palm_c + d * 0.042 * k + fw * off * k
        axis = np.cross(d, inward)
        r = (0.0090 - 0.0005 * i) * k
        p, bend = base, 0.0
        for seg, frac in enumerate((0.42, 0.33, 0.25)):
            # Relaxed curl toward the palm, a little more for the outer fingers.
            bend += (10 + 5 * i) if seg == 0 else (18 + 4 * i)
            q = p + (rot(axis, bend) @ d) * length * frac * k
            f.add(round_cone(p, q, r, r * 0.88, bone, k=0.004))
            p, r = q, r * 0.88
    # Thumb from the base of the palm, angled forward and in.
    tb = w + d * 0.022 * k + fw * 0.030 * k + inward * 0.006 * k
    t1 = tb + (d * 0.55 + fw * 0.65 + inward * 0.35) * 0.040 * k
    t2 = t1 + (d * 0.85 + fw * 0.2 + inward * 0.45) * 0.032 * k
    f.add(round_cone(tb, t1, 0.0125 * k, 0.0098 * k, bone, k=0.012))
    f.add(round_cone(t1, t2, 0.0098 * k, 0.0085 * k, bone, k=0.004))


def _leg(f: Field, v: Variant, side):
    t = "L" if side > 0 else "R"
    hip, knee, ankle, ball, toe = leg_points(side)
    ul, ll, ft, tt = f"UpperLeg.{t}", f"LowerLeg.{t}", f"Foot.{t}", f"Toe.{t}"
    x = side * HIP[0]
    top = np.array([x * 1.05, 0.004, 0.86])
    f.add(round_cone(top, knee + np.array([0, -0.004, 0]), v.thigh[0], v.thigh[1], ul, k=0.05, scale=(0.95, 1.0)))
    # Quadriceps (front, outer sweep) and hamstrings.
    f.add(ellipsoid((x * 1.08, -0.030, 0.69), (v.thigh[0] * 0.68, v.thigh[0] * 0.62, 0.15), ul, k=0.04,
                    R=rot((0, 1, 0), side * 3)))
    f.add(ellipsoid((x * 0.80, 0.008, 0.76), (v.thigh[0] * 0.52, v.thigh[0] * 0.55, 0.11), ul, k=0.04))
    f.add(ellipsoid((x, 0.032, 0.68), (v.thigh[0] * 0.62, v.thigh[0] * 0.55, 0.14), ul, k=0.04))
    # Knee cap and the shin.
    f.add(ellipsoid((x, -0.040, KNEE_Z + 0.005), (0.030, 0.018, 0.034), ll, k=0.02))
    f.add(round_cone(knee, ankle + np.array([0, 0.004, 0.02]), v.calf[0] * 0.95, v.calf[1], ll, k=0.03, scale=(0.95, 1.0)))
    # Calf (gastrocnemius), higher on the inside.
    f.add(ellipsoid((x * 0.96, 0.024, 0.355), (v.calf[0] * 0.80, v.calf[0] * 0.75, 0.095), ll, k=0.035))
    # Ankle bones.
    for s2 in (1, -1):
        f.add(sphere((x + s2 * 0.026, 0.004, ANKLE_Z + 0.005), 0.015, ll, k=0.02))
    # Foot: heel, arch and the ball; flat sole from Field.floor.
    f.add(sphere((x, 0.030, 0.045), 0.042, ft, k=0.03))
    f.add(round_cone((x, 0.012, 0.060), ball + np.array([0, 0, 0.010]), 0.040, 0.034, ft, k=0.03, scale=(1.1, 1.0)))
    f.add(rbox(ball + np.array([side * 0.003, -0.016, 0.0]), (0.044, 0.050, 0.026), 0.024, ft, k=0.02))
    f.add(rbox(toe + np.array([side * 0.004, 0.024, -0.006]), (0.040, 0.030, 0.020), 0.018, tt, k=0.012))


def body_field(v: Variant) -> Field:
    f = Field(floor=0.0, symmetric=True)
    _head(f, v)
    _torso(f, v)
    for side in (1, -1):
        _arm(f, v, side)
        _leg(f, v, side)
    return f


# ----------------------------------------------------------------------------------------------
# Plates: region functions are negative inside the region. A plate is
#   smax(body - offset, region, edge)
# so it follows the body exactly, stands proud by `offset` and has a rounded edge.


def plane(n, p0):
    n = np.asarray(n, float)
    n = n / np.linalg.norm(n)
    p0 = np.asarray(p0, float)
    return lambda P: (P - p0) @ n


def slab(a, b, t0, t1):
    """Region between fractions t0..t1 along a -> b (perpendicular cut planes)."""
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    ax = b - a
    L = float(np.linalg.norm(ax))
    ax = ax / L

    def fn(P):
        t = (P - a) @ ax
        return np.maximum(t0 * L - t, t - t1 * L)

    return fn


def near_segment(a, b, r):
    """Region within r of segment a-b (keeps a limb plate off the torso and the other limb)."""
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    ab = b - a
    l2 = float(ab @ ab)

    def fn(P):
        t = np.clip((P - a) @ ab / l2, 0.0, 1.0)
        return np.linalg.norm(P - (a + np.outer(t, ab)), axis=1) - r

    return fn


def ellipsoid_region(c, r):
    e = ellipsoid(c, r, "")
    return e.fn


def ball(c, r):
    c = np.asarray(c, float)
    return lambda P: np.linalg.norm(P - c, axis=1) - r


def intersect(*fs, k=0.0):
    def fn(P):
        d = fs[0](P)
        for g in fs[1:]:
            d = smax(d, g(P), k)
        return d

    return fn


def unite(*fs, k=0.0):
    def fn(P):
        d = fs[0](P)
        for g in fs[1:]:
            d = smin(d, g(P), k)
        return d

    return fn


def minus(f1, f2, k=0.0):
    return lambda P: smax(f1(P), -f2(P), k)


@dataclass
class Plate:
    name: str
    bone: str
    region: object
    offset: float = 0.0045
    edge: float = 0.004
    material: int = 0  # 0 shell, 3 glow trim
    centre: np.ndarray | None = None
    radius: float = 0.3


def hexagon_xz(c, r):
    """Hexagonal prism along Y (flat top/bottom), centred at c."""
    c = np.asarray(c, float)
    kx, kz = math.sqrt(3) / 2, 0.5

    def fn(P):
        x = np.abs(P[:, 0] - c[0])
        z = np.abs(P[:, 2] - c[2])
        return np.maximum(x * kx + z * kz, z) - r

    return fn


def chevron(c, half_w, height, thick):
    """A "V" in the XZ plane (VICTUS mark), centred at c, as a prism along Y."""
    c = np.asarray(c, float)
    a = np.array([half_w, height / 2])
    b = np.array([0.0, -height / 2])

    def fn(P):
        q = np.stack([np.abs(P[:, 0] - c[0]), P[:, 2] - c[2]], 1)
        ba = b - a
        t = np.clip((q - a) @ ba / (ba @ ba), 0, 1)
        return np.linalg.norm(q - (a + np.outer(t, ba)), axis=1) - thick

    return fn


def front(y=0.0):
    return lambda P: P[:, 1] - y


def plates(v: Variant):
    out = []
    m = v.masc
    jaw_cut = unite(intersect(plane((0, 0.7, -1.0), (0, -0.08, 1.466)), front(0.03)),
                    intersect(plane((0, 0, -1), (0, 0, 1.512)), lambda P: -P[:, 1]))
    head = intersect(jaw_cut, ball((0, -0.010, 1.575), 0.145))
    out.append(Plate("Head", "Head", head, offset=0.0025, edge=0.003, centre=np.array([0, -0.01, 1.59]), radius=0.14))
    for s in (1, -1):
        out.append(Plate(f"Eye.{'L' if s > 0 else 'R'}", "Head", ellipsoid_region((s * 0.032, -0.094, 1.585), (0.0145, 0.02, 0.0085)),
                         offset=0.0036, edge=0.0015, material=2, centre=np.array([s * 0.032, -0.09, 1.584]), radius=0.03))
    # Sculpted hair cap: short crop (Orion) or a swept cap with a tail (Nova); dark, standing proud.
    hairline = plane((0, -0.5 if m else -0.62, -1.0), (0, -0.08, 1.648 if m else 1.640))
    cap = intersect(hairline, ball((0, 0.012, 1.605), 0.13))
    out.append(Plate("Hair", "Head", cap, offset=0.0065 if m else 0.0085, edge=0.006, material=1,
                     centre=np.array([0, 0.01, 1.62]), radius=0.14))
    # Chest cuirass: front and back, V-cut at the bottom, open over the flanks and the shoulders.
    bottom = 1.19 if m else 1.20
    chest = intersect(lambda P: (bottom + 0.30 * np.abs(P[:, 0])) - P[:, 2], plane((0, 0.25, 1), (0, 0, 1.455)),
                      lambda P: np.abs(P[:, 0]) - (0.128 if m else 0.112))
    for s in (1, -1):
        chest = minus(chest, ball((s * 0.20, 0.0, 1.39), 0.088 if m else 0.076), k=0.004)
    chest = minus(chest, intersect(hexagon_xz((0, 0, 1.372), 0.024), front(-0.05)), k=0.002)
    out.append(Plate("Chest", "Chest", chest, centre=np.array([0, 0, 1.32]), radius=0.25))
    out.append(Plate("Emblem", "Chest", intersect(chevron((0, 0, 1.372), 0.0125, 0.026, 0.0028), front(-0.05)),
                     offset=0.0030, edge=0.001, material=3, centre=np.array([0, -0.1, 1.372]), radius=0.06))
    # Segmented abdominal plates (three rows) over the dark flexible core.
    rows = ((1.122, 1.160, "Spine"), (1.078, 1.114, "Spine"), (1.034, 1.070, "Hips"))
    for i, (z0, z1, bone) in enumerate(rows):
        w = (0.072 if m else 0.060) - 0.004 * i
        out.append(Plate(f"Abs{i}", bone, intersect(plane((0, 0, -1), (0, 0, z0)), plane((0, 0, 1), (0, 0, z1)),
                                                    lambda P, w=w: np.abs(P[:, 0]) - w, front(-0.04)),
                         offset=0.004, edge=0.004, centre=np.array([0, -0.08, (z0 + z1) / 2]), radius=0.14))
    pelvis = intersect(plane((0, 0, 1), (0, 0, 1.012)), lambda P: (0.858 + 0.62 * np.abs(P[:, 0])) - P[:, 2])
    out.append(Plate("Pelvis", "Hips", pelvis, centre=np.array([0, 0, 0.95]), radius=0.22))
    for s in (1, -1):
        t = "L" if s > 0 else "R"
        S, E, W, H = arm_points(s)
        hip, knee, ankle, ballp, toe = leg_points(s)
        side = lambda P, s=s: -s * P[:, 0]
        out.append(Plate(f"Shoulder.{t}", f"Shoulder.{t}",
                         intersect(ball((s * 0.205, 0.0, 1.392), 0.076 if m else 0.064),
                                   plane((-s * 0.55, 0, -1.0), (s * 0.2, 0, 1.355))),
                         offset=0.005, centre=np.array([s * 0.2, 0, 1.40]), radius=0.1))
        out.append(Plate(f"UpperArm.{t}", f"UpperArm.{t}", intersect(slab(S, E, 0.34, 0.82), near_segment(S, E, 0.09)),
                         centre=lerp(S, E, 0.57), radius=0.12))
        out.append(Plate(f"LowerArm.{t}", f"LowerArm.{t}", intersect(slab(E, W, 0.16, 0.84), near_segment(E, W, 0.08)),
                         centre=lerp(E, W, 0.5), radius=0.12))
        out.append(Plate(f"Hand.{t}", f"Hand.{t}", intersect(slab(W, H + arm_dir(s) * 0.03, 0.08, 1.2), near_segment(W, H, 0.09)),
                         offset=0.0012, edge=0.002, centre=lerp(W, H, 0.5), radius=0.12))
        thigh = intersect(slab(hip, knee, 0.20, 0.85), side)
        out.append(Plate(f"UpperLeg.{t}", f"UpperLeg.{t}", thigh, centre=lerp(hip, knee, 0.5), radius=0.24))
        out.append(Plate(f"Knee.{t}", f"LowerLeg.{t}",
                         intersect(ball((s * HIP[0], -0.035, KNEE_Z + 0.004), 0.036), front(-0.02)),
                         offset=0.0065, centre=np.array([s * HIP[0], -0.04, KNEE_Z]), radius=0.06))
        out.append(Plate(f"LowerLeg.{t}", f"LowerLeg.{t}", intersect(slab(knee, ankle, 0.16, 0.86), side, front(0.006)),
                         centre=lerp(knee, ankle, 0.5), radius=0.24))
        foot = intersect(plane((0, 0, 1), (0, 0, 0.112)), plane((0, 1, 0), (0, -0.112, 0)),
                         plane((0, 0, -1), (0, 0, 0.012)), side)
        out.append(Plate(f"Foot.{t}", f"Foot.{t}", foot, offset=0.004, centre=np.array([s * HIP[0], -0.04, 0.06]),
                         radius=0.13))
        out.append(Plate(f"Toe.{t}", f"Toe.{t}", intersect(plane((0, -1, 0), (0, -0.118, 0)),
                                                         plane((0, 0, -1), (0, 0, 0.012)), side),
                         offset=0.004, centre=np.array([s * HIP[0], -0.17, 0.03]), radius=0.08))
    return out
