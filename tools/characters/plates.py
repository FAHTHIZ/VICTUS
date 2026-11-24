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
        if self.kind == "sphere":  # about centre a: (azimuth from the front, elevation) x radius
            rel = P - self.a
            az = np.arctan2(rel[:, 0], -rel[:, 1])
            el = np.arctan2(rel[:, 2], np.hypot(rel[:, 0], rel[:, 1]))
            return np.stack([az * self.radius, el * self.radius], 1), rel[:, 1] < 0.04
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
        if self.kind == "sphere":  # outlines in degrees (azimuth, elevation)
            return np.radians(poly) * self.radius
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
    fair: int = 10  # smoothing passes over the shell's base: a manufactured surface, not anatomy
    facets: int = 0  # > 0: the outer surface is pulled onto this many planes (hard-surface panels)
    facet_mix: float = 0.8  # how far toward the planes (0 smooth moulding, 1 fully planar)
    lift: float = 0.0  # extra stand-off (metres) of the whole plate, edge included
    fit: int = 3  # > 0: the outer surface is refit as a polynomial of this degree over the chart
    extra: dict = field(default_factory=dict)  # "xmin": only points at least this far out from the midline

    def g(self, P):
        uv, ok = self.chart.coords(P)
        poly = self.chart.to_metric(self.outline)
        d = polygon_sdf(uv, poly) - self.round
        d = np.where(ok, d, 1.0)
        if "xmin" in self.extra:
            d = np.where(P[:, 0] * self.chart.side > self.extra["xmin"], d, 1.0)
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
    if plate.lift:
        prof = prof + plate.lift / max(th, 1e-6)
    pos = base + n_used * (th * prof)[:, None]
    if plate.fair and len(used) > 8:
        # Fair the outer surface (Taubin, boundary fixed) so the plate bridges the body's
        # anatomy like a moulded part, then keep at least the edge thickness of clearance.
        e = np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 3]], f[:, [3, 0]]])
        e = np.unique(np.sort(remap[e], 1), axis=0)
        deg = np.bincount(e.reshape(-1), minlength=len(pos)).astype(float)
        free = (~bmask)[:, None].astype(float)
        for it in range(plate.fair * 2):
            acc = np.zeros_like(pos)
            np.add.at(acc, e[:, 0], pos[e[:, 1]])
            np.add.at(acc, e[:, 1], pos[e[:, 0]])
            lap = acc / np.maximum(deg, 1)[:, None] - pos
            pos = pos + (0.5 if it % 2 == 0 else -0.53) * lap * free
        off = ((pos - base) * n_used).sum(1)
        need = th * prof
        pos = pos + n_used * np.maximum(need * 0.85 - off, 0.0)[:, None]
    if plate.fit and len(used) > 30:
        pos = surface_fit(pos, base, n_used, plate.chart, plate.fit, lo=th * prof * 0.7,
                          w=0.75 + 0.25 * smooth01(dist / max(plate.crown * 2.0, 1e-4)))
    if plate.facets and len(used) > 4 * plate.facets:
        pos = facet(pos, base, n_used, remap[f], plate.facets, plate.facet_mix,
                    lo=th * prof * 0.6, hi=th * 2.6 + plate.lift, w=smooth01(dist / max(plate.crown * 1.5, 1e-4)))
    bverts = np.unique(edges[:, :2])
    low = -np.ones(len(q.v), np.int64)
    low[bverts] = len(pos) + np.arange(len(bverts))
    lip = base[remap[bverts]] + nrm[bverts] * tuck
    pos = np.concatenate([pos, lip])
    src = np.concatenate([used, bverts])
    dist = np.concatenate([dist, np.zeros(len(bverts))])
    f_main = remap[f]
    strip = np.stack([remap[edges[:, 1]], remap[edges[:, 0]], low[edges[:, 0]], low[edges[:, 1]]], 1)
    # The lip strip gets its own UVs (an island of its own), so the rim between face and lip can
    # shade as a hard edge (see build_characters.Lod0).
    uu = np.unique(fuv)
    uremap = -np.ones(len(q.uv), np.int64)
    uremap[uu] = np.arange(len(uu))
    luv = np.unique(edges[:, 2:4])
    lremap = -np.ones(len(q.uv), np.int64)
    lremap[luv] = len(uu) + np.arange(len(luv))
    strip_uv = np.stack([lremap[edges[:, 3]], lremap[edges[:, 2]], lremap[edges[:, 2]], lremap[edges[:, 3]]], 1)
    # Lower corners: the rim curve scaled out about its centre, a thin ring of real UV width.
    cuv = q.uv[luv].mean(0)
    low_uv = cuv + (q.uv[luv] - cuv) * 1.06
    lremap2 = -np.ones(len(q.uv), np.int64)
    lremap2[luv] = len(uu) + len(luv) + np.arange(len(luv))
    strip_uv[:, 2] = lremap2[edges[:, 2]]
    strip_uv[:, 3] = lremap2[edges[:, 3]]
    f_all = np.concatenate([f_main, strip])
    out = Quads(pos, f_all, np.concatenate([q.uv[uu], q.uv[luv], low_uv]), np.concatenate([uremap[fuv], strip_uv]))
    label = np.concatenate([np.zeros(len(f_main), np.int32), np.ones(len(strip), np.int32)])
    return out, src, label, dist


