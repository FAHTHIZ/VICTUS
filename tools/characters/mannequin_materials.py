# SPDX-License-Identifier: Apache-2.0
"""Per-texel PBR maps of the VICTUS Frame mannequin (mannequin.py, docs/CHARACTERS.md, ADR-0198).

Every function returns {"color", "rough", "metal", "height" (metres, -> normal map), "cavity"
(multiplied into the baked AO), optionally "emissive"} per covered texel, the contract
build_characters.bake_part expects.

  Frame   rubberised undersuit with a hexagonal micro-weave, ribbed flex zones at the neck,
          shoulders, elbows, waist and knees, recessed piping along the limbs carrying a thin light
          line; the head shell is smooth satin with a centre crest seam;
  Armor   satin ceramic plates with rounded bevels, an inset groove a centimetre in from every edge
          (a light strip on the larger plates), a chevron emblem on the chest; boots with a moulded
          sole and tread; ear pods and the back core in machined metal with light rings / vents;
  Visor   smoked glass with a fine scan-line grid, and the face-display glyphs (emissive).
"""
from __future__ import annotations

import numpy as np

import fit
import suit
from bake import band, dist_to, fbm, soft, worley


class Landmarks:
    """The landmarks sockets, hand axes and the bakes need (the mannequin has no eyes or mouth)."""

    def __init__(self, body: fit.Body, info):
        J = {n: (np.asarray(h), np.asarray(t)) for n, _, h, t in body.joints}
        self.J = J
        self.head = info["head"]
        self.visor = info["visor"]
        self.visor_plate = info.get("visor_plate")
        self.torso = info["torso"]
        self.key = body.spec.key
        self.head_L = info.get("head_landmarks")  # human.py: the unhelmeted head's landmarks
        self.head_style = info.get("head_style")
        self.eL, self.eR = J["Eye.L"][0], J["Eye.R"][0]
        self.eye_mid = 0.5 * (self.eL + self.eR)
        self.collar = J["Neck"][0][2] + 0.35 * (J["Head"][0][2] - J["Neck"][0][2])
        self.hands = {}
        for t, side in (("L", 1.0), ("R", -1.0)):
            i1, p1 = J[f"Index1.{t}"][0], J[f"Pinky1.{t}"][0]
            wr = J[f"Hand.{t}"][0]
            m1 = J[f"Middle1.{t}"][0]
            n = np.cross(i1 - p1, m1 - wr)
            n /= np.linalg.norm(n)
            if n[0] * side < 0:
                n = -n
            self.hands[t] = {"dorsal": n, "wrist": wr}


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def _unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def _seg(P, a, b):
    """(t along a->b in [0, 1] unclamped, radial vector from the axis) per point."""
    d = b - a
    L2 = float(d @ d)
    t = ((P - a) @ d) / L2
    radial = P - a - np.outer(t, d)
    return t, radial


def _hex(ij, cell):
    """Distance (texels) to the nearest edge of a hexagonal grid in texture space, and the cell id."""
    x = ij[:, 0] / cell
    y = ij[:, 1] / (cell * 0.8660254)
    # Two offset rectangular lattices; the nearest centre wins.
    ax, ay = np.round(x), np.round(y / 2) * 2
    bx, by = np.floor(x) + 0.5, np.floor((y - 1) / 2) * 2 + 1
    da = (x - ax) ** 2 + ((y - ay) * 0.8660254) ** 2
    db = (x - bx) ** 2 + ((y - by) * 0.8660254) ** 2
    cx = np.where(da < db, ax, bx)
    cy = np.where(da < db, ay, by)
    dx, dy = np.abs(x - cx), np.abs((y - cy) * 0.8660254)
    # Hexagon "radius" metric (pointy-top): max of the three edge-normal projections.
    r = np.maximum(dx, dx * 0.5 + dy * 0.8660254)
    return (0.5 - r) * cell, cx * 7 + cy * 13


def cells(P, spacing, seed=0, stretch=None):
    """Jittered-cell partition of space: (distance in metres to the nearest cell border, i.e.
    (F2 - F1) / 2, and a per-cell random value in [0, 1)). `stretch` scales axes before the lookup
    (elongated cells)."""
    from scipy.spatial import cKDTree

    from bake import _hash3

    Q = P * (np.asarray(stretch) if stretch is not None else 1.0)
    lo = Q.min(0) - 2 * spacing
    cell = np.unique(np.floor((Q - lo) / spacing).astype(np.int64), axis=0)
    off = np.array([[a, b, c] for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)])
    cell = np.unique((cell[:, None, :] + off[None]).reshape(-1, 3), axis=0)
    jit = np.stack([_hash3(cell[:, 0], cell[:, 1], cell[:, 2], seed + k) for k in range(3)], 1)
    pts = lo + (cell + 0.15 + 0.7 * jit) * spacing
    d, i = cKDTree(pts).query(Q, k=2)
    p1, p2 = pts[i[:, 0]], pts[i[:, 1]]
    m = 0.5 * (p1 + p2)
    nrm = (p2 - p1) / np.maximum(np.linalg.norm(p2 - p1, axis=1, keepdims=True), 1e-12)
    border = np.abs(((Q - m) * nrm).sum(1))
    val = _hash3(cell[i[:, 0], 0], cell[i[:, 0], 1], cell[i[:, 0], 2], seed + 7)
    return border, val


