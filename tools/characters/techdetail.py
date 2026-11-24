# SPDX-License-Identifier: Apache-2.0
"""Procedural hard-surface detail for Nova's suit (ADR-0252; docs/CHARACTERS.md).

The ORION & NOVA sheet's Nova is not smooth latex and plain plates: the black under-layer between
her pearl plates is a dense cybernetic layer - stepped plates a few millimetres to two centimetres
across, slots, vents, bolts and silver-chamfered pieces, packed tightest down the spine, over the
shoulder straps, the flanks, the arms and the knees - and the black bites into the white plates'
edges in jagged teeth. Everything here is texture detail (height -> normal map, colour, roughness,
metal, cavity): the geometry budget is spent on the silhouette.

Coordinates: each texel gets a 2D surface frame (u, v) in metres that follows the body - around and
along each limb bone, and a triplanar projection on the torso mirrored about the midline so the
pattern is symmetric like a designed suit. The pattern is a brick lattice of panels at three
scales, each panel picking a feature from a per-cell hash.
"""
from __future__ import annotations

import numpy as np

from bake import _hash3


def _unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def _smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


LIMBS = (("UpperArm", "LowerArm", 0.10, 0.040), ("LowerArm", "Hand", 0.08, 0.034),
         ("UpperLeg", "LowerLeg", 0.15, 0.065), ("LowerLeg", "Foot", 0.12, 0.050),
         ("Hand", "Middle1", 0.07, 0.030))


def surface_coords(P, N, J):
    """(u, v) metres in a body-following frame, and a region id (0 torso, 1+ limb segments)."""
    n = len(P)
    best = np.full(n, np.inf)
    u = np.zeros(n)
    v = np.zeros(n)
    region = np.zeros(n, np.int64)
    Y = np.array([0, 1.0, 0])
    rid = 1
    for t, side in (("L", 1.0), ("R", -1.0)):
        for a_name, b_name, rad, R0 in LIMBS:
            a, b = J[f"{a_name}.{t}"][0], J[f"{b_name}.{t}"][0]
            d = b - a
            ln = np.linalg.norm(d)
            d = d / ln
            e1 = _unit(np.cross(Y, d))
            if e1[0] * side < 0:
                e1 = -e1
            e2 = np.cross(d, e1)
            rel = P - a
            s = rel @ d
            rad_v = rel - np.outer(s, d)
            dist = np.linalg.norm(rad_v, axis=1)
            sc = np.clip(s, 0, ln)
            dseg = np.linalg.norm(rel - np.outer(sc, d), axis=1)
            ok = (s > -0.01) & (s < ln + 0.01) & (dist < rad) & (dseg < best)
            # Mirror the angle on the right side so both limbs carry the same layout.
            ang = np.arctan2(rad_v @ e2, (rad_v @ e1))
            best = np.where(ok, dseg, best)
            u = np.where(ok, ang * R0, u)
            v = np.where(ok, s, v)
            region = np.where(ok, rid, region)
            rid += 1
    tor = region == 0
    if tor.any():
        x, y, z = np.abs(P[tor, 0]), P[tor, 1], P[tor, 2]
        an = np.abs(N[tor])
        side = (an[:, 0] > an[:, 1]) & (an[:, 0] > an[:, 2])
        cap = (an[:, 2] > an[:, 0]) & (an[:, 2] > an[:, 1])
        uu = np.where(side, y + 1.0, np.where(cap, x + 2.0, x))
        vv = np.where(cap, y, z)
        u[tor], v[tor] = uu, vv
    return u, v, region


def _cells(u, v, su, sv, seed, region):
    """Brick lattice: (distance to the cell border in metres, cell hash a, b, c, local s, t in
    [0, 1])."""
    row = np.floor(v / sv)
    uo = u / su + 0.5 * (_hash3(row.astype(np.int64), region, np.zeros_like(region), seed) > 0.5)
    col = np.floor(uo)
    fu, fv = uo - col, v / sv - row
    db = np.minimum(np.minimum(fu, 1 - fu) * su, np.minimum(fv, 1 - fv) * sv)
    ci, ri = col.astype(np.int64), row.astype(np.int64)
    h = [_hash3(ci, ri, region, seed + k) for k in range(3)]
    return db, h[0], h[1], h[2], fu, fv


