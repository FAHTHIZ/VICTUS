# SPDX-License-Identifier: Apache-2.0
"""Mannequin assembly: turns a fitted body (fit.py) into the VICTUS Frame mannequin, the look of the
Orion and Nova starter characters (docs/CHARACTERS.md, ADR-0198).

A mannequin instead of a realistic human: nothing on it depends on skin, hair or eye shading the
renderer does not have (subsurface scattering, strand hair), and every surface shows something the
engine does well - layered PBR (satin ceramic, brushed metal, rubberised undersuit, glossy glass),
emissive trim with bloom, normal-mapped panel detail, skinning and blend shapes.

Parts (one glTF primitive per material):
  Frame   the undersuit body: the MakeHuman body smoothed into an idealised form (no nipples,
          navel or toes), from the collar to the ankles, with both hands; a featureless head shell
          (the head's blurred volume, shrink-wrapped by a quad cube-sphere and cut flat under the
          jaw so it sits on the neck like a mannequin's);
  Armor   hard-surface plates grown from the body (chest, back, abdomen, hips, shoulders, arms,
          thighs, knees, shins, the backs of the hands), boots, the ear pods and the back core.
          Every plate is a shell over a region bounded by planes: its vertices are pushed out along
          the rest normal and keep the weights of the body vertex they came from, its edge is
          snapped onto the bounding planes (clean, straight cut lines) and rolls back under itself;
  Visor   the faceplate (a glass shell over the front of the head) and the face display: two eye
          glyphs and a mouth bar floating just above the glass, whose blend shapes (eyeBlink_L,
          mouthSmile_R, jawOpen, ...) give the facial clips something to drive.

Every part is a quad mesh, so one Catmull-Clark step (meshops.catmull_clark) subdivides weights,
morphs and positions consistently for LOD 0.
"""
from __future__ import annotations

import os
from dataclasses import dataclass

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

import fit
import gear
import plates
from assemble import Part, joint_map
from meshops import Quads, repack_islands, triangulate, vertex_normals


@dataclass
class Style:
    """Per-character look parameters (colours are linear RGB)."""
    frame: tuple  # undersuit
    armor: tuple  # plate ceramic
    accent: tuple  # secondary plate colour (stripes, trims)
    metal: tuple  # joint rings, boot soles, buckles
    glow: tuple  # emissive trim and the face display
    visor: tuple  # glass tint
    armor_rough: float = 0.32
    frame_rough: float = 0.62


STYLES = {
    # Orion (Frame 01): white ceramic plates over a graphite composite undersuit, brushed-metal
    # hardware, cyan light (the ORION & NOVA design sheet).
    "orion": Style(frame=(0.010, 0.011, 0.013), armor=(0.80, 0.81, 0.82), accent=(0.014, 0.016, 0.020),
                   metal=(0.56, 0.57, 0.60), glow=(0.10, 0.72, 1.0), visor=(0.003, 0.005, 0.008),
                   armor_rough=0.24, frame_rough=0.58),
    # Nova (Frame 02): pearl ceramic plates over a graphite undersuit, silver hardware, violet light.
    "nova": Style(frame=(0.012, 0.011, 0.015), armor=(0.83, 0.82, 0.84), accent=(0.017, 0.015, 0.024),
                  metal=(0.74, 0.74, 0.78), glow=(0.52, 0.26, 1.0), visor=(0.005, 0.003, 0.010),
                  armor_rough=0.20, frame_rough=0.52),
}

# Face labels of the Armor part (bake masks and per-piece looks).
ARMOR_KINDS = ("plate", "boot", "pod", "core", "helmet", "emblem")


def _edges(f):
    e = np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 3]], f[:, [3, 0]]])
    return np.unique(np.sort(e, 1), axis=0)


def taubin(v, f, mask, iters=12, lam=0.5, mu=-0.53):
    """Volume-preserving Laplacian smoothing (Taubin 1995) of the vertices where mask > 0."""
    e = _edges(f)
    deg = np.bincount(e.reshape(-1), minlength=len(v)).astype(float)
    deg = np.maximum(deg, 1)
    v = v.copy()
    m = mask[:, None]
    for _ in range(iters):
        for k in (lam, mu):
            acc = np.zeros_like(v)
            np.add.at(acc, e[:, 0], v[e[:, 1]])
            np.add.at(acc, e[:, 1], v[e[:, 0]])
            v = v + k * m * (acc / deg[:, None] - v)
    return v


