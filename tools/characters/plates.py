# SPDX-License-Identifier: Apache-2.0
"""Sculpted armour plates for the VICTUS Frame mannequins (mannequin.py, docs/CHARACTERS.md, ADR-0223).

A plate is an outline drawn on a 2D chart of the body and grown into a shell:

  chart    maps body points to 2D coordinates in metres: `front` / `back` (x, z seen from the
           front or the back, only points on that half of the torso), `side` (-y, z on one flank)
           and `limb` (arc length around a bone, distance along it; 0 degrees is the front of the
           limb, +90 its outer side, 180 the back);
  outline  a polygon in chart coordinates with rounded corners: its signed distance g (metres,
           negative inside) selects the covered body faces, snaps the shell's boundary onto the
           outline (clean straight or curved cut lines, chevrons, wedges, wraps) and measures how
           far inside each vertex lies;
  shell    every covered vertex is pushed out along the rest normal by a crowned profile (thin at
           the edge, full thickness `crown` metres in), so plates read as moulded, domed pieces
           instead of flat bands; the cut edge rolls back under itself to the suit. Shell
           vertices keep the skin weights of the body vertex they grew from, so plates deform
           with what they cover and cannot sink into it.

Plates carry a tone (0 glazed white ceramic, 1 graphite composite) and a trim flag (a light strip
in the inset groove) the bake reads per face.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy.spatial import cKDTree

from meshops import Quads


def _unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def smooth01(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


# ---- 2D outlines -------------------------------------------------------------------------


def polygon_sdf(p, poly):
    """Signed distance (negative inside) from 2D points p (n, 2) to a closed polygon (m, 2)."""
    poly = np.asarray(poly, float)
    a = poly
    b = np.roll(poly, -1, axis=0)
    ab = b - a  # (m, 2)
    ap = p[:, None, :] - a[None]  # (n, m, 2)
    t = np.clip((ap * ab[None]).sum(2) / np.maximum((ab * ab).sum(1), 1e-18)[None], 0, 1)
    d = np.linalg.norm(ap - t[..., None] * ab[None], axis=2).min(1)
    # Crossing number for the sign.
    y = p[:, 1][:, None]
    cond = (a[None, :, 1] > y) != (b[None, :, 1] > y)
    xint = a[None, :, 0] + (y - a[None, :, 1]) / np.where(np.abs(ab[None, :, 1]) > 1e-18, ab[None, :, 1], 1e-18) * ab[None, :, 0]
    inside = (cond & (p[:, 0][:, None] < xint)).sum(1) % 2 == 1
    return np.where(inside, -d, d)


# ---- charts ------------------------------------------------------------------------------


class Torso:
    """Front/back/side charts of the torso: the torso's centre line yc(z) splits front from back."""

    def __init__(self, body_v):
        m = np.abs(body_v[:, 0]) < 0.035
        z = body_v[m, 2]
        y = body_v[m, 1]
        bins = np.arange(0.70, 1.75, 0.01)
        yc = []
        for z0 in bins:
            s = np.abs(z - z0) < 0.012
            yc.append(0.5 * (y[s].min() + y[s].max()) if s.sum() > 3 else np.nan)
        yc = np.array(yc)
        ok = np.isfinite(yc)
        self.zb, self.yc = bins[ok], yc[ok]

    def centre(self, z):
        return np.interp(z, self.zb, self.yc)


