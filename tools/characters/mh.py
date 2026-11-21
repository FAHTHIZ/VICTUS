# SPDX-License-Identifier: Apache-2.0
"""Reader for the MakeHuman CC0 assets vendored in third_party/makehuman (docs/CHARACTERS.md, ADR-0179).

The VICTUS starter characters start from the MakeHuman hm08 base mesh: an anatomically modelled,
all-quad human (13 380 body vertices, edge loops around eyes, mouth and joints) with a UV layout,
eye/teeth/tongue/eyelash helper geometry, macro shape targets (gender, age, muscle, weight,
ethnicity), a 163-bone reference rig with symmetric skin weights and 59 facial pose units. These
assets were released by the MakeHuman project under CC0 1.0 (third_party/makehuman/LICENSE.ASSETS.md);
only the data is used, none of MakeHuman's AGPL code.

Everything here works in MakeHuman space: decimetres, Y up, +Z forward, character left on +X.
fit.py converts to VICTUS authoring space.
"""
from __future__ import annotations

import gzip
import json
import math
import os
from dataclasses import dataclass, field

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "third_party", "makehuman")


def _open(name):
    p = os.path.join(ROOT, name)
    if os.path.exists(p + ".gz"):
        return gzip.open(p + ".gz", "rt", encoding="utf-8")
    return open(p, encoding="utf-8")


# ---- meshes -----------------------------------------------------------------------------------


@dataclass
class ObjMesh:
    v: np.ndarray  # (N, 3)
    vt: np.ndarray  # (M, 2)
    faces: list  # per face: list of (v, vt) zero-based
    groups: dict  # group name -> list of face indices
    face_material: list = field(default_factory=list)

    def group_faces(self, *names):
        out = []
        for n in names:
            out.extend(self.groups.get(n, []))
        return out

    def group_verts(self, *names):
        return sorted({c[0] for i in self.group_faces(*names) for c in self.faces[i]})


def load_obj(name) -> ObjMesh:
    V, VT, faces, groups, mats = [], [], [], {}, []
    g, m = None, None
    with _open(name) as fh:
        for line in fh:
            if line.startswith("v "):
                V.append([float(x) for x in line.split()[1:4]])
            elif line.startswith("vt "):
                VT.append([float(x) for x in line.split()[1:3]])
            elif line.startswith("g "):
                g = line.split()[1]
                groups.setdefault(g, [])
            elif line.startswith("usemtl "):
                m = line.split()[1]
            elif line.startswith("f "):
                f = []
                for t in line.split()[1:]:
                    p = t.split("/")
                    f.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1))
                groups.setdefault(g, []).append(len(faces))
                faces.append(f)
                mats.append(m)
    return ObjMesh(np.array(V), np.array(VT) if VT else np.zeros((0, 2)), faces, groups, mats)


def load_target(name):
    idx, d = [], []
    with _open(name) as fh:
        for line in fh:
            if not line.strip() or line[0] == "#":
                continue
            s = line.split()
            idx.append(int(s[0]))
            d.append([float(x) for x in s[1:4]])
    return np.array(idx, dtype=np.int64), np.array(d).reshape(-1, 3)


# ---- macro modifiers (the MakeHuman macro model, re-implemented from its documented weighting) --


@dataclass
class Macro:
    gender: float = 0.5  # 0 female .. 1 male
    age: float = 0.5  # 0.5 = 25 years ("young"); only young targets are vendored
    muscle: float = 0.5
    weight: float = 0.5
    african: float = 1 / 3
    asian: float = 1 / 3
    caucasian: float = 1 / 3


def _tri(x):
    """(min, average, max) weights of a 0..1 macro slider."""
    lo = max(0.0, (0.5 - x) * 2.0)
    hi = max(0.0, (x - 0.5) * 2.0)
    return {"min": lo, "average": 1.0 - lo - hi, "max": hi}


def macro_offsets(m: Macro, nverts):
    if abs(m.age - 0.5) > 1e-6:
        raise ValueError("only the young (25 years) macro targets are vendored")
    off = np.zeros((nverts, 3))
    gw = {"female": 1.0 - m.gender, "male": m.gender}
    mw, ww = _tri(m.muscle), _tri(m.weight)
    for g, a in gw.items():
        if a <= 0:
            continue
        for mk, b in mw.items():
            for wk, c in ww.items():
                w = a * b * c
                if w <= 1e-6:
                    continue
                i, d = load_target(f"targets/universal-{g}-young-{mk}muscle-{wk}weight.target")
                off[i] += w * d
        total = m.african + m.asian + m.caucasian
        for race in ("african", "asian", "caucasian"):
            w = a * getattr(m, race) / total
            if w > 1e-6:
                i, d = load_target(f"targets/{race}-{g}-young.target")
                off[i] += w * d
    return off


# ---- proxies (.mhclo: barycentric binding to base-mesh triangles plus a scaled offset) ---------


def load_mhclo(name):
    refs, ws, offs, scales = [], [], [], {}
    in_verts = False
    with _open(name) as fh:
        for line in fh:
            s = line.split()
            if not s or s[0].startswith("#"):
                continue
            if s[0] in ("x_scale", "y_scale", "z_scale"):
                scales[s[0][0]] = (int(s[1]), int(s[2]), float(s[3]))
            elif s[0] == "verts":
                in_verts = True
            elif in_verts and len(s) == 9:
                refs.append([int(x) for x in s[:3]])
                ws.append([float(x) for x in s[3:6]])
                offs.append([float(x) for x in s[6:9]])
            elif in_verts and len(s) == 1 and s[0].isdigit():
                refs.append([int(s[0])] * 3)
                ws.append([1.0, 0.0, 0.0])
                offs.append([0.0, 0.0, 0.0])
            elif in_verts and len(s) < 9:
                in_verts = False
    return np.array(refs), np.array(ws), np.array(offs), scales