def frame_maps(tx, P, N, L: Landmarks, style, kind, seed):
    n = len(P)
    J = L.J
    k = np.rint(kind).astype(np.int64)
    head = k == 1
    body = ~head
    base = np.array(style.frame)
    glow = np.array(style.glow) * style.trim_light
    col = np.tile(base, (n, 1))
    rough = np.full(n, style.frame_rough)
    metal = np.zeros(n)
    h = np.zeros(n)
    cavity = np.ones(n)
    em = np.zeros((n, 3))
    ij = tx.ij.astype(float)
    mottle = fbm(P, 30, 3, seed + 1)

    # Hexagonal micro-weave (texture space, ~2.5 mm cells at production density).
    edge_d, cid = _hex(ij * (tx.size / 2048.0) ** -1, 7.0)
    cell_var = (np.sin(cid * 12.9898) * 43758.5453) % 1.0
    weave_groove = np.exp(-(edge_d / 0.9) ** 2)
    h[body] += (-weave_groove * 0.00005 + cell_var * 0.000008)[body]
    col[body] *= (1 + 0.05 * (cell_var[:, None] - 0.5) - 0.10 * weave_groove[:, None])[body]
    rough[body] += (0.06 * weave_groove - 0.04 * cell_var)[body]
    cavity[body] -= 0.12 * weave_groove[body]

    # Ribbed flex zones: rings across the bone around each joint.
    ribs = np.zeros(n)

    def flex(a, b, centre_t, half, pitch, radius):
        t, radial = _seg(P, a, b)
        L_ = np.linalg.norm(b - a)
        s = (t - centre_t) * L_
        zone = band(s, -half, half, 0.006) * (np.linalg.norm(radial, axis=1) < radius)
        r = 0.5 + 0.5 * np.cos(2 * np.pi * s / pitch)
        return zone * r, zone

    zones = []
    for t in ("L", "R"):
        sh, el, wr = J[f"UpperArm.{t}"][0], J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0]
        hp, kn, an = J[f"UpperLeg.{t}"][0], J[f"LowerLeg.{t}"][0], J[f"Foot.{t}"][0]
        zones.append(flex(sh, el, 1.0, 0.035, 0.0045, 0.09))  # elbow
        zones.append(flex(el, wr, 0.97, 0.012, 0.004, 0.06))  # wrist cuff
        zones.append(flex(hp, kn, 1.0, 0.04, 0.005, 0.12))  # knee
        zones.append(flex(J[f"Shoulder.{t}"][0], sh, 1.0, 0.025, 0.005, 0.10))  # shoulder cap
    hips, neck, headj = J["Hips"][0], J["Neck"][0], J["Head"][0]
    zones.append(flex(hips, neck, 0.06, 0.035, 0.006, 0.25))  # waist
    neck_r, neck_z = flex(neck, headj, 0.45, 0.05, 0.005, 0.09)  # neck
    zones.append((neck_r, neck_z))
    zone_any = np.zeros(n)
    for r, z in zones:
        ribs = np.maximum(ribs, r)
        zone_any = np.maximum(zone_any, z)
    zb = zone_any * body
    h += zb * (ribs ** 1.5 * 0.0006 - 0.0002)
    col = col * (1 - 0.25 * zb[:, None]) + (base * 0.6 + np.array(style.metal) * 0.05) * 0.25 * zb[:, None]
    rough += zb * (0.12 * (1 - ribs) - 0.05)
    cavity -= zb * 0.35 * (1 - ribs) ** 2
    # The sheet's neck is a slim near-black column: darker and satin, its ribs subtler.
    nz = neck_z * body
    col = col * (1 - 0.55 * nz[:, None])
    h -= nz * ribs ** 1.5 * 0.0003

    # Piping: a recessed seam along the outside of every limb carrying a thin light line.
    pipe = np.zeros(n)
    Y = np.array([0, 1.0, 0])
    for t, s in (("L", 1.0), ("R", -1.0)):
        segs = [(J[f"UpperArm.{t}"][0], J[f"LowerArm.{t}"][0], 0.09), (J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0], 0.07),
                (J[f"UpperLeg.{t}"][0], J[f"LowerLeg.{t}"][0], 0.13), (J[f"LowerLeg.{t}"][0], J[f"Foot.{t}"][0], 0.10)]
        for a, b, rad in segs:
            d = _unit(b - a)
            outer = _unit(np.cross(Y, d)) * (1 if s > 0 else -1)
            if outer[0] * s < 0:
                outer = -outer
            perp = _unit(np.cross(d, outer))
            tt, radial = _seg(P, a, b)
            on = (tt > 0.02) & (tt < 0.98) & (radial @ outer > 0) & (np.linalg.norm(radial, axis=1) < rad)
            w = np.abs(radial @ perp)
            pipe = np.maximum(pipe, on * np.exp(-(w / 0.0012) ** 2))
    pb = pipe * body * (1 - zone_any)
    h -= pb * 0.00035
    em += pb[:, None] * glow * 0.9
    col = col * (1 - pb[:, None]) + glow * 0.25 * pb[:, None]
    rough -= pb * 0.25

    # Moulded panels following the muscles (suit.py): a recessed seam along each outline, a soft
    # crown inside it, light lines in the seams of the flagged panels and down the flanks.
    seam_all = np.zeros(n)
    relief = np.zeros(n)
    line = np.zeros(n)
    sat = np.zeros(n)  # alternate panels are glossier
    free = body & (zone_any < 0.5)
    for i, pn in enumerate(suit.panels(J, L.torso, L.key)):
        gv = pn.plate.g(P)
        inside = gv < 0
        if not (inside & free).any() and not ((np.abs(gv) < 0.004) & free).any():
            continue
        if pn.seam:
            seam_all = np.maximum(seam_all, np.exp(-(gv / 0.0015) ** 2))
            relief = np.maximum(relief, inside * pn.relief * smooth(-gv / pn.crown))
            sat = np.where(inside, (i % 3 == 0) * 1.0, sat)
        if pn.glow:
            line = np.maximum(line, pn.glow * np.exp(-(gv / 0.0011) ** 2))
            seam_all = np.maximum(seam_all, np.exp(-(gv / 0.0018) ** 2))
    fd, fon = suit.flank(P, L.torso, J, L.key)
    fon = fon & (np.abs(P[:, 0]) < 0.21) & (np.abs(N[:, 0]) > 0.45)
    line = np.maximum(line, fon * np.exp(-(fd / 0.0011) ** 2))
    seam_all = np.maximum(seam_all, fon * np.exp(-(fd / 0.0013) ** 2))
    mfree = free * (1 - pb)
    seam_all *= mfree
    line *= mfree
    relief *= mfree
    h += relief - seam_all * 0.00035
    cavity -= seam_all * 0.5
    col = col * (1 - 0.45 * seam_all[:, None])
    rough += seam_all * 0.12 - 0.10 * sat * mfree
    em += line[:, None] * glow * 0.9
    col = col * (1 - line[:, None]) + glow * 0.25 * line[:, None]

    # Head shell: satin finish, a centre crest seam front to back, two side seams.
    if head.any():
        Ph = P[head]
        rel = Ph - L.head.c
        top = rel[:, 2] > -0.01
        crest = np.exp(-(np.abs(Ph[:, 0]) / 0.0012) ** 2) * top * (Ph[:, 2] > L.visor[1] - 0.004)
        side = np.zeros(len(Ph))
        for s in (1.0, -1.0):
            ang = np.arctan2(rel[:, 2] + 0.02, s * rel[:, 0])
            side = np.maximum(side, (s * rel[:, 0] > 0.03) * np.exp(-((ang - 0.55) / 0.02) ** 2) * (rel[:, 1] > -0.07))
        seam = np.clip(crest + side, 0, 1)
        h[head] = -seam * 0.00045 + fbm(Ph, 900, 2, seed + 3) * 0.000006
        col[head] = base * (1.0 + 0.04 * mottle[head, None]) * (1 - 0.4 * seam[:, None])
        rough[head] = style.frame_rough * 0.62 + 0.15 * seam
        cavity[head] = 1 - 0.5 * seam

    if L.key == "nova":
        dk = _nova_black(P, J, L, zone_any) * body
        black = np.array(style.accent) * (1.0 + 0.05 * mottle[:, None])
        col = col * (1 - dk[:, None]) + black * dk[:, None]
        rough = rough * (1 - dk) + (0.30 + 0.08 * (1 - ribs)) * dk
        em *= (1 - dk[:, None])

    rough += 0.04 * mottle
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": metal,
            "height": h, "cavity": np.clip(cavity, 0, 1), "emissive": np.clip(em, 0, 1)}