def _height(chart, P):
    """The chart's 'height' coordinate of P and the unit direction it grows along."""
    if chart.kind in ("front", "back"):
        s = -1.0 if chart.kind == "front" else 1.0
        return P[:, 1] * s, np.tile([0, s, 0.0], (len(P), 1))
    if chart.kind == "side":
        return P[:, 0] * chart.side, np.tile([chart.side, 0, 0.0], (len(P), 1))
    rel = P - chart.a
    if chart.kind == "limb":
        d = _unit(chart.b - chart.a)
        rel = rel - np.outer(rel @ d, d)
    r = np.linalg.norm(rel, axis=1)
    return r, rel / np.maximum(r, 1e-9)[:, None]


def surface_fit(pos, base, nrm, chart, deg, lo, w, cap=0.012):
    """Replaces the plate's outer surface by a low-order polynomial height field over its chart
    (a moulded panel or a cylinder-section sleeve), so the body's anatomy - pectoral folds, the
    deltoid groove, quad heads - no longer telegraphs through the armour as lumps. Keeps at least
    `lo` of clearance off the suit; `w` fades the fit out toward the rolled cut edge."""
    uv, _ = chart.coords(pos)
    h, dirn = _height(chart, pos)
    c = uv.mean(0)
    sc = np.maximum(uv.std(0), 1e-4)
    x, y = ((uv - c) / sc).T
    A = np.stack([x ** i * y ** j for i in range(deg + 1) for j in range(deg + 1 - i)], 1)
    ww = np.maximum(w, 0.05)
    coef, *_ = np.linalg.lstsq(A * ww[:, None], h * ww, rcond=None)
    hf = A @ coef
    # Plates that wrap far round their chart are not height fields: cap how far the fit may move.
    tgt = pos + dirn * np.clip(hf - h, -cap, cap)[:, None]
    off = ((tgt - base) * nrm).sum(1)
    tgt = tgt + nrm * np.maximum(lo - off, 0.0)[:, None]
    return pos + (tgt - pos) * w[:, None]


def _kmeans_dirs(N, w, k, iters=24):
    """Deterministic spherical k-means of unit vectors N (weights w): farthest-point seeding from
    the weighted mean direction."""
    mean = (N * w[:, None]).sum(0)
    c = [N[np.argmax(N @ _unit(mean))]]
    for _ in range(1, k):
        sim = np.max(np.stack([N @ ci for ci in c], 1), 1)
        c.append(N[np.argmin(sim)])
    C = np.array(c)
    for _ in range(iters):
        lab = np.argmax(N @ C.T, 1)
        for i in range(k):
            m = lab == i
            if m.any():
                C[i] = _unit((N[m] * w[m, None]).sum(0) + 1e-9)
    return C, np.argmax(N @ C.T, 1)


