# SPDX-License-Identifier: Apache-2.0
"""Fits the MakeHuman base (mh.py) into a VICTUS character body (docs/CHARACTERS.md, ADR-0179).

Steps, all deterministic NumPy:
  1. macro targets shape the identity (gender, muscle, weight, ethnicity mix);
  2. facial pose units are evaluated on the shaped mesh and stored as blend-shape deltas;
  3. the arms are reposed from MakeHuman's bent rest pose into the VICTUS A-pose (straight arms
     40 degrees from vertical, palms toward the thighs), by skinning with the reference weights;
  4. the result is converted to VICTUS authoring space (metres, Z up, -Y forward, feet on Z = 0)
     and scaled to the character's height;
  5. the 163-bone reference rig is collapsed onto the VICTUS Humanoid skeleton: the 21 core joints
     of the original starter rig (same names, order and hierarchy) followed by twist, finger, jaw
     and eye joints; each VICTUS joint sums the weights of the reference bones it absorbs.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

import mh

ARM_FROM_VERTICAL = 40.0  # the original starter rig's A-pose

# The original 21-joint starter skeleton, in its original order (both arms, then both legs).
CORE = (
    "Hips", "Spine", "Chest", "Neck", "Head",
    "Shoulder.L", "UpperArm.L", "LowerArm.L", "Hand.L", "Shoulder.R", "UpperArm.R", "LowerArm.R", "Hand.R",
    "UpperLeg.L", "LowerLeg.L", "Foot.L", "Toe.L", "UpperLeg.R", "LowerLeg.R", "Foot.R", "Toe.R",
)
FINGERS = (("Thumb", 1), ("Index", 2), ("Middle", 3), ("Ring", 4), ("Pinky", 5))


def skeleton_layout():
    """(name, parent) of every VICTUS Humanoid joint: CORE first (indices 0..20 unchanged), then
    per side the twist joints and three joints per finger, then Jaw and the eyes."""
    out = [("Hips", None), ("Spine", "Hips"), ("Chest", "Spine"), ("Neck", "Chest"), ("Head", "Neck")]
    for t in ("L", "R"):
        out += [(f"Shoulder.{t}", "Chest"), (f"UpperArm.{t}", f"Shoulder.{t}"), (f"LowerArm.{t}", f"UpperArm.{t}"),
                (f"Hand.{t}", f"LowerArm.{t}")]
    for t in ("L", "R"):
        out += [(f"UpperLeg.{t}", "Hips"), (f"LowerLeg.{t}", f"UpperLeg.{t}"), (f"Foot.{t}", f"LowerLeg.{t}"),
                (f"Toe.{t}", f"Foot.{t}")]
    assert tuple(n for n, _ in out) == CORE
    for t in ("L", "R"):
        out += [(f"UpperArmTwist.{t}", f"UpperArm.{t}"), (f"LowerArmTwist.{t}", f"LowerArm.{t}"),
                (f"UpperLegTwist.{t}", f"UpperLeg.{t}")]
        for f, _ in FINGERS:
            out += [(f"{f}1.{t}", f"Hand.{t}"), (f"{f}2.{t}", f"{f}1.{t}"), (f"{f}3.{t}", f"{f}2.{t}")]
    out += [("Jaw", "Head"), ("Eye.L", "Head"), ("Eye.R", "Head")]
    return out


# Reference bone -> VICTUS joint. Unlisted face bones go to Head; the jaw subtree to Jaw.
def bone_map(rig: mh.MhRig):
    m = {"root": "Hips", "spine05": "Hips", "pelvis.L": "Hips", "pelvis.R": "Hips",
         "spine04": "Spine", "spine03": "Spine", "spine02": "Chest", "spine01": "Chest",
         "breast.L": "Chest", "breast.R": "Chest",
         "neck01": "Neck", "neck02": "Neck", "neck03": "Neck", "head": "Head",
         "eye.L": "Eye.L", "eye.R": "Eye.R"}
    for t in ("L", "R"):
        m.update({f"clavicle.{t}": f"Shoulder.{t}", f"shoulder01.{t}": f"Shoulder.{t}",
                  f"upperarm01.{t}": f"UpperArm.{t}", f"upperarm02.{t}": f"UpperArm.{t}",
                  f"lowerarm01.{t}": f"LowerArm.{t}", f"lowerarm02.{t}": f"LowerArmTwist.{t}",
                  f"wrist.{t}": f"Hand.{t}",
                  f"upperleg01.{t}": f"UpperLeg.{t}", f"upperleg02.{t}": f"UpperLeg.{t}",
                  f"lowerleg01.{t}": f"LowerLeg.{t}", f"lowerleg02.{t}": f"LowerLeg.{t}",
                  f"foot.{t}": f"Foot.{t}"})
        for i in range(1, 5):
            m[f"metacarpal{i}.{t}"] = f"Hand.{t}"
        for f, n in FINGERS:
            for j in (1, 2, 3):
                m[f"finger{n}-{j}.{t}"] = f"{f}{j}.{t}"
        for b in rig.bones:
            if b.startswith("toe") and b.endswith(f".{t}"):
                m[b] = f"Toe.{t}"
    jaw_tree = {"jaw"}
    for b in rig.order:
        if rig.bones[b]["parent"] in jaw_tree:
            jaw_tree.add(b)
    for b in rig.order:
        if b not in m:
            m[b] = "Jaw" if b in jaw_tree else "Head"
    return m


# ---- character specs -------------------------------------------------------------------------


@dataclass
class Spec:
    key: str
    name: str
    macro: mh.Macro
    skull_top: float  # metres, top of the scalp (hair adds to it)
    profile: str
    # Facial identity: extra MakeHuman detail targets are not vendored; identity comes from the
    # macro mix plus these proportional tweaks applied in VICTUS space (see _identity).
    tweaks: dict = field(default_factory=dict)
    # Heroic proportions (the ORION & NOVA design sheet): the head is scaled about the base of the
    # skull before the height is normalised, so the body grows to fill the freed height (about
    # eight heads tall instead of MakeHuman's seven).
    head_scale: float = 1.0
    # Leg length factor (hip joint to sole, applied through the leg skin weights).
    leg_scale: float = 1.0
    # The sheet's slim neck column: the neck is narrowed radially by this factor.
    neck_scale: float = 1.0


ORION = Spec("orion", "Orion", mh.Macro(gender=1.0, muscle=0.92, weight=0.46, african=0.55, asian=0.10, caucasian=0.35),
             skull_top=1.785, profile="VictusHumanoid.Orion", head_scale=0.86,
             leg_scale=1.04, neck_scale=0.86)
NOVA = Spec("nova", "Nova", mh.Macro(gender=0.0, muscle=0.52, weight=0.15, african=0.0, asian=0.85, caucasian=0.15),
            skull_top=1.695, profile="VictusHumanoid.Nova", head_scale=0.88, leg_scale=1.09,
            neck_scale=0.84)
SPECS = {"orion": ORION, "nova": NOVA}


# ---- geometry helpers ------------------------------------------------------------------------


def rot_between(a, b):
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        if c > 0:
            return np.eye(3)
        p = np.array([1.0, 0, 0]) if abs(a[0]) < 0.9 else np.array([0, 1.0, 0])
        v = np.cross(a, p)
        v /= np.linalg.norm(v)
        return 2 * np.outer(v, v) - np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1.0 / (1.0 + c))


def axis_angle(axis, ang):
    a = axis / np.linalg.norm(axis)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * K @ K


def to_victus(p):
    """MakeHuman (dm, Y up, +Z forward) -> VICTUS authoring (m, Z up, -Y forward)."""
    p = np.asarray(p)
    return np.stack([p[..., 0], -p[..., 2], p[..., 1]], -1) * 0.1


def arm_rest_rotations(v, rig: mh.MhRig):
    """World-aligned rotations straightening each arm into the VICTUS A-pose."""
    rots = {}
    for t, side in (("L", 1.0), ("R", -1.0)):
        s, _ = rig.head_tail(v, f"upperarm01.{t}")
        e, _ = rig.head_tail(v, f"lowerarm01.{t}")
        w, _ = rig.head_tail(v, f"wrist.{t}")
        # Upper arm: 40 degrees from vertical in the frontal plane (MakeHuman is within ~1 degree).
        a = math.radians(ARM_FROM_VERTICAL)
        target = np.array([side * math.sin(a), -math.cos(a), 0.0])
        R_up = rot_between(e - s, target)
        rots[f"upperarm01.{t}"] = R_up
        # Forearm continues the upper arm.
        fore = R_up @ (w - e)
        R_lo = rot_between(fore, target)
        rots[f"lowerarm01.{t}"] = R_up.T @ R_lo @ R_up
        W1 = R_lo @ R_up  # world rotation of the forearm
        # Hand: along the forearm, thumb side forward (+Z in MakeHuman), palm toward the thigh.
        k3, _ = rig.head_tail(v, f"finger3-1.{t}")
        k2, _ = rig.head_tail(v, f"finger2-1.{t}")
        k5, _ = rig.head_tail(v, f"finger5-1.{t}")
        hd = W1 @ (k3 - w)
        R_h = rot_between(hd, target)
        thumb_side = R_h @ W1 @ (k2 - k5)
        thumb_side -= target * np.dot(thumb_side, target)
        fwd = np.array([0, 0, 1.0])
        ang = math.atan2(np.dot(np.cross(thumb_side, fwd), target), np.dot(thumb_side, fwd))
        R_roll = axis_angle(target, ang)
        rots[f"wrist.{t}"] = W1.T @ R_roll @ R_h @ W1
        # Legs: MakeHuman stands with the ankles ~40 cm apart; bring them in to a relaxed stance
        # (ankle 3 cm outside the hip joint) and turn the foot back so the sole stays flat.
        hip, _ = rig.head_tail(v, f"upperleg01.{t}")
        ankle, _ = rig.head_tail(v, f"foot.{t}")
        leg = ankle - hip
        want = np.array([side * 0.3, leg[1], leg[2]])
        want *= np.linalg.norm(leg) / np.linalg.norm(want)
        R_leg = rot_between(leg, want)
        rots[f"upperleg01.{t}"] = R_leg
        rots[f"foot.{t}"] = R_leg.T
    return rots


# ---- the fitted body -------------------------------------------------------------------------


@dataclass
class Body:
    spec: Spec
    obj: mh.ObjMesh
    v: np.ndarray  # VICTUS space, all base vertices (body + helpers)
    eyes_v: np.ndarray  # high-poly eye proxy vertices, VICTUS space
    eyes: mh.ObjMesh
    morphs: dict  # name -> (N, 3) deltas over self.v (VICTUS space)
    eye_morphs: dict
    joints: list  # (name, parent, head, tail) VICTUS space
    weights: np.ndarray  # (N, J) dense skin weights over joints (normalised)
    eye_weights: np.ndarray
    scale: float


# Facial pose units kept as blend shapes (VICTUS name <- MakeHuman unit). Eye rotation and jaw
# drop are driven by the Eye.L/R and Jaw joints instead.
FACE_UNITS = (
    ("browDown_L", "LeftBrowDown"), ("browDown_R", "RightBrowDown"),
    ("browOuterUp_L", "LeftOuterBrowUp"), ("browOuterUp_R", "RightOuterBrowUp"),
    ("browInnerUp_L", "LeftInnerBrowUp"), ("browInnerUp_R", "RightInnerBrowUp"),
    ("noseSneer", "NoseWrinkler"),
    ("eyeWide_L", "LeftUpperLidOpen"), ("eyeWide_R", "RightUpperLidOpen"),
    ("eyeBlink_L", "LeftUpperLidClosed"), ("eyeBlink_R", "RightUpperLidClosed"),
    ("eyeSquint_L", "LeftLowerLidUp"), ("eyeSquint_R", "RightLowerLidUp"),
    ("cheekSquint_L", "LeftCheekUp"), ("cheekSquint_R", "RightCheekUp"),
    ("cheekPuff", "CheeksPump"), ("cheekSuck", "CheeksSuck"), ("nasolabialDeepen", "NasolabialDeepener"),
    ("jawLeft", "ChinLeft"), ("jawRight", "ChinRight"), ("chinDown", "ChinDown"), ("jawForward", "ChinForward"),
    ("mouthShrugLower", "lowerLipUp"), ("mouthLowerDown", "lowerLipDown"),
    ("mouthRollLower", "lowerLipBackward"), ("mouthLowerForward", "lowerLipForward"),
    ("mouthUpperUp", "UpperLipUp"), ("mouthRollUpper", "UpperLipBackward"),
    ("mouthUpperForward", "UpperLipForward"), ("mouthUpperStretch", "UpperLipStretched"),
    ("jawOpenStretch", "JawDropStretched"), ("mouthPucker", "LipsKiss"),
    ("mouthLeft", "MouthMoveLeft"), ("mouthRight", "MouthMoveRight"),
    ("mouthSmile_L", "MouthLeftPullUp"), ("mouthSmile_R", "MouthRightPullUp"),
    ("mouthStretch_L", "MouthLeftPullSide"), ("mouthStretch_R", "MouthRightPullSide"),
    ("mouthFrown_L", "MouthLeftPullDown"), ("mouthFrown_R", "MouthRightPullDown"),
    ("mouthPlatysma_L", "MouthLeftPlatysma"), ("mouthPlatysma_R", "MouthRightPlatysma"),
    ("tongueOut", "TongueOut"), ("tongueUp", "TongueUp"),
)


def heroic_head(v0, rig: mh.MhRig, k):
    """Scales the head by k about the base of the skull (MakeHuman space), ramping in over the
    neck so the throat narrows smoothly into the smaller head. Helper geometry (eyes, teeth)
    is transformed with it, so proxies and the eye joints stay in place relative to the face."""
    if abs(k - 1.0) < 1e-6:
        return v0
    pivot = rig.head_tail(v0, "head")[0]
    y0 = rig.head_tail(v0, "neck02")[0][1]
    t = np.clip((v0[:, 1] - y0) / max(pivot[1] - y0, 1e-6), 0, 1)
    t = t * t * (3 - 2 * t)
    # Only the head and neck column: the arms never reach this high in the MakeHuman rest pose,
    # but the shoulders' tops do - keep them out by the distance from the neck axis.
    r = np.hypot(v0[:, 0] - pivot[0], v0[:, 2] - pivot[2])
    radial = np.clip((1.4 - r) / 0.4, 0, 1)
    t *= np.maximum(radial, np.clip((v0[:, 1] - pivot[1]) / 0.15, 0, 1))
    f = 1.0 + (k - 1.0) * t
    return pivot + (v0 - pivot) * f[:, None]


def slim_neck(v0, rig: mh.MhRig, k):
    """Narrows the neck column by k about its axis (MakeHuman space, Y up), weighted by the neck
    bones' reference skin weights and kept to the column so the trapezius slopes stay put."""
    if abs(k - 1.0) < 1e-6:
        return v0
    w = np.zeros(len(v0))
    for b, (vi, wt) in rig.weights.items():
        if b.startswith("neck"):
            np.add.at(w, vi, wt)
    w = np.clip(w, 0, 1)
    a = rig.head_tail(v0, "neck01")[0]
    b = rig.head_tail(v0, "head")[0]
    t = np.clip((v0[:, 1] - a[1]) / max(b[1] - a[1], 1e-6), 0, 1)
    c = a + (b - a) * t[:, None]
    d = v0 - c
    d[:, 1] = 0.0
    r = np.linalg.norm(d, axis=1)
    w *= np.clip((1.1 - r) / 0.3, 0, 1)
    out = v0 + d * ((k - 1.0) * w)[:, None]
    return out