def greeble(P, N, J, seed, density=1.0, scale=1.0, coords=None):
    """Hard-surface panel detail. Returns height (m), colour multiplier, metal, roughness offset,
    cavity multiplier and a 0/1 'piece' mask (texels on a raised piece, for tinting)."""
    u, v, region = coords if coords is not None else surface_coords(P, N, J)
    n = len(P)
    h = np.zeros(n)
    cm = np.ones(n)
    metal = np.zeros(n)
    rough = np.zeros(n)
    cav = np.ones(n)
    piece = np.zeros(n)
    bev = 0.0007 * scale
    levels = ((0.026 * scale, 0.016 * scale, 0.00090), (0.012 * scale, 0.0075 * scale, 0.00060),
              (0.0055 * scale, 0.0040 * scale, 0.00040))
    parent = np.ones(n)
    for li, (su, sv, amp) in enumerate(levels):
        db, a, b, c, fu, fv = _cells(u, v, su, sv, seed + 101 * li, region)
        prof = _smooth(db / bev)  # 0 at the border, 1 inside
        groove = np.exp(-(db / (0.00045 * scale)) ** 2)
        act = parent * (a < (0.62, 0.70, 0.45)[li] * density)
        raised = act * (b < 0.45)
        sunk = act * (b >= 0.45) * (b < 0.62)
        slots = act * (b >= 0.62) * (b < 0.74)
        bolt = act * (b >= 0.74) * (b < 0.82)
        silver = act * (b >= 0.86) * (b < 0.895)
        hh = (raised + silver) * prof * amp * (0.6 + 0.8 * c) - sunk * prof * amp * 0.8
        # Slots: three to five recessed bars across the panel.
        k = 3 + np.floor(c * 3)
        bar = (np.abs(((fu * k) % 1.0) - 0.5) < 0.22) & (fv > 0.18) & (fv < 0.82) & (fu > 0.08) & (fu < 0.92)
        hh -= slots * bar * amp * 0.9
        # Bolt: a small raised boss with a dark centre.
        rr = np.hypot((fu - 0.5) * su, (fv - 0.5) * sv)
        br = 0.22 * min(su, sv)
        boss = bolt * _smooth((br - rr) / (0.25 * br))
        hh += boss * amp * 0.7 - bolt * _smooth((0.35 * br - rr) / (0.15 * br)) * amp * 0.4
        h += hh
        cav *= 1 - 0.55 * groove * act - 0.35 * slots * bar
        cm *= 1 - 0.35 * groove * act
        cm *= 1 + (0.30 * (c - 0.5)) * raised
        cm *= 1 - 0.5 * slots * bar
        # Silver: brushed metal pieces, and a bright chamfer on some raised edges.
        cham = raised * (c > 0.90) * np.exp(-((db - bev * 0.6) / (bev * 0.5)) ** 2)
        met = np.clip(silver * prof + boss + cham, 0, 1)
        metal = np.maximum(metal, met)
        rough += -0.10 * raised * prof + 0.18 * groove * act + 0.15 * slots * bar
        piece = np.maximum(piece, (raised + silver) * prof)
        # Finer levels only subdivide some of this level's panels.
        parent = act * (c < 0.65) + (1 - act) * (a < 0.35)
    return {"height": h, "color_mul": np.clip(cm, 0.2, 1.6), "metal": metal, "rough": rough,
            "cavity": np.clip(cav, 0, 1), "piece": piece, "u": u, "v": v, "region": region}


def hex_scales(u, v, cell, seed):
    """Fine overlapping hexagonal scales (the sheet's seat and corset): distance to the scale edge
    in metres and a per-scale hash."""
    x = u / cell
    y = v / (cell * 0.8660254)
    ax, ay = np.round(x), np.round(y / 2) * 2
    bx, by = np.floor(x) + 0.5, np.floor((y - 1) / 2) * 2 + 1
    da = (x - ax) ** 2 + ((y - ay) * 0.8660254) ** 2
    db = (x - bx) ** 2 + ((y - by) * 0.8660254) ** 2
    cx = np.where(da < db, ax, bx)
    cy = np.where(da < db, ay, by)
    dx, dy = np.abs(x - cx), np.abs((y - cy) * 0.8660254)
    r = np.maximum(dx, dx * 0.5 + dy * 0.8660254)
    hv = _hash3((cx * 2).astype(np.int64), cy.astype(np.int64), np.zeros(len(u), np.int64), seed)
    return (0.5 - r) * cell, hv, (y - cy) * 0.8660254


def black_layer(P, N, J, seed, fabric, accent, metal_c, coords=None, scale_cell=0.0034):
    """Nova's black under-layer: cybernetic hard-surface detail where `fabric` is 0, glossy
    hexagonal micro-scales where it is 1 (the seat, the corset front). Absolute maps: colour,
    roughness, metal, height (m), cavity."""
    n = len(P)
    g = greeble(P, N, J, seed, coords=coords)
    u, v, region = g["u"], g["v"], g["region"]
    accent = np.asarray(accent, float)
    metal_c = np.asarray(metal_c, float)
    # Mechanical: graphite pieces in a few shades, brushed-silver hardware.
    shade = accent[None] * (1.0 + 0.9 * g["piece"][:, None]) * g["color_mul"][:, None]
    # Mostly gunmetal; only the odd piece catches the light as bright silver.
    tint = 0.22 + 0.60 * (_hash3(np.floor(u / 0.006).astype(np.int64), np.floor(v / 0.006).astype(np.int64), region, seed + 3) > 0.78)
    col_m = shade * (1 - g["metal"][:, None]) + metal_c[None] * tint[:, None] * g["metal"][:, None]
    rough_m = 0.42 + g["rough"] - 0.06 * g["metal"]
    # Fabric: domed hex scales with dark grooves, glossy on the crowns.
    ed, hv, _ = hex_scales(u, v, scale_cell, seed + 7)
    crown = _smooth(ed / (scale_cell * 0.28))
    gro = np.exp(-(ed / (scale_cell * 0.07)) ** 2)
    h_f = crown * 0.00022 - gro * 0.00008
    col_f = accent[None] * (0.85 + 0.45 * hv[:, None]) * (1 - 0.45 * gro[:, None]) * (1 + 0.35 * crown[:, None])
    rough_f = 0.20 + 0.30 * gro + 0.06 * hv
    w = np.clip(fabric, 0, 1)
    col = col_m * (1 - w[:, None]) + col_f * w[:, None]
    rough = rough_m * (1 - w) + rough_f * w
    metal = g["metal"] * (1 - w)
    h = g["height"] * (1 - w) + h_f * w
    cav = g["cavity"] * (1 - w) + (1 - 0.4 * gro) * w
    return {"color": col, "rough": np.clip(rough, 0.08, 1), "metal": metal, "height": h, "cavity": cav,
            "coords": (u, v, region)}