def fit_proxy(base_v, clo):
    refs, ws, offs, scales = clo
    sc = np.ones(3)
    for ax, k in enumerate("xyz"):
        if k in scales:
            a, b, den = scales[k]
            sc[ax] = abs(base_v[a, ax] - base_v[b, ax]) / den
    return (base_v[refs] * ws[:, :, None]).sum(1) + offs * sc


# ---- rig ------------------------------------------------------------------------------------


@dataclass
class MhRig:
    bones: dict  # name -> dict(parent, head, tail)
    joints: dict  # joint name -> vertex list
    weights: dict  # bone -> (vertex idx, weight)
    order: list  # parents before children

    def joint_pos(self, v, jname):
        return v[self.joints[jname]].mean(0)

    def head_tail(self, v, bone):
        b = self.bones[bone]
        return self.joint_pos(v, b["head"]), self.joint_pos(v, b["tail"])


def load_rig() -> MhRig:
    with _open("default.mhskel") as fh:
        sk = json.load(fh)
    with _open("default_weights.mhw") as fh:
        wt = json.load(fh)["weights"]
    bones = sk["bones"]
    order, seen = [], set()

    def visit(n):
        if n in seen:
            return
        p = bones[n]["parent"]
        if p:
            visit(p)
        seen.add(n)
        order.append(n)

    for n in sorted(bones):
        visit(n)
    weights = {b: (np.array([p[0] for p in w], dtype=np.int64), np.array([p[1] for p in w])) for b, w in wt.items()}
    return MhRig(bones, sk["joints"], weights, order)


# ---- BVH (face pose units) -----------------------------------------------------------------


@dataclass
class Bvh:
    names: list
    parent: list
    offset: np.ndarray
    channels: list  # per joint: list of channel names
    frames: np.ndarray  # (F, C)

    def rotations(self, frame):
        """World-aligned local rotation matrix per joint (BVH joints carry no rest rotation)."""
        row = self.frames[frame]
        out, c = [], 0
        for ch in self.channels:
            R = np.eye(3)
            for name in ch:
                val = row[c]
                c += 1
                if name.endswith("rotation"):
                    R = R @ _axis_rot(name[0], val)
            out.append(R)
        return out


def _axis_rot(axis, deg):
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    if axis == "X":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == "Y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def load_bvh(name="face-poseunits.bvh") -> Bvh:
    with _open(name) as fh:
        tokens = fh.read().split()
    names, parent, offset, channels = [], [], [], []
    stack = []
    i = 0
    while tokens[i] != "MOTION":
        t = tokens[i]
        if t in ("ROOT", "JOINT"):
            names.append(tokens[i + 1])
            parent.append(stack[-1] if stack else -1)
            i += 2
        elif t == "End":
            names.append(None)
            parent.append(stack[-1])
            i += 2
        elif t == "{":
            stack.append(len(names) - 1)
            i += 1
        elif t == "}":
            stack.pop()
            i += 1
        elif t == "OFFSET":
            offset.append([float(x) for x in tokens[i + 1:i + 4]])
            i += 4
        elif t == "CHANNELS":
            n = int(tokens[i + 1])
            channels.append(tokens[i + 2:i + 2 + n])
            i += 2 + n
        else:
            i += 1
    # End sites have an OFFSET but no CHANNELS.
    ch_full, k = [], 0
    for n in names:
        if n is None:
            ch_full.append([])
        else:
            ch_full.append(channels[k])
            k += 1
    i += 1
    nframes = int(tokens[i + 1])
    i += 5  # "Frames:" n "Frame" "Time:" t
    data = np.array([float(x) for x in tokens[i:]]).reshape(nframes, -1)
    return Bvh(names, parent, np.array(offset), ch_full, data)


def face_unit_names():
    with _open("face-poseunits.json") as fh:
        return json.load(fh)["framemapping"]


def pose_skin(rest_v, rig: MhRig, bvh: Bvh, frame, bones=None):
    """Linear-blend-skins rest_v with one BVH frame (joint positions from the targeted mesh,
    rotations from the BVH, so a pose fits any body shape)."""
    idx = {n: i for i, n in enumerate(bvh.names) if n}
    rots = bvh.rotations(frame)
    return skin(rest_v, rig, {b: rots[idx[b]] for b in rig.bones if b in idx}, bones)


def bone_transforms(rest_v, rig: MhRig, rots):
    """World transforms (R, posed head, rest head) per bone for world-aligned local rotations
    `rots` (bone -> 3x3; missing = identity): BVH semantics, a bone turns its subtree about its head."""
    out = {}
    for b in rig.order:
        h, _ = rig.head_tail(rest_v, b)
        p = rig.bones[b]["parent"]
        R = rots.get(b, np.eye(3))
        if p is None:
            out[b] = (R, h, h)
        else:
            Rp, tp, hp = out[p]
            out[b] = (Rp @ R, tp + Rp @ (h - hp), h)
    return out


def skin(rest_v, rig: MhRig, rots, bones=None, verts=None):
    """Linear blend skinning of rest_v (or of the points `verts` bound with the same weights)."""
    X = rest_v if verts is None else verts
    T = bone_transforms(rest_v, rig, rots)
    acc = np.zeros_like(X)
    total = np.zeros(len(X))
    for b, (vi, w) in rig.weights.items():
        R, t, h = T[b]
        if bones is not None and b not in bones:
            R, t, h = np.eye(3), h, h
        acc[vi] += w[:, None] * ((X[vi] - h) @ R.T + t)
        total[vi] += w
    out = X.copy()
    m = total > 1e-9
    out[m] = acc[m] / total[m, None]
    return out