@dataclass
class Chart:
    kind: str  # "front", "back", "side", "limb"
    side: float = 1.0  # +1 the character's left (+X)
    a: np.ndarray | None = None  # limb: bone start
    b: np.ndarray | None = None  # limb: bone end
    seam: float = -120.0  # limb: angle (degrees) where the unwrap is cut
    radius: float = 0.06  # limb: reference radius turning degrees into metres
    torso: Torso | None = None

    def frame(self):
        d = _unit(self.b - self.a)
        f = np.array([0, -1.0, 0])
        f = _unit(f - d * (f @ d))
        lat = _unit(np.cross(d, f))
        if lat[0] * self.side < 0:
            lat = -lat
        return d, f, lat

    def coords(self, P):
        """(n, 2) chart coordinates in metres, and a validity mask."""
        if self.kind in ("front", "back"):
            yc = self.torso.centre(P[:, 2])
            ok = (P[:, 1] < yc) if self.kind == "front" else (P[:, 1] > yc)
            return np.stack([P[:, 0], P[:, 2]], 1), ok
        if self.kind == "side":
            ok = P[:, 0] * self.side > 0.0
            return np.stack([-P[:, 1], P[:, 2]], 1), ok
        d, f, lat = self.frame()
        rel = P - self.a
        t = rel @ d
        radial = rel - np.outer(t, d)
        th = np.degrees(np.arctan2(radial @ lat, radial @ f))
        th = (th - self.seam) % 360.0 + self.seam
        return np.stack([np.radians(th) * self.radius, t], 1), np.ones(len(P), bool)

    def to_metric(self, poly):
        """Outline vertices in the plate's authoring units -> chart metres. Limb outlines are given
        as (degrees around, fraction of the bone length); torso outlines in metres."""
        poly = np.asarray(poly, float)
        if self.kind != "limb":
            return poly
        L = float(np.linalg.norm(self.b - self.a))
        return np.stack([np.radians(poly[:, 0]) * self.radius, poly[:, 1] * L], 1)


@dataclass
class Plate:
    name: str
    chart: Chart
    outline: list  # polygon (chart authoring units)
    thickness: float = 0.012  # metres off the suit at the crown
    tone: int = 0  # 0 white ceramic, 1 graphite composite
    trim: int = 0  # 1 light strip in the inset groove, 2 the chest emblem as well
    round: float = 0.010  # corner radius (metres)
    crown: float = 0.035  # distance from the edge to full thickness (metres)
    edge: float = 0.35  # edge thickness as a fraction of the crown thickness
    gate: tuple | None = None  # (a, b, radius): only body faces within radius of the segment
    mirror: bool = True  # outline drawn for the left side; the right is mirrored
    extra: dict = field(default_factory=dict)

    def g(self, P):
        uv, ok = self.chart.coords(P)
        poly = self.chart.to_metric(self.outline)
        d = polygon_sdf(uv, poly) - self.round
        d = np.where(ok, d, 1.0)
        if self.gate is not None:
            a, b, r = self.gate
            d = np.where(_seg_dist(P, a, b) < r, d, 1.0)
        return d


def _seg_dist(p, a, b):
    ab = b - a
    t = np.clip(((p - a) @ ab) / (ab @ ab), 0, 1)
    return np.linalg.norm(p - (a + t[:, None] * ab), axis=1)


def mirrored(plate: Plate) -> Plate:
    """The right-side twin of a left-side plate (outline mirrored in x for torso charts; limb
    charts are already side-relative)."""
    import copy

    p = copy.copy(plate)
    p.name = plate.name.replace(".L", ".R")
    c = copy.copy(plate.chart)
    c.side = -plate.chart.side
    M = np.array([-1.0, 1.0, 1.0])
    if c.kind in ("front", "back"):
        p.outline = [(-x, z) for x, z in plate.outline][::-1]
    if c.kind == "limb":  # the skeleton is symmetric in x
        c.a, c.b = c.a * M, c.b * M
    if plate.gate is not None:
        a, b, r = plate.gate
        p.gate = (a * M, b * M, r)
    p.chart = c
    return p


# ---- shells ------------------------------------------------------------------------------


def _largest_component(f, nv):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    if len(f) == 0:
        return np.zeros(0, bool)
    rows = np.repeat(np.arange(len(f)), 4)
    a = coo_matrix((np.ones(len(rows)), (rows, f.reshape(-1))), shape=(len(f), nv)).tocsr()
    _, lab = connected_components(a @ a.T, directed=False)
    return lab == np.bincount(lab).argmax()


