# SPDX-License-Identifier: Apache-2.0
"""Character assembly: turns a fitted body (fit.py) into the renderable parts of a VICTUS
character (docs/CHARACTERS.md, ADR-0179).

Parts (one glTF primitive per material):
  Skin   head, neck above the collar and the hands, from the MakeHuman body topology;
  Suit   the rest of the body surface pushed out by a few millimetres into a fitted suit, with a
         rolled cuff strip at the collar and wrists (the skin underneath is not kept, so nothing can
         poke through);
  Eye    the MakeHuman high-poly eye proxy (sclera, iris and cornea surface);
  Mouth  upper and lower teeth and the tongue (lower teeth and tongue follow the Jaw joint);
  Groom  scalp hair shell, eyebrow shells and eyelash cards, alpha-tested strands;
  Gear   boots, gloves, belt and backpack (gear.py), exported as one mesh node per item.

Every part is a quad mesh with face-varying UVs plus per-vertex skin weights, blend-shape deltas
and bake attributes (hair flow, card coordinates, region masks), so one Catmull-Clark step
(meshops.catmull_clark) subdivides all of them consistently for the highest LOD.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

import fit
import gear
import mh
from meshops import Quads, repack_islands, vertex_normals, triangulate


@dataclass
class Part:
    name: str
    material: str
    q: Quads
    W: np.ndarray  # (n, J) skin weights
    morphs: dict  # name -> (n, 3)
    attrs: dict = field(default_factory=dict)  # name -> (n, k) bake attributes
    face_attrs: dict = field(default_factory=dict)  # name -> (m,) per-face labels


@dataclass
class Style:
    """Per-character look parameters (colours are linear RGB)."""
    skin: tuple
    lips: tuple
    iris: tuple
    hair: tuple
    suit: tuple
    trim: tuple
    glow: tuple
    hair_style: str  # "crop" | "bun"
    stubble: float
    freckles: float
    brow_thickness: float
    gear: tuple = (0.03, 0.03, 0.032)  # boots, gloves and pack shell
    webbing: tuple = (0.05, 0.05, 0.05)  # belt and pack straps


STYLES = {
    "orion": Style(skin=(0.235, 0.120, 0.074), lips=(0.20, 0.085, 0.07), iris=(0.10, 0.052, 0.022),
                   hair=(0.012, 0.009, 0.008), suit=(0.040, 0.048, 0.060), trim=(0.32, 0.34, 0.36),
                   glow=(0.30, 0.85, 1.0), hair_style="crop", stubble=0.8, freckles=0.0, brow_thickness=1.15,
                   gear=(0.022, 0.022, 0.024), webbing=(0.055, 0.060, 0.050)),
    "nova": Style(skin=(0.56, 0.37, 0.27), lips=(0.48, 0.20, 0.18), iris=(0.13, 0.075, 0.035),
                  hair=(0.018, 0.012, 0.010), suit=(0.50, 0.47, 0.42), trim=(0.06, 0.06, 0.07),
                  glow=(1.0, 0.55, 0.22), hair_style="bun", stubble=0.0, freckles=0.35, brow_thickness=0.85,
                  gear=(0.11, 0.072, 0.045), webbing=(0.035, 0.034, 0.036)),
}


def obj_quads(obj: mh.ObjMesh, faces, positions):
    f = np.array([[c[0] for c in obj.faces[i]] for i in faces])
    fuv = np.array([[c[1] for c in obj.faces[i]] for i in faces])
    uv = obj.vt.copy()
    uv[:, 1] = 1.0 - uv[:, 1]  # glTF UV origin is the top-left
    return Quads(positions, f, uv, fuv)


def joint_map(body: fit.Body):
    return {n: (np.asarray(h), np.asarray(t)) for n, _, h, t in body.joints}


# ---- regions --------------------------------------------------------------------------------


def head_frame(body):
    J = joint_map(body)
    eL, eR = J["Eye.L"][0], J["Eye.R"][0]
    eye_mid = 0.5 * (eL + eR)
    return J, eL, eR, eye_mid


def collar_height(J):
    neck0, head0 = J["Neck"][0], J["Head"][0]
    return neck0[2] + 0.35 * (head0[2] - neck0[2])


def cut_planes(J):
    """Suit edge planes (point, normal toward the bare skin): the collar and both wrist cuffs."""
    planes = [("collar", np.array([0, 0, collar_height(J)]), np.array([0, 0, 1.0]))]
    for t in ("L", "R"):
        wr, el = J[f"Hand.{t}"][0], J[f"LowerArm.{t}"][0]
        d = (wr - el) / np.linalg.norm(wr - el)
        planes.append((f"cuff.{t}", wr - d * 0.012, d))
    return planes


def plane_distance(p, J):
    """Per point: signed distance past the nearest relevant cut plane (> 0 on the bare side) and
    the plane index; points far from the hands only test the collar."""
    planes = cut_planes(J)
    best = np.full(len(p), -1e9)
    which = np.zeros(len(p), np.int64)
    for i, (_, o, n) in enumerate(planes):
        s = (p - o) @ n
        if i > 0:
            near = np.linalg.norm(p - o, axis=1) < 0.25
            s = np.where(near, s, -1e9)
        upd = s > best
        best = np.where(upd, s, best)
        which = np.where(upd, i, which)
    return best, which, planes


SKIN_OVERLAP = 0.02  # bare skin continues this far under the suit edge


SKULL_AXIS_BEHIND_EYES = 0.085

# Hairline height above eye level (metres) by azimuth from the front (degrees, 0 = forehead).
HAIRLINE = {
    "crop": ((0, 0.064), (20, 0.062), (35, 0.054), (48, 0.042), (60, 0.034), (68, 0.012), (76, 0.002), (83, 0.022),
             (95, 0.026), (108, 0.004), (125, -0.030), (150, -0.056), (180, -0.066)),
    "bun": ((0, 0.069), (20, 0.067), (35, 0.059), (48, 0.046), (60, 0.036), (68, 0.020), (76, 0.014), (83, 0.024),
            (95, 0.026), (108, 0.006), (125, -0.026), (150, -0.052), (180, -0.060)),
}


def hair_margin(p, eye_mid, style):
    """Signed height of points above the hairline (metres); > 0 is under the hair. The azimuth is
    taken about the skull's vertical axis (~9 cm behind the eyes), heights from eye level."""
    d = p - eye_mid
    az = np.degrees(np.abs(np.arctan2(d[:, 0], -(d[:, 1] - SKULL_AXIS_BEHIND_EYES))))
    tab = np.array(HAIRLINE[style])
    h = np.interp(az, tab[:, 0], tab[:, 1])
    return d[:, 2] - h, az