def _nova_black(P, J, L, zone_any):
    """Nova's black under-layer (ORION & NOVA sheet, Frame 02; ADR-0252): the whole undersuit is
    black composite from the neck to the top of the thighs - the collar, the straps over the
    shoulders and down the flanks, the arms, gloves and every gap between the pearl plates - and
    the high-cut leotard reaches down the inner thigh along a line parallel to the hip bands. The
    legs below that line are pearl except the knee joints. 1 = black, soft-edged."""
    hips = J["Hips"][0]
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    ax = np.abs(x)
    back = smooth((y - hips[1] + 0.01) / 0.05)  # 0 front, 1 back
    # Leg line: high outside, low on the inner thigh in front; under the glute fold behind.
    front_leg = hips[2] - 0.035 - 1.05 * np.clip(0.175 - ax, 0, 0.14)
    back_leg = hips[2] - 0.165 + 0.25 * np.maximum(ax - 0.03, 0)
    leg_line = front_leg * (1 - back) + back_leg * back
    upper = soft(leg_line - z, -0.004, 0.004)
    # Gloves: past the wrist.
    glove = np.zeros(len(P))
    for t in ("L", "R"):
        el, wr = J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0]
        d = _unit(wr - el)
        glove = np.maximum(glove, soft(-((P - wr) @ d), -0.025, -0.012) * (np.linalg.norm(P - wr, axis=1) < 0.25))
    return np.clip(np.maximum.reduce([upper, glove, zone_any]), 0, 1)