def select(q: Quads, plate: Plate, centroids):
    g = plate.g(centroids)
    sel = np.where(g < 0)[0]
    if len(sel) < 4:
        return sel
    keep = _largest_component(q.f[sel], len(q.v))
    return sel[keep]


def snap(P, nrm, gfun, iters=4, eps=0.0008, max_step=0.02):
    """Moves points P along the surface (tangent to nrm) onto the zero level of gfun."""
    P = P.copy()
    for _ in range(iters):
        g = gfun(P)
        grad = np.zeros_like(P)
        for k in range(3):
            dP = np.zeros(3)
            dP[k] = eps
            grad[:, k] = (gfun(P + dP) - gfun(P - dP)) / (2 * eps)
        gt = grad - (grad * nrm).sum(1, keepdims=True) * nrm
        n2 = np.maximum((gt * gt).sum(1), 1e-6)
        step = -(g / n2)[:, None] * gt
        ln = np.linalg.norm(step, axis=1, keepdims=True)
        step *= np.minimum(1.0, max_step / np.maximum(ln, 1e-12))
        bad = np.abs(g) > 0.5  # outside the chart/gate: leave alone
        P = np.where(bad[:, None], P, P + step)
    return P


def shell(q: Quads, nrm, faces, plate: Plate, tuck=-0.0015, scale=1.0):
    """Crowned shell of q's `faces` bounded by plate's outline. Returns (Quads, src, lip label,
    per-vertex distance to the cut edge)."""
    f = q.f[faces]
    fuv = q.fuv[faces]
    e_all = np.concatenate([np.stack([f[:, k], f[:, (k + 1) % 4], fuv[:, k], fuv[:, (k + 1) % 4]], 1) for k in range(4)])
    key = np.sort(e_all[:, :2], 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    edges = e_all[cnt[inv.reshape(-1)] == 1]
    used = np.unique(f)
    remap = -np.ones(len(q.v), np.int64)
    remap[used] = np.arange(len(used))
    is_b = np.zeros(len(q.v), bool)
    is_b[edges[:, :2].reshape(-1)] = True
    base = q.v[used].copy()
    bmask = is_b[used]
    n_used = nrm[used]
    base[bmask] = snap(base[bmask], n_used[bmask], plate.g)
    # Relax the first interior ring halfway toward its neighbours so snapped boundary quads do
    # not collapse into slivers.
    ring = np.zeros(len(q.v), bool)
    for k in range(4):
        ring[f[:, k][is_b[f].any(1)]] = True
    ring &= ~is_b
    if ring.any():
        e = np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 3]], f[:, [3, 0]]])
        e = np.unique(np.sort(e, 1), axis=0)
        loc = remap[e]
        acc = np.zeros_like(base)
        deg = np.zeros(len(base))
        np.add.at(acc, loc[:, 0], base[loc[:, 1]])
        np.add.at(acc, loc[:, 1], base[loc[:, 0]])
        np.add.at(deg, loc[:, 0], 1)
        np.add.at(deg, loc[:, 1], 1)
        r = ring[used]
        avg = acc[r] / np.maximum(deg[r], 1)[:, None]
        moved = base[r] + 0.5 * (avg - base[r])
        # Stay on the surface: remove the normal component of the move.
        dm = moved - base[r]
        dm -= (dm * n_used[r]).sum(1, keepdims=True) * n_used[r]
        base[r] = base[r] + dm
    bpts = base[bmask]
    dist = cKDTree(bpts).query(base)[0] if len(bpts) else np.full(len(base), 1.0)
    dist[bmask] = 0.0
    th = plate.thickness * scale
    prof = plate.edge + (1 - plate.edge) * smooth01(dist / max(plate.crown, 1e-4))
    pos = base + n_used * (th * prof)[:, None]
    bverts = np.unique(edges[:, :2])
    low = -np.ones(len(q.v), np.int64)
    low[bverts] = len(pos) + np.arange(len(bverts))
    lip = base[remap[bverts]] + nrm[bverts] * tuck
    pos = np.concatenate([pos, lip])
    src = np.concatenate([used, bverts])
    dist = np.concatenate([dist, np.zeros(len(bverts))])
    f_main = remap[f]
    strip = np.stack([remap[edges[:, 1]], remap[edges[:, 0]], low[edges[:, 0]], low[edges[:, 1]]], 1)
    strip_uv = np.stack([edges[:, 3], edges[:, 2], edges[:, 2], edges[:, 3]], 1)
    f_all = np.concatenate([f_main, strip])
    fuv_all = np.concatenate([fuv, strip_uv])
    uu = np.unique(fuv_all)
    uremap = -np.ones(len(q.uv), np.int64)
    uremap[uu] = np.arange(len(uu))
    out = Quads(pos, f_all, q.uv[uu], uremap[fuv_all])
    label = np.concatenate([np.zeros(len(f_main), np.int32), np.ones(len(strip), np.int32)])
    return out, src, label, dist