def brow_density(p, eL, eR, thickness):
    """0..1 eyebrow coverage on the face surface around each brow arch."""
    out = np.zeros(len(p))
    for e, side in ((eL, 1.0), (eR, -1.0)):
        x = (p[:, 0] - e[0]) * side  # lateral offset from the eye centre, + outward
        s = np.clip((x + 0.018) / 0.046, 0, 1)  # 0 at the inner head, 1 at the tail
        centre_z = e[2] + 0.0215 + 0.006 * np.sin(np.pi * np.clip(s * 1.15, 0, 1)) - 0.004 * s
        half = (0.0052 * (1 - s) + 0.0022 * s) * thickness
        dz = np.abs(p[:, 2] - centre_z)
        along = np.clip(1 - np.maximum(-(x + 0.018), x - 0.028) / 0.004, 0, 1)
        dens = np.clip(1 - (dz - half) / 0.0018, 0, 1) * along
        front = p[:, 1] < e[1] + 0.012
        out = np.maximum(out, dens * front)
    return out


def tangent_project(v, n):
    t = v - n * np.sum(v * n, 1, keepdims=True)
    return t / np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-9)


# ---- part builders -----------------------------------------------------------------------


def body_parts(body: fit.Body, style: Style):
    obj = body.obj
    faces = obj.group_faces("body")
    q_all = obj_quads(obj, faces, body.v)
    q, used = q_all.subset(np.arange(len(faces)))
    W = body.weights[used]
    morphs = {k: d[used] for k, d in body.morphs.items()}
    J, eL, eR, eye_mid = head_frame(body)
    c = q.v[q.f].mean(1)
    dist_f, _, planes = plane_distance(c, J)
    skin = dist_f > -SKIN_OVERLAP
    suit = dist_f < 0.0

    # Skin part: head, neck and hands, continuing under the suit edges.
    skin_q, s_used = q.subset(np.where(skin)[0])
    cs = skin_q.v[skin_q.f].mean(1)
    is_head = cs[:, 2] > collar_height(J) - SKIN_OVERLAP - 0.01
    skin_q = repack_islands(skin_q, density=np.where(is_head, 2.3, 1.0))
    skin_part = Part("Skin", "Skin", skin_q, W[s_used], {k: d[s_used] for k, d in morphs.items()},
                     attrs={"rest": skin_q.v.copy()})

    # Suit part: the covered faces pushed out, the edge snapped onto the cut plane and closed by a
    # rolled lip that tucks back to the skin.
    nrm = vertex_normals(q.v, triangulate(q.f))
    suit_faces = np.where(suit)[0]
    sf = q.f[suit_faces]
    sfuv = q.fuv[suit_faces]
    # Boundary edges of the suit region (edges used once within the suit faces).
    e_all = np.concatenate([np.stack([sf[:, k], sf[:, (k + 1) % 4], sfuv[:, k], sfuv[:, (k + 1) % 4]], 1) for k in range(4)])
    key = np.sort(e_all[:, :2], 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    edges = e_all[cnt[inv.reshape(-1)] == 1]
    used_v = np.unique(sf)
    remap = -np.ones(len(q.v), np.int64)
    remap[used_v] = np.arange(len(used_v))
    is_b = np.zeros(len(q.v), bool)
    is_b[edges[:, :2].reshape(-1)] = True
    base = q.v[used_v].copy()
    # Snap edge vertices onto their cut plane.
    dist_v, which_v, _ = plane_distance(base, J)
    bmask = is_b[used_v]
    for i, (_, o, n) in enumerate(planes):
        m = bmask & (which_v == i)
        base[m] -= ((base[m] - o) @ n)[:, None] * n
    off = np.where(bmask, 0.0042, 0.0026)
    pos = base + nrm[used_v] * off[:, None]
    src = used_v.copy()
    bverts = np.unique(edges[:, :2])
    low = -np.ones(len(q.v), np.int64)
    low[bverts] = len(pos) + np.arange(len(bverts))
    lip = base[remap[bverts]] - nrm[bverts] * 0.0006
    pos = np.concatenate([pos, lip])
    src = np.concatenate([src, bverts])
    f_suit = remap[sf]
    strip = np.stack([remap[edges[:, 1]], remap[edges[:, 0]], low[edges[:, 0]], low[edges[:, 1]]], 1)
    strip_uv = np.stack([edges[:, 3], edges[:, 2], edges[:, 2], edges[:, 3]], 1)
    f_all = np.concatenate([f_suit, strip])
    fuv_all = np.concatenate([sfuv, strip_uv])
    uu = np.unique(fuv_all)
    uremap = -np.ones(len(q.uv), np.int64)
    uremap[uu] = np.arange(len(uu))
    suit_q = Quads(pos, f_all, q.uv[uu], uremap[fuv_all])
    suit_q = repack_islands(suit_q)
    label = np.concatenate([np.zeros(len(f_suit), np.int32), np.ones(len(strip), np.int32)])
    suit_part = Part("Suit", "Suit", suit_q, W[src], {k: d[src] for k, d in morphs.items()},
                     attrs={"rest": pos.copy(), "nrm0": nrm[src]}, face_attrs={"cuff": label})
    return skin_part, suit_part, q, nrm, used


def eye_part(body: fit.Body):
    e = body.eyes
    f = np.array([[c[0] for c in fc] for fc in e.faces])
    fuv = np.array([[c[1] for c in fc] for fc in e.faces])
    uv = e.vt.copy()
    uv[:, 1] = 1 - uv[:, 1]
    q = Quads(body.eyes_v.copy(), f, uv, fuv)
    q = repack_islands(q)
    morphs = {k: d for k, d in body.eye_morphs.items() if np.abs(d).max() > 1e-4}
    J, eL, eR, _ = head_frame(body)
    # Per-vertex eye-local coordinates: angle from the gaze axis (-Y) about each eye's centre.
    centre = np.where((q.v[:, 0] > 0)[:, None], eL, eR)
    d = q.v - centre
    r = np.linalg.norm(d, axis=1)
    theta = np.degrees(np.arccos(np.clip(-d[:, 1] / np.maximum(r, 1e-9), -1, 1)))
    phi = np.arctan2(d[:, 2], d[:, 0])
    return Part("Eye", "Eye", q, body.eye_weights.copy(), morphs,
                attrs={"theta": theta[:, None], "phi": phi[:, None], "r": r[:, None], "rest": q.v.copy()})


def mouth_part(body: fit.Body):
    obj = body.obj
    groups = ("helper-upper-teeth", "helper-lower-teeth", "helper-tongue")
    faces = obj.group_faces(*groups)
    lab = np.concatenate([np.full(len(obj.group_faces(g)), i) for i, g in enumerate(groups)])
    q_all = obj_quads(obj, faces, body.v)
    q, used = q_all.subset(np.arange(len(faces)))
    q = _planar_uv(q)
    q = repack_islands(q)
    vlab = np.zeros(len(q.v))
    for i in range(3):
        vlab[np.unique(q.f[lab == i])] = i
    W = body.weights[used]
    # Lower teeth and tongue ride the Jaw joint (the reference weights split them across face bones).
    names = [n for n, _ in fit.skeleton_layout()]
    jaw, head = names.index("Jaw"), names.index("Head")
    W = np.zeros_like(W)
    W[vlab == 0, head] = 1
    W[vlab > 0, jaw] = 1
    z = q.v[:, 2]
    return Part("Mouth", "Mouth", q, W, {k: d[used] for k, d in body.morphs.items()},
                attrs={"label": vlab[:, None], "rest": q.v.copy(), "z": z[:, None]})


def _planar_uv(q: Quads):
    """Fresh per-face-island UVs (the helpers' reference UVs overlap): each face is its own chart,
    then islands are merged by shared vertices with a front/top projection."""
    n = vertex_normals(q.v, triangulate(q.f))
    fn = n[q.f].mean(1)
    axis = np.argmax(np.abs(fn), 1)  # 0: x, 1: y (front), 2: z (top)
    uv, fuv = [], np.zeros_like(q.f)
    for ax in range(3):
        sel = np.where(axis == ax)[0]
        if not len(sel):
            continue
        keep = [i for i in range(3) if i != ax]
        verts, inv = np.unique(q.f[sel], return_inverse=True)
        base = sum(len(u) for u in uv)
        uv.append(q.v[verts][:, keep] * 20.0)
        fuv[sel] = base + inv.reshape(-1, 4)
    return Quads(q.v, q.f, np.concatenate(uv), fuv)


def groom_parts(body: fit.Body, style: Style, skin_q_full: Quads, nrm_full, body_used):
    """Scalp hair shell, brow shells and eyelash cards."""
    J, eL, eR, eye_mid = head_frame(body)
    q = skin_q_full
    c = q.v[q.f].mean(1)
    names = [n for n, _ in fit.skeleton_layout()]
    del names
    parts = []

    # Hair shell over the scalp faces.
    marg_c, _ = hair_margin(q.v, eye_mid, style.hair_style)
    hair_faces = np.where((marg_c[q.f].max(1) > -0.003) & (c[:, 2] > collar_height(J)))[0]
    hq, used = q.subset(hair_faces)
    marg, az = hair_margin(hq.v, eye_mid, style.hair_style)
    n = nrm_full[used]
    if style.hair_style == "crop":
        top = 0.0055 - 0.0030 * np.clip((az - 60) / 60, 0, 1)  # fade toward the sides and back
        crown = eye_mid + np.array([0.0, 0.035, 0.105])
        flow = tangent_project(hq.v - crown + np.array([0, -0.02, 0.0]), n)
        flow = tangent_project(flow + np.array([0, 0, -0.35]) * (az[:, None] > 70), n)
    else:
        top = 0.0085 - 0.0020 * np.clip((az - 60) / 60, 0, 1)
        bun_c = eye_mid + np.array([0.0, 0.085, 0.06])
        flow = tangent_project(bun_c - hq.v, n)
    thick = top * _smooth(np.clip(marg / 0.022, 0, 1)) + 0.0004
    hpos = hq.v + n * thick[:, None]
    parts.append(("Hair", hq, hpos, body_used[used], {"flow": flow, "margin": marg[:, None], "kind": np.zeros((len(used), 1))}))
    if style.hair_style == "bun":
        parts.append(bun_piece(hq, hpos, body_used[used], eye_mid))

    # Brows.
    dens_f = brow_density(c, eL, eR, style.brow_thickness)
    brow_faces = np.where(dens_f > 0.01)[0]
    if len(brow_faces):
        # Grow by one ring so the strands fade out inside the shell.
        vs = np.unique(q.f[brow_faces])
        ring = np.where(np.isin(q.f, vs).any(1))[0]
        bq, bused = q.subset(ring)
        nb = nrm_full[bused]
        side = np.sign(bq.v[:, 0])
        e = np.where(side[:, None] > 0, eL, eR)
        x = (bq.v[:, 0] - e[:, 0]) * side
        s = np.clip((x + 0.018) / 0.046, 0, 1)
        # Inner brow hairs point up and out, the body outward, the tail outward and down.
        dirv = np.stack([side * (0.4 + 0.9 * s), np.zeros_like(s), 1.0 - 1.3 * s], 1)
        flow = tangent_project(dirv, nb)
        bpos = bq.v + nb * 0.0007
        parts.append(("Brow", bq, bpos, body_used[bused], {"flow": flow, "density": brow_density(bq.v, eL, eR, style.brow_thickness)[:, None],
                                                "kind": np.ones((len(bused), 1))}))
    return parts


def bun_piece(hq: Quads, hpos, hsrc, eye_mid, rings=18, segs=32):
    """A low bun: an ellipsoid of quads sunk into the back of the hair shell. Hair flows in circles
    around the bun's axis (pointing back), so the strands read as hair wound into a coil. The bun
    follows the Head joint through the nearest scalp vertex's skin weights."""
    # Back of the hair shell on the midline, slightly below the crown.
    zc = eye_mid[2] + 0.012
    near = np.abs(hpos[:, 0]) < 0.012
    near &= np.abs(hpos[:, 2] - zc) < 0.012
    back = hpos[near][np.argmax(hpos[near][:, 1])]
    radii = np.array([0.047, 0.031, 0.039])
    centre = back + np.array([0.0, 0.010, 0.0])
    th = np.linspace(0, np.pi, rings + 1)  # from the back pole (outward, +Y) to the front pole
    ph = np.linspace(0, 2 * np.pi, segs + 1)[:-1]
    T, P = np.meshgrid(th, ph, indexing="ij")
    dirs = np.stack([np.sin(T) * np.cos(P), np.cos(T), np.sin(T) * np.sin(P)], -1).reshape(-1, 3)
    # Coil ridges: three wraps of a slightly lumpy surface.
    lump = 1.0 + 0.06 * np.sin(3 * P.reshape(-1) + 5 * T.reshape(-1))
    v = centre + dirs * radii * lump[:, None]
    idx = np.arange((rings + 1) * segs).reshape(rings + 1, segs)
    f = []
    for i in range(rings):
        for j in range(segs):
            a, b = idx[i, j], idx[i, (j + 1) % segs]
            c, d = idx[i + 1, (j + 1) % segs], idx[i + 1, j]
            f.append([a, d, c, b])
    f = np.array(f)
    # Seamless-enough UVs: one island per quad ring strip, u around, v down the bun.
    uidx = np.arange((rings + 1) * (segs + 1)).reshape(rings + 1, segs + 1)
    uv = np.stack(np.meshgrid(np.linspace(0, 1, rings + 1), np.linspace(0, 1, segs + 1), indexing="ij"), -1).reshape(-1, 2)
    uv = uv[:, ::-1] * np.array([0.25, 0.08]) * 1.0
    fuv = np.array([[uidx[i, j], uidx[i + 1, j], uidx[i + 1, j + 1], uidx[i, j + 1]] for i in range(rings) for j in range(segs)])
    # Wind the flow around the bun's axis (+Y).
    nrm = dirs / np.maximum(np.linalg.norm(dirs, axis=1, keepdims=True), 1e-9)
    flow = tangent_project(np.cross(np.array([0.0, 1.0, 0.0]), v - centre) + 1e-4, nrm)
    from scipy.spatial import cKDTree
    _, nn = cKDTree(hpos).query(v)
    q = Quads(v, f, uv, fuv)
    attrs = {"flow": flow, "margin": np.full((len(v), 1), 0.05), "kind": np.zeros((len(v), 1))}
    return ("Hair", q, v, hsrc[nn], attrs)


def _smooth(t):
    return t * t * (3 - 2 * t)


def lash_part(body: fit.Body):
    obj = body.obj
    J, eL, eR, _ = head_frame(body)
    out = []
    for g, upper in (("helper-l-eyelashes-2", 1), ("helper-l-eyelashes-1", 0),
                     ("helper-r-eyelashes-2", 1), ("helper-r-eyelashes-1", 0)):
        faces = obj.group_faces(g)
        q_all = obj_quads(obj, faces, body.v)
        q, used = q_all.subset(np.arange(len(faces)))
        e = eL if g.startswith("helper-l") else eR
        x = q.v[:, 0]
        s = (x - x.min()) / max(x.max() - x.min(), 1e-9)
        if e[0] < 0:
            s = 1 - s
        d = np.linalg.norm(q.v - e, axis=1)
        t = np.zeros_like(s)
        for i in range(len(s)):
            m = np.abs(s - s[i]) < 0.06
            lo, hi = d[m].min(), d[m].max()
            t[i] = (d[i] - lo) / max(hi - lo, 1e-9)
        # Card UVs from the card coordinates (the helpers' reference UVs are a shared atlas).
        length = x.max() - x.min()
        uv = np.stack([s * length * 4.0, t * 0.25], 1)
        fuv = q.f.copy()
        q = Quads(q.v, q.f, uv, fuv)
        out.append(("Lash", q, q.v.copy(), used, {"s": s[:, None], "t": t[:, None], "upper": np.full((len(s), 1), upper),
                                                   "kind": np.full((len(s), 1), 2.0)}))
    return out


def merge_groom(body, pieces):
    """Merges groom pieces into one Groom part with a shared, repacked atlas."""
    pos, f, uv, fuv, W, src, attrs = [], [], [], [], [], [], {}
    keys = ("flow", "margin", "density", "s", "t", "upper", "kind")
    width = {"flow": 3}
    nv = nuv = 0
    density = []
    for name, q, p, used, a in pieces:
        pos.append(p)
        f.append(q.f + nv)
        fuv.append(q.fuv + nuv)
        uv.append(q.uv)
        src.append(used)
        dens = {"Hair": 1.0, "Brow": 2.5, "Lash": 6.0}[name]
        density.append(np.full(len(q.f), dens))
        for k in keys:
            attrs.setdefault(k, []).append(a.get(k, np.zeros((len(p), width.get(k, 1)))))
        nv += len(p)
        nuv += len(q.uv)
    q = Quads(np.concatenate(pos), np.concatenate(f), np.concatenate(uv), np.concatenate(fuv))
    q = repack_islands(q, density=np.concatenate(density), padding=0.004)
    src = np.concatenate(src)
    W = body.weights[src]
    morphs = {k: d[src] for k, d in body.morphs.items()}
    attrs = {k: np.concatenate(v) for k, v in attrs.items()}
    attrs["rest"] = q.v.copy()
    return Part("Groom", "Groom", q, W, morphs, attrs=attrs)


def assemble(body: fit.Body):
    style = STYLES[body.spec.key]
    skin, suit, body_q, body_n, body_used = body_parts(body, style)
    pieces = groom_parts(body, style, body_q, body_n, body_used) + lash_part(body)
    groom = merge_groom(body, pieces)
    names = [j[0] for j in body.joints]
    gq, gW, kind, lip = gear.build(fit_joints(body), names, skin.q, skin.W, suit.q, suit.W, None)
    gear_part = Part("Gear", "Gear", gq, gW, {}, attrs={"rest": gq.v.copy()},
                     face_attrs={"kind": kind, "cuff": lip})
    return [skin, suit, eye_part(body), mouth_part(body), groom, gear_part], style


def fit_joints(body):
    return joint_map(body)


del math