def armor_maps(tx, P, N, L: Landmarks, style, kind, cuff, trim, attrs, seed, tone=None):
    n = len(P)
    J = L.J
    k = np.rint(kind).astype(np.int64)
    plate, boot, pod, core, helmet, emblem = (k == 0), (k == 1), (k == 2), (k == 3), (k == 4), (k == 5)
    collar = k == 6
    trim = np.rint(trim).astype(np.int64)
    e = attrs["edge"][:, 0]
    centre = attrs["centre"]
    armor = np.array(style.armor)
    accent = np.array(style.accent)
    metal_c = np.array(style.metal)
    glow = np.array(style.glow) * style.trim_light
    graphite = (np.rint(tone) == 1) if tone is not None else np.zeros(n, bool)
    graphite = graphite & ~boot
    col = np.where(graphite[:, None], accent[None], armor[None]).astype(float)
    rough = np.where(graphite, style.armor_rough + 0.18, style.armor_rough)  # satin composite, not glazed
    metal = np.zeros(n)
    h = np.zeros(n)
    cavity = np.ones(n)
    em = np.zeros((n, 3))
    mottle = fbm(P, 22, 3, seed + 11)
    fine = fbm(P, 700, 2, seed + 12)
    shell = plate | boot | helmet | collar

    # Rolled edge: a soft bevel into the cut edge, then an inset groove 8 mm in from it; on the
    # trimmed white plates the groove carries a thin light line.
    bevel = np.exp(-(e / 0.004) ** 2)
    groove_at = np.where(boot, 0.006, 0.008)
    groove = np.exp(-((e - groove_at) / 0.0011) ** 2) * shell * (cuff < 0.5)
    h += shell * (-bevel * 0.0010) - groove * 0.0005
    cavity -= groove * 0.5 + shell * bevel * 0.10
    col = col * (1 - 0.5 * groove[:, None])
    rough += groove * 0.15
    lit = groove * (trim > 0) * ~graphite
    em += lit[:, None] * glow * 1.4
    col = col * (1 - lit[:, None]) + glow * 0.3 * lit[:, None]

    # Graphite composite: a fine twill weave under the coat.
    if graphite.any():
        Pg = P[graphite]
        a_ = np.sin(2 * np.pi * (Pg[:, 0] + Pg[:, 2]) / 0.0042)
        b_ = np.sin(2 * np.pi * (Pg[:, 0] - Pg[:, 2] + Pg[:, 1]) / 0.0042)
        tw = 0.5 + 0.5 * np.sign(a_) * np.sign(b_) * np.minimum(np.abs(a_), np.abs(b_)) ** 0.5
        h[graphite] += tw * 0.00008
        col[graphite] *= (0.85 + 0.3 * tw)[:, None]
        rough[graphite] += 0.06 * (1 - tw)

    # Panel lines on the larger white plates: a few recessed seams splitting each plate into
    # moulded sections, small graphite inserts and fasteners near the seams.
    inner = (plate | helmet) & (e > 0.018) & (cuff < 0.5) & ~graphite
    pb_, pv_ = cells(P, 0.12, seed + 41)
    pline = np.exp(-(pb_ / 0.0006) ** 2) * inner * soft(e, 0.018, 0.026)
    insert = inner * (pv_ < 0.10) * soft(pb_, 0.0016, 0.0026) * soft(e, 0.024, 0.03) * plate
    h += -pline * 0.0004 - insert * 0.0004
    cavity -= pline * 0.45
    col = col * (1 - 0.5 * pline[:, None])
    col = col * (1 - insert[:, None]) + accent * insert[:, None]
    rough += pline * 0.12 + insert * 0.1
    fv_ = cells(P, 0.022, seed + 43)[1]
    fast_c = worley(P, 0.022, seed + 43) * 0.022
    fast = inner * (fv_ < 0.06) * np.exp(-(fast_c / 0.0015) ** 4) * (pb_ > 0.003)
    h += fast * 0.0003
    col = col * (1 - fast[:, None]) + metal_c * fast[:, None]
    metal = np.maximum(metal, fast)
    rough = rough * (1 - fast) + 0.3 * fast

    # Boots: graphite uppers, a white ceramic toe cap and heel counter, a lug sole.
    if boot.any():
        Pb = P[boot]
        z = Pb[:, 2]
        sole_top = 0.034
        sole = soft(z - sole_top, -0.0015, 0.0015)
        tread = np.clip((worley(Pb * np.array([1, 1, 0.0]), 0.008, seed + 13) - 0.4) * 4, 0, 1) * (N[boot, 2] < -0.3)
        ribs = 0.5 + 0.5 * np.sin(2 * np.pi * z / 0.0045)
        cap = np.zeros(len(Pb))
        heel = np.zeros(len(Pb))
        for t in ("L", "R"):
            toe = J[f"Toe.{t}"][0]
            cap = np.maximum(cap, soft(dist_to(Pb, toe + np.array([0, -0.025, -0.005])), 0.040, 0.044) * (z > sole_top + 0.004))
            hc = J[f"Foot.{t}"][0] + np.array([0, 0.06, -0.03])
            heel = np.maximum(heel, soft(dist_to(Pb, hc), 0.036, 0.040) * (z > sole_top + 0.004))
        strap = np.zeros(len(Pb))
        for t in ("L", "R"):  # Orion's ceramic instep strap (the sheet's front view)
            sy = J[f"Toe.{t}"][0][1] + 0.060
            strap = np.maximum(strap, soft(np.abs(Pb[:, 1] - sy), 0.013, 0.016) * (1 - soft(z, sole_top + 0.022, sole_top + 0.026))
                               * (N[boot, 2] > -0.3))
        white = np.clip(cap + heel, 0, 1)
        if L.key == "nova":  # Nova's boots: pearl ceramic shells over a graphite sole and ankle band
            white = np.maximum(white, (1 - soft(z, sole_top + 0.012, sole_top + 0.016)) * soft(z, 0.105, 0.11))
        else:
            white = np.maximum(white, strap)
        cb = np.tile(accent, (len(Pb), 1)) * (1 - white[:, None]) + armor * white[:, None]
        rubber = np.array([0.012, 0.012, 0.014])
        hb = sole * (tread * 0.0009 + ribs * 0.0003 * (N[boot, 2] > -0.3)) - np.exp(-((z - sole_top) / 0.0012) ** 2) * 0.0005
        hb += -np.exp(-((cap - 0.5) / 0.12) ** 2) * 0.0003 - np.exp(-((heel - 0.5) / 0.12) ** 2) * 0.0003
        h[boot] += hb
        col[boot] = cb * (1 - sole[:, None]) + rubber * sole[:, None]
        rough[boot] = (style.armor_rough + 0.30 * (1 - white)) * (1 - sole) + 0.85 * sole
        cavity[boot] -= 0.3 * np.exp(-((z - sole_top) / 0.0015) ** 2)
        sole_line = np.exp(-((z - sole_top - 0.006) / 0.0008) ** 2) * (N[boot, 2] > -0.3)
        em[boot] += sole_line[:, None] * glow * 0.8

    # Machined hardware: ear pods, the back core and the chest emblem.
    hw = pod | core | emblem
    if hw.any():
        brushed = fbm(P[hw] * np.array([1.0, 1.0, 30.0]), 500, 2, seed + 14)
        col[hw] = metal_c * (1 + 0.06 * brushed[:, None])
        rough[hw] = 0.26 + 0.06 * brushed
        metal[hw] = 1.0
    if pod.any():
        rel = P[pod] - centre[pod]
        side = np.sign(centre[pod, 0])
        outer = (N[pod, 0] * side) > 0.7
        rr = np.linalg.norm(rel[:, 1:], axis=1)
        # Per piece (the ear pods and the core housing collar share this kind).
        key = np.round(centre[pod] * 1000).astype(np.int64)
        _, gi = np.unique(key, axis=0, return_inverse=True)
        R = np.zeros(gi.max() + 1)
        np.maximum.at(R, gi.reshape(-1), rr)
        R = R[gi.reshape(-1)]
        ring = outer * np.exp(-((rr - R * 0.62) / 0.0013) ** 2)
        cap = outer * (rr < R * 0.5)
        h[pod] += -ring * 0.0004 + cap * 0.0003
        em[pod] += ring[:, None] * glow * 1.4
        col[pod] = col[pod] * (1 - cap[:, None]) + accent * cap[:, None]
        metal[pod] *= 1 - cap
        rough[pod] = rough[pod] * (1 - cap) + 0.3 * cap
    for msk, face_sign in ((core, 1.0), (emblem, -1.0)):
        if not msk.any():
            continue
        rel = P[msk] - centre[msk]
        face = (N[msk, 1] * face_sign) > 0.6
        rr = np.linalg.norm(rel[:, [0, 2]], axis=1)
        R = rr.max()
        ang = np.arctan2(rel[:, 2], rel[:, 0])
        if face_sign > 0:  # the core: three concentric light rings, segmented, a bright centre cell
            rings = np.zeros(len(rr))
            for fr, w in ((0.30, 0.05), (0.55, 0.035), (0.76, 0.03)):
                rings = np.maximum(rings, np.exp(-((rr / R - fr) / w) ** 2))
            seg = (0.5 + 0.5 * np.cos(ang * 12)) > 0.15
            glow_m = face * (rings * np.where(rr / R > 0.45, seg, 1.0) + (rr / R < 0.16) * 1.0)
            dark = (face & (rr / R < 0.86)).astype(float)
        else:  # the emblem: a hexagonal light ring around a dark lens
            hexr = rr * np.cos((ang % (np.pi / 3)) - np.pi / 6) / np.cos(np.pi / 6)
            glow_m = face * (np.exp(-((hexr / R - 0.6) / 0.08) ** 2) + (hexr / R < 0.22))
            dark = (face & (hexr / R < 0.8)).astype(float)
        glow_m = np.clip(np.asarray(glow_m, float), 0, 1)
        h[msk] += -dark * 0.0006 + glow_m * 0.0002
        cm = col[msk] * (1 - dark[:, None]) + np.array([0.01, 0.012, 0.016]) * dark[:, None]
        col[msk] = cm * (1 - glow_m[:, None]) + glow * 0.5 * glow_m[:, None]
        metal[msk] *= 1 - dark
        rough[msk] = rough[msk] * (1 - dark) + 0.12 * dark
        em[msk] += glow_m[:, None] * glow * 2.0

    # Helmet: a crest ridge from the brow to the nape, graphite cheek panels behind the visor and
    # vent slots above the ear pods.
    if helmet.any():
        Ph = P[helmet]
        c = L.head.c
        rel = Ph - c
        r = np.linalg.norm(rel, axis=1)
        d = rel / r[:, None]
        az = np.degrees(np.arctan2(np.abs(d[:, 0]), -d[:, 1]))  # 0 front, 90 side, 180 back
        el = np.degrees(np.arcsin(np.clip(d[:, 2], -1, 1)))
        eh = e[helmet]
        crest = np.exp(-(np.abs(Ph[:, 0]) / 0.009) ** 4) * (el > 12)
        crest_edge = np.exp(-((np.abs(Ph[:, 0]) - 0.0105) / 0.0008) ** 2) * (el > 12)
        vents = band(az, 100, 124, 1.0) * band(el, 16, 26, 1.0) * ((0.5 + 0.5 * np.cos(np.radians(az - 100) * 2 * np.pi / np.radians(6))) > 0.55)
        seam = np.exp(-((el - (10 - 0.18 * np.abs(az - 120))) / 0.5) ** 2) * band(az, 70, 175, 3) * (eh > 0.01)
        h[helmet] += crest * 0.0010 - crest_edge * 0.0004 - vents * 0.0008 - seam * 0.0004
        col[helmet] *= (1 - vents[:, None] * 0.85) * (1 - 0.5 * seam[:, None]) * (1 - 0.4 * crest_edge[:, None])
        cavity[helmet] -= crest_edge * 0.35 + vents * 0.5 + seam * 0.4
        # Side panels: brushed-metal covers around the ear pods, sweeping back toward the nape, framed by a recessed seam.
        dbox = 36.0 * ((np.abs((az - 102) / 44) ** 4 + np.abs((el + 6) / 36) ** 4) ** 0.25 - 1.0)
        tp = soft(dbox, -1.2, 0.0) * (eh > 0.004)
        tseam = np.exp(-(dbox / 0.7) ** 2) * (eh > 0.004)
        brushed = fbm(Ph * np.array([1.0, 1.0, 30.0]), 500, 2, seed + 15)
        tcol = metal_c[None] * (0.30 + 0.04 * brushed)[:, None]
        col[helmet] = col[helmet] * (1 - tp[:, None]) + tcol * tp[:, None]
        metal[helmet] = np.maximum(metal[helmet], tp * 0.5)
        rough[helmet] = rough[helmet] * (1 - tp) + (0.40 + 0.05 * brushed) * tp
        h[helmet] += -tseam * 0.0005 - tp * 0.0003
        col[helmet] *= (1 - 0.6 * tseam[:, None])
        cavity[helmet] -= tseam * 0.45

    # The sheet's face frame: a band of graphite composite around the visor opening, sweeping
    # back under the ear pods into the side panels.
    if helmet.any() and L.visor_plate is not None:
        Ph = P[helmet]
        gv = L.visor_plate.g(Ph)
        rel = Ph - L.head.c
        frame_w = 0.017 + 0.012 * np.clip(-rel[:, 2] / 0.05, 0, 1)  # wider toward the chin and cheeks
        fr = soft(gv - frame_w, 0.0, 0.0015) * (gv < 0.5) * (e[helmet] > 0.002)
        col[helmet] = col[helmet] * (1 - fr[:, None]) + accent * 1.6 * fr[:, None]
        rough[helmet] = rough[helmet] * (1 - fr) + (style.armor_rough + 0.14) * fr
        fseam = np.exp(-((gv - frame_w - 0.0007) / 0.0007) ** 2) * (gv < 0.5)
        h[helmet] += -fseam * 0.0004 + fr * 0.0002
        cavity[helmet] -= fseam * 0.4
        # Machined panel seams over the crown (the sheet's segmented helmet shell).
        az = np.degrees(np.arctan2(rel[:, 0], -rel[:, 1]))
        el = np.degrees(np.arcsin(np.clip(rel[:, 2] / np.linalg.norm(rel, axis=1), -1, 1)))
        seams = (np.exp(-((np.abs(az) - 38) / 0.9) ** 2) * band(el, 28, 75, 4)
                 + np.exp(-((el - 30 - 0.08 * np.abs(az)) / 0.7) ** 2) * band(np.abs(az), 38, 150, 6))
        seams = np.clip(seams, 0, 1) * (1 - fr) * (gv > 0.004)
        h[helmet] -= seams * 0.0004
        col[helmet] *= (1 - 0.55 * seams[:, None])
        cavity[helmet] -= seams * 0.45

    # Collar: a ribbed band (the sheet's high collar), the ribs a few millimetres apart with a
    # smooth rolled top.
    if collar.any():
        zc = P[collar, 2]
        rib = (0.5 + 0.5 * np.cos(2 * np.pi * zc / 0.0042)) ** 2 * soft(e[collar], 0.004, 0.007)
        h[collar] += rib * 0.00035
        cavity[collar] -= (1 - rib) * 0.25 * soft(e[collar], 0.004, 0.007)
        rough[collar] = rough[collar] + 0.10 * (1 - rib)

    # The sheet's chest emblem: a V in brushed steel standing proud of the breastplate (Orion), a
    # small one at the top of the sternum seam (Nova).
    zt = lambda t_: J["Hips"][0][2] + (J["Neck"][0][2] - J["Hips"][0][2]) * t_  # noqa: E731
    if L.key == "nova":
        vz, vw, vh, vt = zt(0.875), 0.040, 0.032, 0.0085
    else:
        vz, vw, vh, vt = zt(0.80), 0.050, 0.044, 0.0105
    fr_ = (plate & (N[:, 1] < -0.35) & (np.abs(P[:, 0]) < vw + 0.01) & (np.abs(P[:, 2] - vz) < vh)).astype(float)
    if fr_.any():
        x_ = np.abs(P[:, 0])
        zz = P[:, 2] - (vz - vh * 0.5)  # 0 at the V's point
        # Distance to the stroke from the point (0, 0) to the arm tip (vw, vh).
        ax_ = np.array([vw, vh]) / np.hypot(vw, vh)
        rel_ = np.stack([x_, zz], 1)
        tt_ = np.clip(rel_ @ ax_, 0, np.hypot(vw, vh))
        dd = np.linalg.norm(rel_ - tt_[:, None] * ax_[None], axis=1)
        vmask = soft(dd, vt * 0.5 - 0.0006, vt * 0.5) * fr_ * (zz > -0.002)
        bev = np.exp(-((dd - vt * 0.5) / 0.0007) ** 2) * fr_
        brushed_v = fbm(P * np.array([1.0, 1.0, 40.0]), 600, 2, seed + 17)
        if L.key == "nova":  # a black V on the white plate
            vcol = accent * (1.0 + 0.05 * brushed_v[:, None])
            vrough, vmetal = 0.25, 0.0
        else:  # brushed silver on the black plate
            # (Satin, half-metallic: it reads silver under any light, not only in reflections.)
            vcol = np.array([0.72, 0.73, 0.75]) * (1.0 + 0.06 * brushed_v[:, None])
            vrough, vmetal = 0.38 + 0.05 * brushed_v, 0.5
        col = col * (1 - vmask[:, None]) + vcol * vmask[:, None]
        metal = np.maximum(metal, vmask * vmetal)
        rough = rough * (1 - vmask) + vrough * vmask
        h += vmask * 0.0008 - bev * 0.0002
        cavity -= bev * 0.4

    if L.key == "nova":
        # The sheet's small "NOVA" lettering under the V (ADR-0252): a stroke font painted black.
        th_, lw_, gap_ = 0.0095, 0.0068, 0.0032  # letter height, width, spacing (metres)
        glyphs = {"N": [((0, 0), (0, 1)), ((0, 1), (1, 0)), ((1, 0), (1, 1))],
                  "O": [((0, 0), (1, 0)), ((1, 0), (1, 1)), ((1, 1), (0, 1)), ((0, 1), (0, 0))],
                  "V": [((0, 1), (0.5, 0)), ((0.5, 0), (1, 1))],
                  "A": [((0, 0), (0.5, 1)), ((0.5, 1), (1, 0)), ((0.22, 0.42), (0.78, 0.42))]}
        tw_ = 4 * lw_ + 3 * gap_
        tz0 = vz - vh * 0.5 - 0.010 - th_
        tsel = plate & (N[:, 1] < -0.35) & (np.abs(P[:, 0]) < tw_) & (np.abs(P[:, 2] - tz0 - th_ * 0.5) < th_)
        if tsel.any():
            Q = np.stack([P[tsel, 0], P[tsel, 2]], 1)
            dmin = np.full(len(Q), 1.0)
            for i, ch in enumerate("NOVA"):
                # She faces -Y, so the viewer's left is -X: letters run from -X to +X.
                x0 = -tw_ * 0.5 + i * (lw_ + gap_)
                for (ax0, az0), (ax1, az1) in glyphs[ch]:
                    a0 = np.array([x0 + ax0 * lw_, tz0 + az0 * th_])
                    a1 = np.array([x0 + ax1 * lw_, tz0 + az1 * th_])
                    dv = a1 - a0
                    u = np.clip(((Q - a0) @ dv) / (dv @ dv), 0, 1)
                    dmin = np.minimum(dmin, np.linalg.norm(Q - a0 - u[:, None] * dv[None], axis=1))
            tm = np.zeros(len(P))
            tm[tsel] = soft(dmin, 0.00085 - 0.0004, 0.00085)
            col = col * (1 - tm[:, None]) + accent * tm[:, None]
            rough = rough * (1 - tm) + 0.3 * tm

    # Ceramic micro-surface: faint orange peel in the clear coat, slight roughness variation.
    h += shell * fine * 0.000004
    col = col * (1 + 0.025 * mottle[:, None] * shell[:, None])
    rough += 0.03 * mottle
    # Rolled lips: tucked under, darker; the helmet's visor rim glows.
    lip_glow = (cuff > 0.5) & helmet
    col = col * (1 - 0.45 * cuff[:, None])
    cavity = cavity * (1 - 0.35 * cuff)
    if lip_glow.any():
        vz = L.visor[0]
        rim = lip_glow & (P[:, 2] > vz - 0.01) & (P[:, 1] < L.head.c[1])
        em[rim] += glow * 0.55
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": np.clip(metal, 0, 1),
            "height": h, "cavity": np.clip(cavity, 0, 1), "emissive": np.clip(em, 0, 1)}