def smooth01(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


# ---- head --------------------------------------------------------------------------------


class Head:
    """The featureless head: radial function r(dir) about `c` of the blurred head volume, cut by
    the jaw plane."""

    def __init__(self, J, body_v, body_f, plane, blur=0.013, cell=0.0025):
        self.J = J
        eye = 0.5 * (J["Eye.L"][0] + J["Eye.R"][0])
        self.eye = eye
        self.c = np.array([0.0, eye[1] + 0.078, eye[2] + 0.004])
        self.plane = plane  # (origin, normal pointing down/out of the head)
        tri = triangulate(body_f)
        cen = body_v[tri].mean(1)
        po, pn = plane
        keep = ((cen - po) @ pn < 0.03) & (np.linalg.norm(cen - self.c, axis=1) < 0.2)
        tri = tri[keep]
        # Dense surface samples with normals (signs the distance field).
        a, b, cc = body_v[tri[:, 0]], body_v[tri[:, 1]], body_v[tri[:, 2]]
        fn = np.cross(b - a, cc - a)
        area = 0.5 * np.linalg.norm(fn, axis=1)
        fn /= np.maximum(2 * area[:, None], 1e-12)
        rng = np.random.default_rng(7)
        cnt = np.maximum(1, np.rint(area / (0.0012 ** 2)).astype(int))
        ti = np.repeat(np.arange(len(tri)), cnt)
        r1, r2 = rng.random(len(ti)), rng.random(len(ti))
        s = np.sqrt(r1)
        pts = a[ti] * (1 - s)[:, None] + b[ti] * (s * (1 - r2))[:, None] + cc[ti] * (s * r2)[:, None]
        nrm = fn[ti]
        lo = self.c - 0.16
        n = int(np.ceil(0.32 / cell)) + 1
        g = np.stack(np.meshgrid(*[lo[k] + np.arange(n) * cell for k in range(3)], indexing="ij"), -1).reshape(-1, 3)
        d, nn = cKDTree(pts).query(g, workers=-1)
        inside = ((g - pts[nn]) * nrm[nn]).sum(1) < 0
        occ = (inside & (d < 0.12)).astype(float).reshape(n, n, n)
        occ = ndimage.gaussian_filter(occ, blur / cell)
        self.occ, self.lo, self.cell, self.n = occ, lo, cell, n

    def density(self, p):
        idx = ((p - self.lo) / self.cell).T
        o = ndimage.map_coordinates(self.occ, idx, order=1, mode="constant", cval=0.0)
        po, pn = self.plane
        h = (p - po) @ pn  # > 0 below the cut
        return np.minimum(o, 0.5 - h / 0.008)

    def inflated(self, amount):
        return Inflated(self, amount)

    def radius(self, d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        rs = np.linspace(0.01, 0.16, 301)
        P = self.c[None, None, :] + d[:, None, :] * rs[None, :, None]
        D = self.density(P.reshape(-1, 3)).reshape(len(d), len(rs))
        out = D >= 0.5
        # Outermost inside sample, then bisection to the 0.5 level.
        last = len(rs) - 1 - np.argmax(out[:, ::-1], axis=1)
        lo, hi = rs[last], rs[np.minimum(last + 1, len(rs) - 1)]
        for _ in range(16):
            mid = 0.5 * (lo + hi)
            v = self.density(self.c + d * mid[:, None])
            lo = np.where(v >= 0.5, mid, lo)
            hi = np.where(v >= 0.5, hi, mid)
        return 0.5 * (lo + hi)

    def surface(self, d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        return self.c + d * self.radius(d)[:, None]


class Inflated:
    """The head's radial surface pushed out along each direction by amount(dir) metres: the
    helmet, the visor glass and the glyphs are built on it."""

    def __init__(self, base, amount):
        self.base, self.amount, self.c = base, amount, base.c

    def surface(self, d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        a = self.amount(d) if callable(self.amount) else self.amount
        return self.c + d * (self.base.radius(d) + a)[:, None]


class HullSurface:
    """The convex hull of a radial surface, pushed out by amount(dir): helmets have no
    concavities (no nose, lips or eye sockets showing through)."""

    def __init__(self, base, amount, n=40):
        self.c, self.amount = base.c, amount
        pts = base.surface(gear.cube_sphere(n).v)
        self.eq = gear._hull(pts, 0.0)

    def radius(self, d):
        return gear._ray_exit(self.eq, self.c, d)

    def surface(self, d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        return self.c + d * (self.radius(d) + self.amount(d))[:, None]


class RadialMesh:
    """A star-shaped surface about c given by mesh vertices: the radius toward any direction is a
    Gaussian-weighted blend of the nearest vertex directions (smooth at the glyph scale)."""

    def __init__(self, c, v, offset=0.0):
        self.c = c
        rel = v - c
        self.r = np.linalg.norm(rel, axis=1) + offset
        self.tree = cKDTree(rel / np.linalg.norm(rel, axis=1, keepdims=True))

    def surface(self, d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        dist, idx = self.tree.query(d, k=12)
        w = np.exp(-(dist / max(float(np.median(dist[:, 3])), 1e-6)) ** 2)
        r = (w * self.r[idx]).sum(1) / w.sum(1)
        return self.c + d * r[:, None]


def head_quads(head, n=22):
    sph = gear.cube_sphere(n)
    e = _edges(sph.f)
    deg = np.bincount(e.reshape(-1), minlength=len(sph.v)).astype(float)
    p = head.surface(sph.v)
    for _ in range(12):  # even out the quads over the surface
        acc = np.zeros_like(p)
        np.add.at(acc, e[:, 0], p[e[:, 1]])
        np.add.at(acc, e[:, 1], p[e[:, 0]])
        p = head.surface(p + 0.5 * (acc / deg[:, None] - p) - head.c)
    return gear._orient_outward(Quads(p, sph.f, sph.uv, sph.fuv), head.c)


# ---- plate regions -----------------------------------------------------------------------


def _unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def _front(d):
    """Unit normal of the plane containing bone direction d and the X axis, pointing backwards
    (+Y), so `(p - o) . n < 0` selects the front half of a limb."""
    n = _unit(np.cross(d, [1.0, 0, 0]))
    return n if n[1] > 0 else -n


def plate_regions(J):
    """(name, planes, segment) per armour plate: the plate covers the body faces whose centre lies
    on the inner side ((p - o) . n < 0) of every plane and within `segment` = (a, b, radius) of a
    bone; its edge is snapped onto the planes."""
    hips, spine, chest, neck = J["Hips"][0], J["Spine"][0], J["Chest"][0], J["Neck"][0]
    zh, zn = hips[2], neck[2]

    def zt(t):  # torso height fraction: 0 at the hips joint, 1 at the base of the neck
        return zh + (zn - zh) * t

    Z = np.array([0, 0, 1.0])
    X = np.array([1.0, 0, 0])
    Y = np.array([0, 1.0, 0])
    torso = (hips, neck, 0.34)
    out = []
    yc = chest[1]
    out.append(("Chest", [(np.array([0, 0, zt(0.92)]), Z),
                          (np.array([0, 0, zt(0.50)]), _unit([0.45, 0, -1])),
                          (np.array([0, 0, zt(0.50)]), _unit([-0.45, 0, -1])),
                          (np.array([0.158, 0, 0]), X), (np.array([-0.158, 0, 0]), -X),
                          (np.array([0, yc - 0.01, 0]), Y)], torso))
    for i, (t0, t1, w) in enumerate(((0.335, 0.455, 0.092), (0.20, 0.315, 0.086), (0.07, 0.18, 0.08))):
        out.append((f"Abs{i}", [(np.array([0, 0, zt(t1)]), Z), (np.array([0, 0, zt(t0)]), -Z),
                                (np.array([w, 0, 0]), X), (np.array([-w, 0, 0]), -X),
                                (np.array([0, yc - 0.04, 0]), Y)], torso))
    out.append(("Back", [(np.array([0, 0, zt(0.92)]), Z), (np.array([0, 0, zt(0.50)]), -Z),
                         (np.array([0.150, 0, 0]), X), (np.array([-0.150, 0, 0]), -X),
                         (np.array([0, yc + 0.012, 0]), -Y)], torso))
    out.append(("Lumbar", [(np.array([0, 0, zt(0.46)]), Z), (np.array([0, 0, zt(0.22)]), -Z),
                           (np.array([0.075, 0, 0]), X), (np.array([-0.075, 0, 0]), -X),
                           (np.array([0, spine[1] + 0.0, 0]), -Y)], torso))
    out.append(("Pelvis", [(np.array([0, 0, zh + 0.035]), Z), (np.array([0, 0, zh - 0.10]), -Z),
                           (np.array([0.075, 0, 0]), X), (np.array([-0.075, 0, 0]), -X),
                           (np.array([0, hips[1] - 0.03, 0]), Y)], (hips + Z * 0.05, hips - Z * 0.15, 0.2)))
    for t, s in (("L", 1.0), ("R", -1.0)):
        side = (np.array([s * 0.02, 0, 0]), -s * X)  # this side of the body only
        sh = J[f"UpperArm.{t}"][0]
        el = J[f"LowerArm.{t}"][0]
        wr = J[f"Hand.{t}"][0]
        da = _unit(el - sh)
        outer = _unit(np.cross(Y, da))
        if outer[2] < 0:
            outer = -outer
        hp = J[f"UpperLeg.{t}"][0]
        kn = J[f"LowerLeg.{t}"][0]
        an = J[f"Foot.{t}"][0]
        out.append((f"Hip.{t}", [(np.array([0, 0, zh + 0.06]), Z), (np.array([0, 0, hp[2] - 0.12]), -Z),
                                 (np.array([s * (abs(hp[0]) + 0.025), 0, 0]), -s * X),
                                 (np.array([0, hp[1] - 0.05, 0]), -Y), (np.array([0, hp[1] + 0.065, 0]), Y)],
                    (hp + Z * 0.08, hp - Z * 0.16, 0.22)))
        out.append((f"Shoulder.{t}", [(sh + da * 0.115, da), (sh - outer * 0.012, -outer),
                                      (sh - Y * 0.078, -Y), (sh + Y * 0.078, Y),
                                      (np.array([s * (abs(sh[0]) - 0.07), 0, 0]), -s * X), side],
                    (J[f"Shoulder.{t}"][0], sh + da * 0.15, 0.13)))
        la = np.linalg.norm(el - sh)
        out.append((f"UpperArm.{t}", [(sh + da * (0.46 * la), -da), (sh + da * (0.84 * la), da),
                                      (sh + outer * 0.002, -outer), side], (sh, el, 0.08)))
        df = _unit(wr - el)
        of = _unit(outer - df * (outer @ df))
        lf = np.linalg.norm(wr - el)
        out.append((f"LowerArm.{t}", [(el + df * (0.20 * lf), -df), (el + df * (0.86 * lf), df),
                                      (el - of * 0.034, -of), side], (el, wr, 0.07)))
        m1 = J[f"Middle1.{t}"][0]
        dh = _unit(m1 - wr)
        i1, p1 = J[f"Index1.{t}"][0], J[f"Pinky1.{t}"][0]
        dors = _unit(np.cross(i1 - p1, dh))
        if dors[0] * s < 0:
            dors = -dors
        lh = np.linalg.norm(m1 - wr)
        across = _unit(i1 - p1)
        out.append((f"Hand.{t}", [(wr + dh * 0.014, -dh), (wr + dh * (lh * 0.90), dh), (wr + dors * 0.004, -dors),
                                  (i1 + across * 0.004, across), (p1 - across * 0.002, -across)], (wr, m1, 0.06)))
        dl = _unit(kn - hp)
        ll = np.linalg.norm(kn - hp)
        out.append((f"Thigh.{t}", [(hp + dl * (0.24 * ll), -dl), (hp + dl * (0.80 * ll), dl),
                                   (hp, _unit(_front(dl) - s * X * 0.55)), side], (hp, kn, 0.16)))
        ds = _unit(an - kn)
        ls = np.linalg.norm(an - kn)
        out.append((f"Knee.{t}", [(kn - ds * 0.05, -ds), (kn + ds * 0.055, ds), (kn - _front(ds) * 0.01, _front(ds)), side],
                    (kn - ds * 0.08, kn + ds * 0.08, 0.13)))
        out.append((f"Shin.{t}", [(kn + ds * (0.16 * ls), -ds), (kn + ds * (0.74 * ls), ds),
                                  (kn - _front(ds) * 0.004, _front(ds)), side], (kn, an, 0.12)))
        out_calf = (f"Calf.{t}", [(kn + ds * (0.22 * ls), -ds), (kn + ds * (0.66 * ls), ds),
                                  (kn + _front(ds) * 0.012, -_front(ds)), side], (kn, an, 0.12))
        out.append(out_calf)
    return out


# Plate thickness (metres off the undersuit) per plate; the edge sits a little lower so the plate
# reads as a chamfered shell, and the lip rolls under to the suit.
PLATE_THICKNESS = {"Chest": 0.013, "Back": 0.012, "Lumbar": 0.008, "Pelvis": 0.008, "Gorget": 0.010,
                   "Abs0": 0.010, "Abs1": 0.010, "Abs2": 0.009, "Hip": 0.010, "Shoulder": 0.018,
                   "UpperArm": 0.009, "LowerArm": 0.011, "Hand": 0.0045, "Thigh": 0.011, "Knee": 0.015,
                   "Shin": 0.012, "Calf": 0.009}


def plate_thickness(name):
    return PLATE_THICKNESS.get(name.split(".")[0], 0.006)


def seg_dist(p, a, b):
    ab = b - a
    t = np.clip(((p - a) @ ab) / (ab @ ab), 0, 1)
    return np.linalg.norm(p - (a + t[:, None] * ab), axis=1)


def region_faces(c, planes, seg=None):
    m = np.ones(len(c), bool)
    for o, n in planes:
        m &= (c - o) @ n < 0
    if seg is not None:
        m &= seg_dist(c, seg[0], seg[1]) < seg[2]
    return np.where(m)[0]


def _largest_component(q: Quads, faces):
    """Keeps the largest edge-connected group of `faces` (planes can catch stray islands)."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    if len(faces) == 0:
        return faces
    f = q.f[faces]
    rows = np.repeat(np.arange(len(f)), 4)
    a = coo_matrix((np.ones(len(rows)), (rows, f.reshape(-1))), shape=(len(f), len(q.v))).tocsr()
    adj = a @ a.T
    _, lab = connected_components(adj, directed=False)
    big = np.bincount(lab).argmax()
    return faces[lab == big]


# ---- face display ------------------------------------------------------------------------


def _frame_at(head: Head, d):
    """Surface point, normal and tangent frame (right, up) on the head toward direction d."""
    p = head.surface(d[None])[0]
    eps = 0.01
    Xa = np.array([1.0, 0, 0])
    Za = np.array([0, 0, 1.0])
    px = head.surface((d + Xa * eps)[None])[0]
    pz = head.surface((d + Za * eps)[None])[0]
    n = _unit(np.cross(px - p, pz - p))
    if n @ (p - head.c) < 0:
        n = -n
    right = _unit(px - p)
    up = _unit(np.cross(n, right))
    return p, n, right, up


def glyph(head: Head, center_dir, width, height, offset, nu=8, nv=3, bend=0.0):
    """A small quad grid hugging the head surface (offset above it) centred toward `center_dir`.
    Returns (Quads, s, t) with s in [-1, 1] across and t in [-1, 1] up per vertex."""
    s = np.linspace(-1, 1, nu + 1)
    t = np.linspace(-1, 1, nv + 1)
    S, T = np.meshgrid(s, t, indexing="ij")
    S, T = S.reshape(-1), T.reshape(-1)
    p0, n0, r0, u0 = _frame_at(head, _unit(center_dir))
    # Pill outline: the ends round off (height shrinks toward s = +-1).
    hh = 0.5 * height * np.sqrt(np.clip(1 - np.clip((np.abs(S) - 0.78) / 0.22, 0, 1) ** 2, 0.05, 1))
    local = p0 + np.outer(S * 0.5 * width, r0) + np.outer(T * hh + bend * S * S, u0)
    dirs = local - head.c
    pos = head.surface(dirs)
    nrm = np.array([_frame_at(head, d)[1] for d in dirs])
    pos = pos + nrm * offset
    idx = np.arange(len(S)).reshape(nu + 1, nv + 1)
    f = np.stack([idx[:-1, :-1], idx[1:, :-1], idx[1:, 1:], idx[:-1, 1:]], -1).reshape(-1, 4)
    q = gear._orient_outward(Quads(pos, f, np.stack([(S + 1) / 2, (T + 1) / 2], 1), f.copy()), head.c)
    frame = (r0, u0, n0, 0.5 * width, hh)
    return q, S, T, frame


FACE_SHAPES = ("eyeBlink_L", "eyeBlink_R", "eyeWide_L", "eyeWide_R", "eyeSquint_L", "eyeSquint_R",
               "browDown_L", "browDown_R", "browInnerUp_L", "browInnerUp_R", "browOuterUp_L", "browOuterUp_R",
               "cheekSquint_L", "cheekSquint_R", "jawOpen", "mouthSmile_L", "mouthSmile_R", "mouthFrown_L",
               "mouthFrown_R", "mouthStretch_L", "mouthStretch_R", "mouthPucker", "mouthRollUpper",
               "mouthRollLower", "mouthUpperUp", "mouthLowerDown")


def eye_morphs(S, T, frame, side):
    """Blend-shape deltas of one eye glyph (side +1: the character's left)."""
    r, u, n, hw, hh = frame
    m = {}
    inner = -S * side  # +1 at the end toward the nose (the character's left eye's inner end is -X)
    v = T * hh
    sfx = "_L" if side > 0 else "_R"
    m["eyeBlink" + sfx] = np.outer(-v * 0.92 - hh * 0.15, u)
    m["eyeWide" + sfx] = np.outer(v * 0.55 + hh * 0.2, u)
    m["eyeSquint" + sfx] = np.outer(-v * 0.45 + hh * 0.25 * (T < 0), u)
    m["cheekSquint" + sfx] = np.outer(np.where(T < 0, hh * 0.6 * (1 - T * 0) * (-T), 0.0), u)
    m["browDown" + sfx] = np.outer(-np.clip(inner, 0, 1) * hh * 1.1 - 0.1 * hh, u)
    m["browInnerUp" + sfx] = np.outer(np.clip(inner, 0, 1) * hh * 1.0, u)
    m["browOuterUp" + sfx] = np.outer(np.clip(-inner, 0, 1) * hh * 1.0, u)
    return m


def mouth_morphs(S, T, frame):
    r, u, n, hw, hh = frame
    m = {}
    v = T * hh
    for sfx, side in (("_L", 1.0), ("_R", -1.0)):
        k = np.clip(S * side, 0, 1) ** 2
        m["mouthSmile" + sfx] = np.outer(k * hw * 0.32, u) + np.outer(k * hw * 0.04 * side, r)
        m["mouthFrown" + sfx] = np.outer(-k * hw * 0.28, u)
        m["mouthStretch" + sfx] = np.outer(np.clip(S * side, 0, 1) * hw * 0.25 * side, r)
    m["jawOpen"] = np.outer(np.where(T < 0, -hh * 5.0 * (-T), 0.0) * (1 - 0.6 * S * S), u)
    m["mouthPucker"] = np.outer(-S * hw * 0.62, r) + np.outer(v * 1.6, u)
    m["mouthRollUpper"] = np.outer(np.where(T > 0, -v * 0.7, 0.0), u)
    m["mouthRollLower"] = np.outer(np.where(T < 0, -v * 0.7, 0.0), u)
    m["mouthUpperUp"] = np.outer(np.where(T > 0, hh * 1.2 * T, 0.0), u)
    m["mouthLowerDown"] = np.outer(np.where(T < 0, hh * 1.8 * (-T), 0.0), u)
    return m


# ---- assembly ----------------------------------------------------------------------------


VISOR_OFFSET = 0.0032
PLATE_OFFSET = 0.0042


def assemble(body: fit.Body):
    style = STYLES[body.spec.key]
    J = joint_map(body)
    names = [j[0] for j in body.joints]
    obj = body.obj
    faces = obj.group_faces("body")
    q_all = Quads(body.v, np.array([[c[0] for c in obj.faces[i]] for i in faces]),
                  obj.vt.copy() * np.array([1.0, -1.0]) + np.array([0.0, 1.0]),
                  np.array([[c[1] for c in obj.faces[i]] for i in faces]))
    q, used = q_all.subset(np.arange(len(faces)))
    W = body.weights[used]

    # Idealise the body: smooth everything but the hands (fingers keep their shape), more over
    # the torso (no nipples, navel or ribs) and the feet (inside the boots anyway).
    v = q.v
    hand_d = np.min([np.linalg.norm(v - J[f"Hand.{t}"][0], axis=1) for t in ("L", "R")], axis=0)
    mask = smooth01((hand_d - 0.03) / 0.05)
    q = Quads(taubin(v, q.f, mask, iters=14), q.f, q.uv, q.fuv)

    # The jaw cut: a plane under the chin rising to the base of the skull.
    jaw_tip = J["Jaw"][1]
    back = J["Head"][0] + np.array([0, 0.10, -0.012])
    po = jaw_tip + np.array([0, 0.0, 0.004])
    tdir = _unit(back - po)
    pn = _unit(np.cross(np.cross(tdir, np.array([0, 0, -1.0])), tdir))
    if pn[2] > 0:
        pn = -pn
    plane = (po, pn)
    head = Head(J, body.v, np.array([[c[0] for c in obj.faces[i]] for i in faces]), plane)

    # Frame: the body below the head (the neck reaches up inside the head shell) and above the
    # boots (the feet are inside them).
    c = q.v[q.f].mean(1)
    ankle, boot_top = gear.boot_heights(J)
    keep = ((c - po) @ pn > 0.004) & (c[:, 2] > boot_top - 0.025)
    nrm_body = vertex_normals(q.v, triangulate(q.f))

    # Armour plates (plates.py): sculpted shells drawn as outlines on body charts; the backs of the
    # hands keep their plane-bounded plates.
    pieces = []  # (Quads, W, kind, lip label, name, tone, trim, edge distance)
    torso = plates.Torso(q.v)
    hidden = np.zeros(len(q.f), bool)
    for pl in plates.layout(J, torso, body.spec.key):
        sel = plates.select(q, pl, c)
        if len(sel) < 4:
            continue
        sq, src, lab, dist = plates.shell(q, nrm_body, sel, pl)
        pieces.append((sq, W[src], 0, lab, pl.name, pl.tone, pl.trim, dist))
        # Suit faces well inside a plate never show: drop them from the Frame.
        if not os.environ.get("VICTUS_KEEP_SUIT"):
            hidden[sel[pl.g(c[sel]) < -0.022]] = True
    for pname, planes, seg in plate_regions(J):
        if not pname.startswith("Hand."):
            continue
        sel = _largest_component(q, region_faces(c, planes, seg))
        if len(sel) < 4:
            continue
        th = plate_thickness(pname) * (0.8 if body.spec.key == "nova" else 1.0)
        sq, src, lab = gear.shell(q, nrm_body, sel, planes, th, th * 0.72, -0.0015)
        pieces.append((sq, W[src], 0, lab, pname, 1, 0, None))
    suit_n = nrm_body
    for t in ("L", "R"):
        bq, lab, near = gear.boot(J, t, q.v, suit_n)
        pieces.append((bq, W[near], 1, lab, f"Boot.{t}", 1, 1, None))
    # Back core: a power-cell puck on the graphite spine housing (glowing rings, the design
    # sheet's back view), and a small emblem puck on the sternum keel.
    zt = lambda t: J["Hips"][0][2] + (J["Neck"][0][2] - J["Hips"][0][2]) * t  # noqa: E731
    by_name = {p_[4]: p_ for p_ in pieces}
    nova = body.spec.key == "nova"

    def puck(host, z0, front, radius, depth, kind, pname, joint="Chest"):
        hv = by_name[host][0].v if host in by_name else q.v
        m = (np.abs(hv[:, 0]) < 0.02) & (np.abs(hv[:, 2] - z0) < 0.02)
        y0 = hv[m, 1].min() if front else hv[m, 1].max()
        nrm = np.array([0, -1.0 if front else 1.0, 0])
        dq = plates.disc(np.array([0.0, y0, z0]) + nrm * (depth * 0.5 - 0.002), nrm, radius, depth, n=8)
        dw = np.zeros((len(dq.v), len(names)))
        dw[:, names.index(joint)] = 1
        pieces.append((dq, dw, kind, np.zeros(len(dq.f), np.int32), pname, 1, 0, None))

    # The sheet's back view: a large reactor ring between the shoulder blades, set in a machined
    # housing collar; a hexagon-sized emblem on the breastplate.
    puck("Spine", zt(0.76), False, 0.092 if nova else 0.104, 0.016, 2, "CoreHousing")
    puck("Spine", zt(0.76), False, 0.074 if nova else 0.086, 0.030, 3, "Core")
    puck("Sternum" if nova else "Chest", zt(0.80), True, 0.018 if nova else 0.027, 0.012, 5, "Emblem")

    # Helmet: a ceramic shell over the head volume (cut lower than the head shell, so it covers
    # the jaw), inflated more at the crown and the back so it reads as a helmet, not a scalp.
    # Its front opening holds the visor: one wraparound pane of smoked glass from the brow to
    # below the mouth, its lower corners chamfered into a shield outline.
    jaw_z = J["Jaw"][1][2]
    hpo = po + np.array([0.0, 0.0, -0.024])
    hplane = (hpo, pn)
    hvol = Head(J, body.v, np.array([[c_[0] for c_ in obj.faces[i]] for i in faces]), hplane, blur=0.026)

    def puff(d):
        # A full helmet rather than a scalp: thicker over the crown and the back of the skull,
        # with a brow that overhangs the visor.
        up, back_, fwd = np.clip(d[:, 2], 0, 1), np.clip(d[:, 1], 0, 1), np.clip(-d[:, 1], 0, 1)
        brow = np.exp(-((d[:, 2] - 0.42) / 0.16) ** 2) * fwd
        return 0.0165 + 0.010 * up + 0.010 * back_ + 0.006 * brow

    helm = HullSurface(hvol, puff)
    hm = head_quads(helm, n=27)
    # Smooth the pane and shell: the head volume still carries the nose, lips and brow.
    hm = Quads(taubin(hm.v, hm.f, np.ones(len(hm.v)), iters=30, lam=0.6, mu=-0.62), hm.f, hm.uv, hm.fuv)
    hmc = hm.v[hm.f].mean(1)
    eye = head.eye
    vtop = eye[2] + 0.034
    vbot = jaw_z + 0.020
    vis_plate = plates.Plate("Visor", plates.Chart("sphere", a=head.c.copy(), radius=0.09), plates.visor_outline(body.spec.key),
                             thickness=0.0002, crown=0.004, edge=1.0, round=0.004, mirror=False, fair=0)
    vsel = plates.select(hm, vis_plate, hmc)
    below = (hmc - hpo) @ pn > -0.002
    hsel = _largest_component(hm, np.where(~below)[0])
    vsel = np.intersect1d(vsel, hsel)
    hn = vertex_normals(hm.v, triangulate(hm.f))
    helm_q, _, helm_lab = gear.shell(hm, hn, hsel, [hplane], 0.0, -0.0008, -0.0105)
    helm_W = np.zeros((len(helm_q.v), len(names)))
    helm_W[:, names.index("Head")] = 1.0
    pieces.append((helm_q, helm_W, 4, helm_lab, "Helmet", 0, 1, None))
    # Ear pods: machined discs on the helmet's sides, a light ring on each.
    for s_ in (1.0, -1.0):
        dpod = np.array([s_, 0.12, -0.12])
        p, n, _, _ = _frame_at(helm, dpod)
        pr = 0.027 if body.spec.key == "nova" else 0.032
        pq = plates.disc(p + n * 0.004, n, pr, 0.016, n=8)
        pw = np.zeros((len(pq.v), len(names)))
        pw[:, names.index("Head")] = 1
        pieces.append((pq, pw, 2, np.zeros(len(pq.f), np.int32), "Pod", 0, 0, None))

    # Visor: a pane of glass standing proud of the helmet front, its edge rolling down into it.
    gq = Quads(hm.v + hn * 0.0035, hm.f, hm.uv, hm.fuv)
    glass_s = RadialMesh(head.c, gq.v)
    vq, vsrc, vlab, _ = plates.shell(gq, hn, vsel, vis_plate, tuck=-0.0068)
    head_W_v = np.zeros((len(vq.v), len(names)))
    head_W_v[:, names.index("Head")] = 1.0

    # Face display glyphs floating just above the glass.
    glyphs = []
    eye_dirs = {1.0: J["Eye.L"][0] - head.c, -1.0: J["Eye.R"][0] - head.c}
    for side in (1.0, -1.0):
        dd = eye_dirs[side] + np.array([side * 0.006, 0, 0.002])
        # Orion: level bar eyes; Nova: soft arcs (the sheet's visor glyphs).
        gq_, S, T, fr = glyph(glass_s, dd, 0.030 if nova else 0.033, 0.0060 if nova else 0.0066, 0.0009, nu=12, nv=3,
                              bend=-0.0042 if nova else -0.0016)
        glyphs.append((gq_, eye_morphs(S, T, fr, side), 1))
    mouth_dir = np.array([0, -1.0, 0]) * 0.1 + np.array([0, 0, (eye[2] - 0.056) - head.c[2]])
    gq_, S, T, fr = glyph(glass_s, mouth_dir, 0.024 if nova else 0.028, 0.0032, 0.0009, nu=12, nv=2)
    glyphs.append((gq_, mouth_morphs(S, T, fr), 2))

    # ---- parts ----
    # The helmet encloses the head, so the Frame stops at the neck (no head shell under it).
    fq, fused = q.subset(np.where(keep & ~hidden)[0])
    frame_W = W[fused]
    frame_q, frame_W, frame_kind = _merge([(fq, frame_W, 0)])
    frame_q = repack_islands(frame_q, density=np.where(frame_kind == 1, 1.6, 1.0))
    frame_part = Part("Frame", "Frame", frame_q, frame_W, {}, attrs={"rest": frame_q.v.copy()},
                      face_attrs={"kind": frame_kind})

    aq_list = pieces
    armor_q, armor_W, armor_kind = _merge(aq_list)
    armor_lip = np.concatenate([p[3] for p in pieces])
    armor_trim = np.concatenate([np.full(len(p[0].f), p[6], np.int32) for p in pieces])
    armor_tone = np.concatenate([np.full(len(p[0].f), p[5], np.int32) for p in pieces])
    edge, centre = _piece_edges(pieces)
    armor_q = repack_islands(armor_q)
    armor_part = Part("Armor", "Armor", armor_q, armor_W, {},
                      attrs={"rest": armor_q.v.copy(), "edge": edge[:, None], "centre": centre},
                      face_attrs={"kind": armor_kind, "cuff": armor_lip, "trim": armor_trim, "tone": armor_tone})

    vis_pieces = [(vq, head_W_v, 0)] + [(g[0], np.tile(head_W_v[:1], (len(g[0].v), 1)), g[2]) for g in glyphs]
    visor_q, visor_W, visor_kind = _merge(vis_pieces)
    visor_q = repack_islands(visor_q, density=np.where(visor_kind > 0, 4.0, 1.0))
    nvis = len(vq.v)
    morphs = {}
    off = nvis
    for g in glyphs:
        for k, dv in g[1].items():
            morphs.setdefault(k, np.zeros((len(visor_q.v), 3)))[off:off + len(g[0].v)] += dv
        off += len(g[0].v)
    morphs = {k: morphs[k] for k in FACE_SHAPES if k in morphs}
    for d_ in morphs.values():  # sub-0.02 mm offsets are float noise: keep mirrored shapes mirrored
        d_[np.linalg.norm(d_, axis=1) < 2e-5] = 0.0
    glyph_v = np.zeros(len(visor_q.v))
    glyph_v[nvis:] = 1
    visor_lip = np.concatenate([vlab, np.zeros(len(visor_q.f) - len(vlab), np.int32)])
    visor_part = Part("Visor", "Visor", visor_q, visor_W, morphs,
                      attrs={"rest": visor_q.v.copy(), "glyph": glyph_v[:, None]},
                      face_attrs={"kind": visor_kind, "cuff": visor_lip})
    # Landmarks the materials and sockets use.
    info = {"head": head, "plane": plane, "visor": (vbot, vtop), "torso": torso, "visor_plate": vis_plate}
    return [frame_part, armor_part, visor_part], style, info


# Plates whose inset groove carries a light strip; 2 = the chest (the emblem as well).
TRIM_PLATES = {"Chest": 2, "Back": 1, "Helmet": 1, "Shoulder.L": 1, "Shoulder.R": 1, "Shin.L": 1, "Shin.R": 1, "LowerArm.L": 1, "LowerArm.R": 1,
               "Thigh.L": 1, "Thigh.R": 1, "Boot.L": 1, "Boot.R": 1}


def _trim_of(name):
    return TRIM_PLATES.get(name, 0)


def _piece_edges(pieces):
    """Per vertex of the merged armour: distance (metres) to its piece's open cut edge (the rolled
    lip), the bake's input for bevels and inset grooves; and the piece centroid."""
    edges, centres = [], []
    for q, _, _, lab, *_ in pieces:
        lipv = np.unique(q.f[np.asarray(lab) > 0]) if np.any(np.asarray(lab) > 0) else np.zeros(0, np.int64)
        if len(lipv):
            d, _ = cKDTree(q.v[lipv]).query(q.v)
        else:
            d = np.full(len(q.v), 1.0)
        edges.append(d)
        centres.append(np.tile(q.v.mean(0), (len(q.v), 1)))
    return np.concatenate(edges), np.concatenate(centres)


def _merge(pieces):
    vs, fs, uvs, fuvs, Ws, kinds = [], [], [], [], [], []
    nv = nu = 0
    for q, W, k, *_ in pieces:
        vs.append(q.v)
        fs.append(q.f + nv)
        uvs.append(q.uv)
        fuvs.append(q.fuv + nu)
        Ws.append(W)
        kinds.append(np.full(len(q.f), k, np.int32))
        nv += len(q.v)
        nu += len(q.uv)
    return (Quads(np.concatenate(vs), np.concatenate(fs), np.concatenate(uvs), np.concatenate(fuvs)),
            np.concatenate(Ws), np.concatenate(kinds))