def long_legs(v0, rig: mh.MhRig, k):
    """Stretches the legs by k below the hip joints (MakeHuman space, Y up), blended by the legs'
    reference skin weights so the pelvis and crotch stay continuous and the hands (which hang
    beside the thighs) are untouched."""
    if abs(k - 1.0) < 1e-6:
        return v0
    w = np.zeros(len(v0))
    for b, (vi, wt) in rig.weights.items():
        if b.startswith(("upperleg", "lowerleg", "foot", "toe")):
            np.add.at(w, vi, wt)
    w = np.clip(w, 0, 1)
    yh = 0.5 * (rig.head_tail(v0, "upperleg01.L")[0][1] + rig.head_tail(v0, "upperleg01.R")[0][1])
    dy = np.minimum(v0[:, 1] - yh, 0.0) * (k - 1.0) * w
    out = v0.copy()
    out[:, 1] += dy
    return out


def build(spec: Spec) -> Body:
    obj = mh.load_obj("base.obj")
    v0 = obj.v + mh.macro_offsets(spec.macro, len(obj.v))
    rig = mh.load_rig()
    v0 = heroic_head(v0, rig, spec.head_scale)
    v0 = long_legs(v0, rig, spec.leg_scale)
    v0 = slim_neck(v0, rig, spec.neck_scale)
    eyes = mh.load_obj("high-poly.obj")
    clo = mh.load_mhclo("high-poly.mhclo")
    eye0 = mh.fit_proxy(v0, clo)

    # Facial blend shapes on the shaped, MakeHuman-rest mesh; the head is untouched by the arm
    # repose, so the deltas carry over unchanged (only rotated into VICTUS axes and scaled).
    bvh = mh.load_bvh()
    units = mh.face_unit_names()
    morphs_mh = {}
    for name, unit in FACE_UNITS:
        posed = mh.pose_skin(v0, rig, bvh, units.index(unit))
        morphs_mh[name] = posed - v0
    eye_morphs_mh = {n: mh.fit_proxy(v0 + d, clo) - eye0 for n, d in morphs_mh.items()}

    rots = arm_rest_rotations(v0, rig)
    v1 = mh.skin(v0, rig, rots)
    eye1 = mh.fit_proxy(v1, clo)
    T = mh.bone_transforms(v0, rig, rots)

    def posed(p, bone):
        R, t, h = T[bone]
        return (p - h) @ R.T + t

    body_ids = obj.group_verts("body")
    ground = v1[body_ids, 1].min()
    top = v1[body_ids, 1].max()
    s = spec.skull_top / ((top - ground) * 0.1)

    def conv(p):
        q = np.asarray(p, float).copy()
        q[..., 1] -= ground
        return to_victus(q) * s

    def conv_d(d):
        return to_victus(d) * s

    v = conv(v1)
    eyes_v = conv(eye1)
    morphs = {n: conv_d(d) for n, d in morphs_mh.items()}
    eye_morphs = {n: conv_d(d) for n, d in eye_morphs_mh.items()}

    # Joint positions from the reposed reference rig.
    def head(b):
        return conv(posed(rig.head_tail(v0, b)[0], b))

    def tail(b):
        return conv(posed(rig.head_tail(v0, b)[1], b))

    P = {"Hips": (head("spine05"), head("spine04")), "Spine": (head("spine04"), head("spine02")),
         "Chest": (head("spine02"), head("neck01")), "Neck": (head("neck01"), head("head")),
         "Head": (head("head"), tail("head")), "Jaw": (head("jaw"), tail("jaw"))}
    for t in ("L", "R"):
        sh, el, wr = head(f"upperarm01.{t}"), head(f"lowerarm01.{t}"), head(f"wrist.{t}")
        P[f"Shoulder.{t}"] = (head(f"clavicle.{t}"), sh)
        P[f"UpperArm.{t}"] = (sh, el)
        P[f"UpperArmTwist.{t}"] = (sh + (el - sh) * 0.5, el)
        P[f"LowerArm.{t}"] = (el, wr)
        P[f"LowerArmTwist.{t}"] = (head(f"lowerarm02.{t}"), wr)
        P[f"Hand.{t}"] = (wr, head(f"finger3-1.{t}"))
        for f, n in FINGERS:
            for j in (1, 2, 3):
                b = f"finger{n}-{j}.{t}"
                P[f"{f}{j}.{t}"] = (head(b), tail(b))
        hip, knee, ankle = head(f"upperleg01.{t}"), head(f"lowerleg01.{t}"), head(f"foot.{t}")
        P[f"UpperLeg.{t}"] = (hip, knee)
        P[f"UpperLegTwist.{t}"] = (hip + (knee - hip) * 0.5, knee)
        P[f"LowerLeg.{t}"] = (knee, ankle)
        ball = np.mean([head(f"toe{i}-1.{t}") for i in (2, 3, 4)], axis=0)
        ball[2] = max(ball[2], 0.02)
        P[f"Foot.{t}"] = (ankle, ball)
        # Toe tip: the farthest-forward foot vertex on this side.
        side = v[body_ids]
        side = side[(side[:, 0] * (1 if t == "L" else -1) > 0) & (side[:, 2] < 0.06)]
        tip = np.array([ball[0], side[:, 1].min(), ball[2]])
        P[f"Toe.{t}"] = (ball, tip)
        e = conv(posed(rig.head_tail(v0, f"eye.{t}")[0], f"eye.{t}"))
        P[f"Eye.{t}"] = (e, e + np.array([0, -0.02, 0]))
    layout = skeleton_layout()
    joints = [(n, p, P[n][0], P[n][1]) for n, p in layout]

    # Skin weights: each VICTUS joint sums its reference bones.
    jidx = {n: i for i, (n, _) in enumerate(layout)}
    bmap = bone_map(rig)
    W = np.zeros((len(v), len(layout)))
    for b, (vi, w) in rig.weights.items():
        W[vi, jidx[bmap[b]]] += w
    # Upper-arm and thigh twist: the lower half of the limb follows the twist joint, ramping in
    # from the middle so a roll of the twist joint spreads along the skin.
    for t in ("L", "R"):
        for limb, tw in ((f"UpperArm.{t}", f"UpperArmTwist.{t}"), (f"UpperLeg.{t}", f"UpperLegTwist.{t}")):
            a, b = P[limb]
            ax = b - a
            u = np.clip(((v - a) @ ax) / (ax @ ax), 0, 1)
            f = np.clip((u - 0.25) / 0.6, 0, 1)
            f = f * f * (3 - 2 * f) * 0.85
            j0, j1 = jidx[limb], jidx[tw]
            moved = W[:, j0] * f
            W[:, j0] -= moved
            W[:, j1] += moved
    W = _limit(W, 4)
    eye_W = np.zeros((len(eyes_v), len(layout)))
    half = eyes_v[:, 0] > 0
    eye_W[half, jidx["Eye.L"]] = 1
    eye_W[~half, jidx["Eye.R"]] = 1
    return Body(spec, obj, v, eyes_v, eyes, morphs, eye_morphs, joints, W, eye_W, s)


def _limit(W, k):
    """Keeps the k largest influences per vertex and renormalises."""
    out = np.zeros_like(W)
    top = np.argsort(-W, axis=1)[:, :k]
    rows = np.arange(len(W))[:, None]
    out[rows, top] = W[rows, top]
    s = out.sum(1, keepdims=True)
    out[s[:, 0] < 1e-9, 0] = 1.0
    return out / np.maximum(out.sum(1, keepdims=True), 1e-9)