def visor_maps(tx, P, N, L: Landmarks, style, kind, cuff, seed, attrs=None):
    n = len(P)
    k = np.rint(kind).astype(np.int64)
    glass = k == 0
    gl = ~glass
    glow = np.array(style.glow)
    col = np.tile(np.array(style.visor), (n, 1))
    rough = np.full(n, 0.05)
    metal = np.zeros(n)
    h = np.zeros(n)
    cavity = np.ones(n)
    em = np.zeros((n, 3))
    # Glass: a faint scan-line grid that only shows in the reflection (roughness), a metal rim.
    scan = (0.5 + 0.5 * np.cos(2 * np.pi * P[:, 2] / 0.0016)) ** 8
    rough[glass] = 0.05 + 0.05 * scan[glass]
    rim = cuff > 0.5
    col[rim] = np.array(style.metal) * 0.6
    metal[rim] = 1.0
    rough[rim] = 0.3
    # Smoked glass lit from its edge: a light guide in the rim bleeds a thin tinted glow inward,
    # while the centre stays near-black so the display glyphs read.
    if L.visor_plate is not None:
        gv = L.visor_plate.g(P)
        depth = np.clip(-gv, 0, None)
        inner = np.exp(-depth / 0.005) * (glass & ~rim)
        edge = glow * style.trim_light
        col += np.outer(inner, edge) * 0.02
        em += np.outer(inner ** 2, edge) * 0.10
        # A faint face-shaped tint band: the display substrate behind the glass.
        band = np.exp(-((depth - 0.010) / 0.002) ** 2) * (glass & ~rim)
        em += np.outer(band, edge) * 0.025
        # Face contour lines (the sheet's visor): thin light guides tracing the cheekbones and
        # the jaw inside the glass, fading out toward the brow.
        rel_ = P - L.head.c
        eye_z = L.J["Eye.L"][0][2]
        lowf = np.clip((eye_z - 0.006 - P[:, 2]) / 0.02, 0, 1)
        contour = np.exp(-((depth - 0.0055) / 0.0009) ** 2) * lowf * (np.abs(rel_[:, 0]) > 0.012)
        cheek = np.exp(-((np.abs(rel_[:, 0]) - 0.034) / 0.0010) ** 2) * np.clip((P[:, 2] - eye_z + 0.040) / 0.006 + 1, 0, 1) * np.clip((-0.012 - (P[:, 2] - eye_z)) / 0.006 + 1, 0, 1)
        lines = (glass & ~rim) * np.clip(contour + 0.0 * cheek, 0, 1)
        em += np.outer(lines, edge) * 0.22
        col += np.outer(lines, edge) * 0.05
        # The sheet's inner light: the tinted display substrate glows faintly through the whole
        # pane, brightest low and toward the edges, dark enough behind the glyphs to keep them legible.
        rel = P - L.head.c
        low = np.clip(0.5 - rel[:, 2] / 0.12, 0, 1)
        fill = (glass & ~rim) * (0.12 + 0.88 * np.exp(-depth / 0.014)) * (0.6 + 0.4 * low)
        col += np.outer(fill, glow) * 0.0015
        em += np.outer(fill, glow) * 0.0035
    # Glyphs: lit display segments. Eyes are hollow rounded rings around a dim cell (the sheet's
    # face display); the mouth is a solid bar.
    ring = np.ones(n)
    if attrs is not None and "gst" in attrs:
        g = attrs["gst"]
        eye_g = np.rint(g[:, 2]) == 1
        edge_d = np.minimum(g[:, 0], g[:, 1])
        r_ = 1 - soft(edge_d, 0.0011, 0.0019)  # 0 on the ring, 1 inside it
        ring = np.where(eye_g, 1.0 if style.solid_eyes else 1 - r_ * 0.985, 1.0 if style.mouth_glyph else 0.0)
    col[gl] = (glow * 0.6) * ring[gl, None] + np.array(style.visor) * (1 - ring[gl, None])
    em[gl] = glow * ring[gl, None]
    rough[gl] = 0.2
    return {"color": np.clip(col, 0, 1), "rough": rough, "metal": metal, "height": h, "cavity": cavity,
            "emissive": np.clip(em, 0, 1)}