def disc(center, normal, radius, depth, n=8, bevel=0.82):
    """Closed quad mesh of a puck (a cube mapped onto a cylinder, its caps inset to `bevel` of the
    radius so one subdivision step rounds the rim): ear pods, the back core, the chest emblem."""
    import gear

    q = gear.rounded_box(np.zeros(3), (1.0, 1.0, 1.0), 0.0, n=n)
    x, y, zz = q.v[:, 0], q.v[:, 1], q.v[:, 2]
    dx = x * np.sqrt(np.clip(1 - y * y / 2, 0, 1))
    dy = y * np.sqrt(np.clip(1 - x * x / 2, 0, 1))
    cap = np.abs(zz) > 0.999
    rim = np.maximum(np.abs(x), np.abs(y)) > 0.999
    sc = np.where(cap & ~rim, bevel, 1.0)
    sc = np.where(cap & rim, bevel + (1 - bevel) * 0.35, sc)
    local = np.stack([dx * radius * sc, dy * radius * sc, zz * depth * 0.5], 1)
    nz = _unit(normal)
    tx = _unit(np.cross([0, 0, 1.0], nz)) if abs(nz[2]) < 0.9 else _unit(np.cross([1.0, 0, 0], nz))
    ty = np.cross(nz, tx)
    R = np.stack([tx, ty, nz], 1)
    v = local @ R.T + np.asarray(center, float)
    out = Quads(v, q.f, q.uv, q.fuv)
    return gear._orient_outward(out, np.asarray(center, float))


# ---- layouts -----------------------------------------------------------------------------