def facet(pos, base, nrm, f, k, mix, lo, hi, w):
    """Pulls a plate's outer surface onto k planes: the outer normals are clustered, each cluster
    gets a plane through its outermost point, and every vertex moves outward along its (smooth)
    outer normal to where that ray leaves the intersection of the planes' half-spaces (a convex
    polyhedron enclosing the moulding). The planes meet in crisp ridges, so plates read as
    machined panels with flat faces and chamfered keels instead of a moulded skin. The offset off
    the suit (along the rest normal `nrm` from `base`) stays inside [lo, hi]; `w` (0..1 per
    vertex) keeps the rolled edge where it was."""
    from meshops import triangulate, vertex_normals

    N = vertex_normals(pos, triangulate(f))
    C, lab = _kmeans_dirs(N, np.maximum(w, 0.05), k)
    h = np.array([np.percentile(pos[lab == i] @ C[i], 99) if (lab == i).sum() > 3 else 1e9 for i in range(k)])
    dn = N @ C.T  # (n, k)
    num = h[None] - pos @ C.T  # >= 0 inside the polyhedron
    t = np.where(dn > 0.2, np.maximum(num, 0) / np.maximum(dn, 1e-6), np.inf).min(1)
    t = np.minimum(np.where(np.isfinite(t), t, 0.0), hi)
    tgt = pos + N * t[:, None]
    off = ((tgt - base) * nrm).sum(1)
    tgt = tgt + nrm * (np.clip(off, lo, hi) - off)[:, None]
    a = (mix * w)[:, None]
    return pos * (1 - a) + tgt * a


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


def visor_outline(key):
    """The visor's shield outline in degrees (azimuth from the front, elevation) about the head
    centre: from the brow, widest at the eye line where it wraps toward the ear pods, narrowing to
    the chin (the design sheet's face-shaped visor). Nova's is narrower and more oval."""
    k = 0.92 if key == "nova" else 1.0
    half = [(0, 27), (26, 26), (48, 20), (63, 9), (70 * k, -6), (68 * k, -22), (61 * k, -38), (50 * k, -53),
            (32 * k, -66), (0, -74)]
    right = [(-a, e) for a, e in half[1:-1]][::-1]
    return half + right


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
        # Manufactured parts: near-uniform thickness with a short bevel at the edge (the fairing
        # in `shell` gives the surface its moulded form).
        p.crown = min(p.crown, 0.018)
        p.edge = max(p.edge, 0.55)
        if p.facets:
            # Machined panels: a short chamfer to full thickness, so the cut edge reads as a crisp
            # wall rather than a pillow.
            p.crown = min(p.crown, 0.009)
            p.edge = max(p.edge, 0.78)
        if p.chart.kind == "limb":
            # Cut the unwrap in the middle of the outline's angular gap, never through the plate
            # (a cut through it makes the edge snapping jump across the seam).
            import copy

            a = np.array([pt[0] for pt in p.outline], float)
            gap = 360.0 - (a.max() - a.min())
            p.chart = copy.copy(p.chart)
            p.chart.seam = a.min() - max(gap, 0.0) * 0.5
            assert gap > 4.0, f"{p.name}: the outline must leave a gap around the limb"
        out.append(p)
        if p.mirror if mirror is None else mirror:
            out.append(mirrored(p))

    if not nova:
        # Chest: one angular ceramic breastplate from shoulder to shoulder, notched at the collar,
        # its lower edge a shallow chevron over the abdomen (the sheet's front view).
        half = [(0.0, z(0.905)), (0.035, z(0.93)), (0.085, z(0.955)), (0.128, z(0.945)), (0.150, z(0.89)),
                (0.153, z(0.80)), (0.146, z(0.735)), (0.110, z(0.665)), (0.050, z(0.630)), (0.0, z(0.600))]
        add(Plate("Chest", F, half + [(-x_, z_) for x_, z_ in half[-2:0:-1]],
                  thickness=0.018, crown=0.04, edge=0.45, trim=1, round=0.010, gate=torso_gate, mirror=False,
                  facets=6, facet_mix=0.75))
        # Abdomen: four graphite lames stacked down the midline, each a chevron, with clear gaps
        # between them (they slide over each other as the spine flexes).
        # Split down the linea alba into a segmented six-pack (the sheet's front view).
        rows = ((0.575, 0.495, 0.074), (0.48, 0.40, 0.070), (0.385, 0.305, 0.066), (0.29, 0.215, 0.058))
        for i, (t1, t0, w) in enumerate(rows):
            add(Plate(f"Abs{i}.L", F, [(0.008, z(t1)), (w, z(t1 + 0.012)), (w - 0.006, z(t0 + 0.006)), (0.008, z(t0))],
                      thickness=0.010, crown=0.010, tone=1, round=0.005, gate=torso_gate, facets=2, facet_mix=0.6))
        add(Plate("Oblique.L", F, [(0.094, z(0.555)), (0.148, z(0.60)), (0.152, z(0.38)), (0.118, z(0.25)), (0.088, z(0.27))],
                  thickness=0.009, crown=0.02, tone=1, round=0.006, gate=torso_gate))
        add(Plate("Belt", F, [(-0.16, z(0.185)), (0.16, z(0.185)), (0.16, z(0.145)), (-0.16, z(0.145))],
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
            add(Plate(f"Lumbar{i}.L", B, [(0.034, z(t1)), (0.082, z(t1)), (0.074, z(t0)), (0.034, z(t0))],
                      thickness=0.010, crown=0.014, tone=1, round=0.006, gate=torso_gate))
        # Flanks: white rib plates under the arms continuing the breastplate around the sides.
        add(Plate("Flank.L", S, [(0.055, z(0.80)), (-0.035, z(0.83)), (-0.055, z(0.66)), (-0.03, z(0.50)), (0.04, z(0.47)),
                                 (0.068, z(0.62))],
                  thickness=0.016, crown=0.02, round=0.010, trim=1, gate=torso_gate, extra={"xmin": 0.115},
                  facets=4, facet_mix=0.8))
        # Spinal assembly: graphite vertebra segments down the lower back under the core.
        for i, t1 in enumerate((0.555, 0.49, 0.425, 0.36, 0.295)):
            w = 0.022 - 0.0015 * i
            add(Plate(f"Vertebra{i}", B, [(-w, z(t1)), (w, z(t1)), (w * 0.8, z(t1 - 0.05)), (-w * 0.8, z(t1 - 0.05))],
                      thickness=0.016, crown=0.008, tone=1, round=0.004, gate=torso_gate, mirror=False,
                      facets=3, facet_mix=0.9))
        add(Plate("BeltBack", B, [(-0.16, z(0.185)), (0.16, z(0.185)), (0.16, z(0.145)), (-0.16, z(0.145))],
                  thickness=0.011, crown=0.012, tone=1, round=0.004, gate=torso_gate, mirror=False))
    else:
        # Chest (ADR-0252, the sheet's front views): one sculpted pearl bust plate from the collar
        # notch around both breasts, no centre seam; its outer edges stop short of the arms so the
        # black straps run from the collar over the shoulders and down the flanks.
        half = [(0.0, z(0.905)), (0.03, z(0.925)), (0.07, z(0.935)), (0.102, z(0.915)), (0.122, z(0.86)),
                (0.126, z(0.76)), (0.116, z(0.67)), (0.084, z(0.62)), (0.045, z(0.625)), (0.0, z(0.685))]
        add(Plate("Bust", F, half + [(-x_, z_) for x_, z_ in half[-2:0:-1]],
                  thickness=0.009, crown=0.035, edge=0.45, trim=1, round=0.014, gate=torso_gate, fit=0, fair=3,
                  mirror=False))
        # Corset: one smooth graphite piece cinching the waist, pointed under the bust and at the
        # groin (the sheet's leotard line).
        add(Plate("Corset", F, [(0.0, z(0.585)), (0.05, z(0.57)), (0.092, z(0.56)), (0.084, z(0.44)), (0.076, z(0.32)),
                                (0.080, z(0.20)), (0.05, z(0.06)), (0.0, z(-0.04)), (-0.05, z(0.06)), (-0.080, z(0.20)),
                                (-0.076, z(0.32)), (-0.084, z(0.44)), (-0.092, z(0.56)), (-0.05, z(0.57))],
                  thickness=0.006, crown=0.03, edge=0.5, tone=1, round=0.010, gate=torso_gate, mirror=False))
        # Hip bands: the sheet's white flares sweeping from the side of the waist down into the
        # groin beside the corset's point, over the black high-cut leotard.
        add(Plate("HipBand.L", F, [(0.112, z(0.31)), (0.150, z(0.26)), (0.150, z(0.15)), (0.112, z(0.04)),
                                   (0.074, z(-0.06)), (0.046, z(-0.14)), (0.030, z(-0.12)), (0.052, z(-0.03)),
                                   (0.088, z(0.08)), (0.110, z(0.19))],
                  thickness=0.007, crown=0.025, trim=1, round=0.010,
                  gate=(hips - np.array([0, 0, 0.18]), hips + np.array([0, 0, 0.16]), 0.22)))
        add(Plate("Blade.L", B, [(0.08, z(0.94)), (0.125, z(0.92)), (0.142, z(0.82)), (0.14, z(0.68)), (0.115, z(0.60)),
                                 (0.078, z(0.64))],
                  thickness=0.011, crown=0.035, trim=1, round=0.012, gate=torso_gate))
        add(Plate("Spine", B, [(-0.062, z(0.93)), (0.062, z(0.93)), (0.072, z(0.82)), (0.064, z(0.68)), (0.025, z(0.6)),
                               (-0.025, z(0.6)), (-0.064, z(0.68)), (-0.072, z(0.82))],
                  thickness=0.011, crown=0.02, tone=1, round=0.01, gate=torso_gate, mirror=False))
        # The corset closes at the back: one smooth graphite piece under the spine housing.
        add(Plate("CorsetBack", B, [(0.0, z(0.585)), (0.07, z(0.57)), (0.098, z(0.52)), (0.088, z(0.40)), (0.09, z(0.26)),
                                    (0.06, z(0.20)), (0.0, z(0.19)), (-0.06, z(0.20)), (-0.09, z(0.26)), (-0.088, z(0.40)),
                                    (-0.098, z(0.52)), (-0.07, z(0.57))],
                  thickness=0.006, crown=0.03, edge=0.5, tone=1, round=0.010, gate=torso_gate, mirror=False))
        # The sheet's mechanical spine: a column of graphite vertebra segments riding on the spine
        # housing and the corset back, from between the shoulder blades to the small of the back.
        for i, t1 in enumerate((0.90, 0.835, 0.77, 0.705, 0.64, 0.575, 0.51, 0.445, 0.38)):
            w = 0.024 - 0.0012 * i
            add(Plate(f"Vertebra{i}", B, [(-w, z(t1)), (w, z(t1)), (w * 0.78, z(t1 - 0.05)), (-w * 0.78, z(t1 - 0.05))],
                      thickness=0.007, crown=0.004, tone=1, round=0.003, gate=torso_gate, mirror=False,
                      facets=3, facet_mix=0.9, lift=0.011 if t1 > 0.6 else 0.006))

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
        # Shoulders: Orion's broad, faceted pauldron over the whole deltoid; Nova's a smaller,
        # streamlined cap that leaves the graphite upper arm showing.
        paul = ([(-12, -0.02), (35, -0.11), (90, -0.15), (145, -0.11), (192, -0.02), (190, 0.15), (120, 0.21), (50, 0.20),
                 (-10, 0.14)] if nova else
                [(-28, -0.02), (30, -0.13), (90, -0.17), (150, -0.13), (208, -0.02), (208, 0.25), (120, 0.32), (40, 0.30),
                 (-28, 0.22)])
        add(Plate("Pauldron.L", ua, paul, extra={"xmin": 0.148 if nova else 0.15},
                  thickness=0.012 if nova else 0.019, crown=0.05, trim=1, round=0.016, gate=arm_gate,
                  facets=4, facet_mix=0.45 if nova else 0.55, lift=0.001))
        if not nova:
            # Layered shoulder (the sheet's front and three-quarter views): a second, smaller
            # faceted shell rides on top of the pauldron, its edge standing clear of the one below.
            add(Plate("PauldronTop.L", ua, [(5, -0.10), (90, -0.15), (175, -0.10), (178, 0.12), (90, 0.20), (2, 0.12)],
                      extra={"xmin": 0.15}, thickness=0.008, crown=0.03, round=0.012, gate=arm_gate,
                      lift=0.019, fair=16))
        add(Plate("Lame.L", ua, [(-45, 0.40), (40, 0.46), (130, 0.50), (215, 0.44), (210, 0.58), (120, 0.62),
                                (30, 0.58), (-40, 0.54)],
                  thickness=0.015 * k, crown=0.02, round=0.008, gate=arm_gate, tone=1))
        if not nova:
            # Outer upper-arm plate over the graphite sleeve.
            add(Plate("UpperArm.L", ua, [(35, 0.50), (150, 0.48), (165, 0.62), (150, 0.84), (35, 0.86), (20, 0.68)],
                      thickness=0.016, crown=0.02, round=0.008, trim=1, gate=arm_gate, facets=4, facet_mix=0.8))
        if nova:
            # The sheet's white upper arm: a pearl sleeve wrapping all but the inner arm, where the
            # black undersuit shows as a strip down to the elbow.
            add(Plate("Bicep.L", ua, [(-30, 0.62), (60, 0.60), (150, 0.62), (205, 0.64), (205, 0.88), (120, 0.90),
                                      (30, 0.90), (-30, 0.86)],
                      thickness=0.008, crown=0.02, trim=1, round=0.010, gate=arm_gate, facets=4, facet_mix=0.35))
        else:
            add(Plate("Bicep.L", ua, [(-40, 0.66), (28, 0.66), (28, 0.86), (-40, 0.86)],
                      thickness=0.008, crown=0.015, tone=1, round=0.008, gate=arm_gate))
        add(Plate("Elbow.L", fa, [(120, -0.08), (240, -0.08), (250, 0.10), (180, 0.14), (110, 0.10)],
                  thickness=0.013, crown=0.02, tone=1, round=0.008, gate=(el - da * 0.08, el + _unit(wr - el) * 0.08, 0.09)))
        # Forearm: a gauntlet wrapping all but the inner wrist, its top cut on a slant.
        add(Plate("Gauntlet.L", fa, [(-100, 0.36), (-20, 0.20), (90, 0.12), (200, 0.18), (240, 0.36), (240, 0.90),
                                    (-100, 0.90)],
                  thickness=0.013 * k, crown=0.035, trim=1, round=0.010, gate=(el, wr, 0.09),
                  facets=4 if nova else 6, facet_mix=0.45 if nova else 0.65))
        if nova:
            # The sheet's forearms (ADR-0252): black segmented inserts down the outer side of the
            # pearl gauntlet.
            for i, (t0, t1) in enumerate(((0.30, 0.46), (0.50, 0.66), (0.70, 0.86))):
                add(Plate(f"ForearmSeg{i}.L", fa, [(35, t0), (115, t0 + 0.01), (110, t1), (40, t1 - 0.01)],
                          thickness=0.005, crown=0.010, tone=1, round=0.005, gate=(el, wr, 0.09),
                          lift=0.012, facets=3, facet_mix=0.7))
        # Thighs: the front-outer plate with a slanted hip cut, a rear plate.
        thick_gate = (hp + _unit(kn - hp) * 0.02, kn, 0.14)
        if not nova:
            # Front-outer thigh armour in two layered plates (the graphite inner thigh and a
            # graphite band above the knee show between them, the sheet's front view).
            add(Plate("Thigh.L", th, [(-14, 0.30), (0, 0.14), (55, 0.07), (115, 0.12), (132, 0.30), (128, 0.60), (70, 0.67),
                                     (0, 0.64), (-14, 0.58)],
                      thickness=0.015, crown=0.05, trim=1, round=0.012, gate=thick_gate, facets=5, facet_mix=0.55))
            add(Plate("ThighLow.L", th, [(-22, 0.72), (60, 0.74), (122, 0.71), (118, 0.86), (60, 0.90), (-22, 0.87)],
                      thickness=0.013, crown=0.03, round=0.010, gate=thick_gate, facets=4, facet_mix=0.5))
            add(Plate("Hamstring.L", th, [(178, 0.28), (238, 0.34), (238, 0.80), (178, 0.76)],
                      thickness=0.010, crown=0.02, tone=1, round=0.008, gate=thick_gate))
        else:
            # Nova's broader thigh assembly: a long sweeping front-outer plate cut on a diagonal
            # above the knee, and a separate knee-guard plate below the cut.
            add(Plate("ThighLow.L", th, [(-35, 0.80), (40, 0.74), (130, 0.70), (160, 0.74), (160, 0.88), (70, 0.92), (-30, 0.90)],
                      thickness=0.009, crown=0.03, round=0.010, trim=0, gate=thick_gate, facets=4, facet_mix=0.35))
            # Its top edge runs parallel to the hip band, high outside and low inside, so the
            # leotard's black shows as a triangle at the top of the inner thigh (ADR-0252).
            add(Plate("Thigh.L", th, [(-40, 0.42), (0, 0.34), (60, 0.20), (120, 0.11), (165, 0.18), (165, 0.64),
                                     (140, 0.62), (40, 0.68), (-35, 0.74), (-40, 0.70)],
                      thickness=0.009, crown=0.045, trim=1, round=0.014, gate=thick_gate, facets=5, facet_mix=0.35))
            add(Plate("Hamstring.L", th, [(205, 0.34), (238, 0.36), (238, 0.82), (205, 0.84)],
                      thickness=0.008, crown=0.015, tone=0, round=0.006, gate=thick_gate))
            # A raised outer-thigh panel over the sweep, its own lit groove the sheet's violet line.
            add(Plate("ThighPanel.L", th, [(45, 0.30), (125, 0.26), (125, 0.58), (45, 0.56)],
                      thickness=0.006, crown=0.015, round=0.010, trim=1, gate=thick_gate, lift=0.010,
                      facets=3, facet_mix=0.6))
        # Knee: a faceted graphite cop.
        ks = Chart("limb", 1.0, kn, an, seam=-120, radius=0.06)
        add(Plate("Kneecap.L", ks, [(0, -0.075), (40, -0.05), (42, 0.035), (0, 0.075), (-42, 0.035), (-40, -0.05)],
                  thickness=0.030 * k, crown=0.02, round=0.006, trim=1, tone=0, facets=4, facet_mix=0.5 if nova else 0.85,
                  gate=(kn - _unit(an - kn) * 0.09, kn + _unit(an - kn) * 0.08, 0.12)))
        add(Plate("Knee.L", ks, [(0, -0.13), (58, -0.085), (62, 0.05), (0, 0.11), (-62, 0.05), (-58, -0.085)],
                  thickness=0.020 * k, crown=0.025, tone=1, round=0.008, gate=(kn - _unit(an - kn) * 0.09, kn + _unit(an - kn) * 0.08, 0.12)))
        # Shin guard (a wedge top under the knee) and calf plate.
        add(Plate("Shin.L", sn, [(0, 0.135), (70, 0.17), (105, 0.26), (100, 0.80), (-62, 0.80), (-66, 0.26), (-50, 0.17)],
                  thickness=0.015 * k, crown=0.04, trim=1, round=0.010, gate=(kn, an, 0.12),
                  facets=4 if nova else 5, facet_mix=0.45 if nova else 0.6))
        add(Plate("Calf.L", sn, [(140, 0.20), (238, 0.22), (238, 0.62), (140, 0.66)],
                  thickness=0.012 * k, crown=0.03, tone=0 if nova else 1, round=0.010, gate=(kn, an, 0.12)))
    return out