def layout(J, torso: Torso, key):
    """The plate set of one character: Orion (Frame 01) or Nova (Frame 02), after the ORION & NOVA
    design sheet. Left-side plates are mirrored to the right."""
    nova = key == "nova"
    hips, neck = J["Hips"][0], J["Neck"][0]
    zh, zn = hips[2], neck[2]

    def z(t):  # torso height fraction: 0 at the hips joint, 1 at the base of the neck
        return zh + (zn - zh) * t

    F = Chart("front", torso=torso)
    B = Chart("back", torso=torso)
    S = Chart("side", torso=torso)
    torso_gate = (hips - np.array([0, 0, 0.14]), neck, 0.30)
    out = []

    def add(p, mirror=None):
        out.append(p)
        if p.mirror if mirror is None else mirror:
            out.append(mirrored(p))

    if not nova:
        # Chest: two domed pectoral plates either side of a graphite sternum keel.
        add(Plate("Pec.L", F, [(0.026, z(0.93)), (0.07, z(0.95)), (0.118, z(0.935)), (0.148, z(0.89)),
                               (0.156, z(0.77)), (0.145, z(0.665)), (0.11, z(0.605)), (0.06, z(0.59)), (0.026, z(0.62))],
                  thickness=0.017, crown=0.05, trim=1, round=0.012, gate=torso_gate))
        add(Plate("Sternum", F, [(-0.014, z(0.92)), (0.014, z(0.92)), (0.016, z(0.66)), (0.0, z(0.6)), (-0.016, z(0.66))],
                  thickness=0.014, crown=0.012, tone=1, trim=2, round=0.004, gate=torso_gate, mirror=False))
        # Abdomen: a six-panel graphite plastron, the top row the widest.
        rows = ((0.545, 0.455, 0.078), (0.44, 0.35, 0.072), (0.335, 0.245, 0.066))
        for i, (t1, t0, w) in enumerate(rows):
            add(Plate(f"Abs{i}.L", F, [(0.012, z(t1)), (w, z(t1 + 0.01)), (w - 0.006, z(t0)), (0.012, z(t0))],
                      thickness=0.011, crown=0.016, tone=1, round=0.006, gate=torso_gate))
        add(Plate("Oblique.L", F, [(0.094, z(0.555)), (0.148, z(0.60)), (0.152, z(0.38)), (0.118, z(0.25)), (0.088, z(0.27))],
                  thickness=0.009, crown=0.02, tone=1, round=0.006, gate=torso_gate))
        add(Plate("Belt", F, [(-0.16, z(0.205)), (0.16, z(0.205)), (0.16, z(0.13)), (-0.16, z(0.13))],
                  thickness=0.011, crown=0.012, tone=1, round=0.004, gate=torso_gate, mirror=False))
        add(Plate("Groin", F, [(-0.052, z(0.11)), (0.052, z(0.11)), (0.046, z(-0.03)), (0.0, z(-0.11)), (-0.046, z(-0.03))],
                  thickness=0.011, crown=0.02, tone=1, round=0.008, gate=(hips + np.array([0, 0, 0.1]), hips - np.array([0, 0, 0.16]), 0.2),
                  mirror=False))
        add(Plate("Hip.L", S, [(0.07, z(0.11)), (-0.045, z(0.11)), (-0.055, z(-0.06)), (-0.02, z(-0.15)), (0.05, z(-0.13)), (0.075, z(-0.02))],
                  thickness=0.013, crown=0.03, trim=1, round=0.010,
                  gate=(J["UpperLeg.L"][0] + np.array([0.03, 0, 0.1]), J["UpperLeg.L"][0] - np.array([-0.03, 0, 0.12]), 0.12)))
        # Back: shoulder-blade plates around a graphite spine housing for the power core.
        add(Plate("Blade.L", B, [(0.095, z(0.94)), (0.135, z(0.925)), (0.155, z(0.85)), (0.155, z(0.70)), (0.125, z(0.60)),
                                 (0.09, z(0.63)), (0.094, z(0.74))],
                  thickness=0.015, crown=0.04, trim=1, round=0.012, gate=torso_gate))
        add(Plate("Spine", B, [(-0.07, z(0.94)), (0.07, z(0.94)), (0.082, z(0.82)), (0.074, z(0.66)), (0.03, z(0.56)),
                               (-0.03, z(0.56)), (-0.074, z(0.66)), (-0.082, z(0.82))],
                  thickness=0.014, crown=0.025, tone=1, round=0.01, gate=torso_gate, mirror=False))
        for i, (t1, t0) in enumerate(((0.52, 0.43), (0.41, 0.32))):
            add(Plate(f"Lumbar{i}.L", B, [(0.01, z(t1)), (0.07, z(t1)), (0.064, z(t0)), (0.01, z(t0))],
                      thickness=0.010, crown=0.014, tone=1, round=0.006, gate=torso_gate))
        add(Plate("BeltBack", B, [(-0.16, z(0.205)), (0.16, z(0.205)), (0.16, z(0.13)), (-0.16, z(0.13))],
                  thickness=0.011, crown=0.012, tone=1, round=0.004, gate=torso_gate, mirror=False))
    else:
        # Chest: one sculpted bust plate split by a lit centre seam.
        add(Plate("Bust.L", F, [(0.006, z(0.92)), (0.06, z(0.95)), (0.11, z(0.93)), (0.14, z(0.86)), (0.148, z(0.72)),
                                (0.14, z(0.62)), (0.09, z(0.575)), (0.035, z(0.59)), (0.006, z(0.64))],
                  thickness=0.012, crown=0.04, trim=1, round=0.012, gate=torso_gate))
        add(Plate("Sternum", F, [(-0.004, z(0.90)), (0.004, z(0.90)), (0.004, z(0.66)), (-0.004, z(0.66))],
                  thickness=0.006, crown=0.004, tone=1, trim=2, round=0.002, gate=torso_gate, mirror=False))
        # Corset: graphite panels over a narrow waist, angled like ribs.
        for i, (t1, t0, w) in enumerate(((0.52, 0.43, 0.085), (0.41, 0.32, 0.075), (0.30, 0.215, 0.07))):
            add(Plate(f"Corset{i}.L", F, [(0.008, z(t1)), (w, z(t1 + 0.03)), (w + 0.01, z(t0 + 0.03)), (0.008, z(t0))],
                      thickness=0.007, crown=0.012, tone=1, round=0.006, gate=torso_gate))
        add(Plate("Hip.L", S, [(0.075, z(0.16)), (-0.03, z(0.16)), (-0.055, z(0.02)), (-0.045, z(-0.16)), (0.02, z(-0.22)),
                               (0.08, z(-0.12)), (0.095, z(0.02))],
                  thickness=0.010, crown=0.03, trim=1, round=0.012,
                  gate=(J["UpperLeg.L"][0] + np.array([0.03, 0, 0.12]), J["UpperLeg.L"][0] - np.array([-0.03, 0, 0.14]), 0.13)))
        add(Plate("Blade.L", B, [(0.08, z(0.94)), (0.125, z(0.92)), (0.142, z(0.82)), (0.14, z(0.68)), (0.115, z(0.60)),
                                 (0.078, z(0.64))],
                  thickness=0.011, crown=0.035, trim=1, round=0.012, gate=torso_gate))
        add(Plate("Spine", B, [(-0.062, z(0.93)), (0.062, z(0.93)), (0.072, z(0.82)), (0.064, z(0.68)), (0.025, z(0.6)),
                               (-0.025, z(0.6)), (-0.064, z(0.68)), (-0.072, z(0.82))],
                  thickness=0.011, crown=0.02, tone=1, round=0.01, gate=torso_gate, mirror=False))
        for i, (t1, t0) in enumerate(((0.54, 0.45), (0.43, 0.34))):
            add(Plate(f"Lumbar{i}.L", B, [(0.008, z(t1)), (0.06, z(t1)), (0.055, z(t0)), (0.008, z(t0))],
                      thickness=0.007, crown=0.012, tone=1, round=0.006, gate=torso_gate))

    for t in ("L",):
        sh, el, wr = J[f"UpperArm.{t}"][0], J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0]
        hp, kn, an = J[f"UpperLeg.{t}"][0], J[f"LowerLeg.{t}"][0], J[f"Foot.{t}"][0]
        ua = Chart("limb", 1.0, sh, el, seam=-150, radius=0.055)
        fa = Chart("limb", 1.0, el, wr, seam=-150, radius=0.045)
        th = Chart("limb", 1.0, hp, kn, seam=-120, radius=0.08)
        sn = Chart("limb", 1.0, kn, an, seam=-120, radius=0.06)
        da = _unit(el - sh)
        k = 0.85 if nova else 1.0
        arm_gate = (sh - da * 0.12, el, 0.15)
        # Shoulders: a domed pauldron over the deltoid and a second lame below it.
        add(Plate("Pauldron.L", ua, [(-70, -0.34), (40, -0.42), (140, -0.40), (235, -0.30), (240, 0.30), (120, 0.40),
                                    (20, 0.36), (-60, 0.28)],
                  thickness=0.024 * k, crown=0.05, trim=1, round=0.014, gate=arm_gate))
        add(Plate("Lame.L", ua, [(-45, 0.40), (40, 0.46), (130, 0.50), (215, 0.44), (210, 0.58), (120, 0.62),
                                (30, 0.58), (-40, 0.54)],
                  thickness=0.015 * k, crown=0.02, round=0.008, gate=arm_gate, tone=1 if not nova else 0))
        add(Plate("Bicep.L", ua, [(-40, 0.66), (150, 0.66), (150, 0.86), (-40, 0.86)],
                  thickness=0.008, crown=0.015, tone=1, round=0.008, gate=arm_gate))
        add(Plate("Elbow.L", fa, [(120, -0.08), (240, -0.08), (250, 0.10), (180, 0.14), (110, 0.10)],
                  thickness=0.013, crown=0.02, tone=1, round=0.008, gate=(el - da * 0.08, el + _unit(wr - el) * 0.08, 0.09)))
        # Forearm: a gauntlet wrapping all but the inner wrist, its top cut on a slant.
        add(Plate("Gauntlet.L", fa, [(-110, 0.36), (-20, 0.20), (90, 0.12), (200, 0.18), (250, 0.36), (250, 0.90),
                                    (-110, 0.90)],
                  thickness=0.015 * k, crown=0.035, trim=1, round=0.010, gate=(el, wr, 0.09)))
        # Thighs: the front-outer plate with a slanted hip cut, a rear plate.
        thick_gate = (hp + _unit(kn - hp) * 0.02, kn, 0.14)
        if not nova:
            add(Plate("Thigh.L", th, [(-55, 0.32), (-25, 0.16), (60, 0.08), (140, 0.12), (160, 0.32), (160, 0.80), (70, 0.88),
                                     (-30, 0.86), (-55, 0.78)],
                      thickness=0.017, crown=0.05, trim=1, round=0.012, gate=thick_gate))
            add(Plate("Hamstring.L", th, [(178, 0.28), (238, 0.34), (238, 0.80), (178, 0.76)],
                      thickness=0.010, crown=0.02, tone=1, round=0.008, gate=thick_gate))
        else:
            add(Plate("Thigh.L", th, [(-70, 0.30), (-30, 0.12), (60, 0.02), (150, 0.06), (195, 0.30), (195, 0.84),
                                     (70, 0.90), (-40, 0.88), (-70, 0.80)],
                      thickness=0.012, crown=0.045, trim=1, round=0.014, gate=thick_gate))
            add(Plate("Hamstring.L", th, [(205, 0.34), (238, 0.36), (238, 0.82), (205, 0.84)],
                      thickness=0.008, crown=0.015, tone=0, round=0.006, gate=thick_gate))
        # Knee: a faceted graphite cop.
        ks = Chart("limb", 1.0, kn, an, seam=-120, radius=0.06)
        add(Plate("Knee.L", ks, [(0, -0.13), (58, -0.085), (62, 0.05), (0, 0.11), (-62, 0.05), (-58, -0.085)],
                  thickness=0.020 * k, crown=0.025, tone=1, round=0.008, gate=(kn - _unit(an - kn) * 0.09, kn + _unit(an - kn) * 0.08, 0.12)))
        # Shin guard (a wedge top under the knee) and calf plate.
        add(Plate("Shin.L", sn, [(0, 0.135), (75, 0.17), (125, 0.26), (125, 0.80), (-75, 0.80), (-75, 0.26), (-55, 0.17)],
                  thickness=0.016 * k, crown=0.04, trim=1, round=0.010, gate=(kn, an, 0.12)))
        add(Plate("Calf.L", sn, [(140, 0.20), (238, 0.22), (238, 0.62), (140, 0.66)],
                  thickness=0.012 * k, crown=0.03, tone=0 if nova else 1, round=0.010, gate=(kn, an, 0.12)))
    return out
