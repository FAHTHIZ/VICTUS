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
from bake import band, dist_to, fbm, soft, worley


class Landmarks:
    """The landmarks sockets, hand axes and the bakes need (the mannequin has no eyes or mouth)."""

    def __init__(self, body: fit.Body, info):
        J = {n: (np.asarray(h), np.asarray(t)) for n, _, h, t in body.joints}
        self.J = J
        self.head = info["head"]
        self.visor = info["visor"]
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
    glow = np.array(style.glow)
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
    zones.append(flex(neck, headj, 0.45, 0.05, 0.005, 0.09))  # neck
    zone_any = np.zeros(n)
    for r, z in zones:
        ribs = np.maximum(ribs, r)
        zone_any = np.maximum(zone_any, z)
    zb = zone_any * body
    h += zb * (ribs ** 1.5 * 0.0006 - 0.0002)
    col = col * (1 - 0.25 * zb[:, None]) + (base * 0.6 + np.array(style.metal) * 0.05) * 0.25 * zb[:, None]
    rough += zb * (0.12 * (1 - ribs) - 0.05)
    cavity -= zb * 0.35 * (1 - ribs) ** 2

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

    # Composite segmentation: the undersuit is built from moulded panels elongated along the
    # body, separated by fine recessed seams; alternate panels have a slightly glossier finish.
    seg_b, seg_v = cells(P, 0.055, seed + 31, stretch=(1.0, 1.0, 0.55))
    seam = np.exp(-(seg_b / 0.0011) ** 2) * body * (1 - zone_any) * (1 - pb)
    h -= seam * 0.00030
    cavity -= seam * 0.45
    col = col * (1 - 0.35 * seam[:, None]) * (1 + 0.06 * (seg_v[:, None] - 0.5) * body[:, None])
    rough += body * (seg_v > 0.6) * -0.12 + seam * 0.1

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

    rough += 0.04 * mottle
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": metal,
            "height": h, "cavity": np.clip(cavity, 0, 1), "emissive": np.clip(em, 0, 1)}


def armor_maps(tx, P, N, L: Landmarks, style, kind, cuff, trim, attrs, seed):
    n = len(P)
    J = L.J
    k = np.rint(kind).astype(np.int64)
    plate, boot, pod, core, helmet = (k == 0), (k == 1), (k == 2), (k == 3), (k == 4)
    trim = np.rint(trim).astype(np.int64)
    e = attrs["edge"][:, 0]
    centre = attrs["centre"]
    armor = np.array(style.armor)
    accent = np.array(style.accent)
    metal_c = np.array(style.metal)
    glow = np.array(style.glow)
    col = np.tile(armor, (n, 1))
    rough = np.full(n, style.armor_rough)
    metal = np.zeros(n)
    h = np.zeros(n)
    cavity = np.ones(n)
    em = np.zeros((n, 3))
    mottle = fbm(P, 22, 3, seed + 11)
    fine = fbm(P, 700, 2, seed + 12)

    shell = plate | boot | helmet
    # Rounded bevel along the cut edge (the lip rolls under), an inset groove 1 cm in.
    bevel = np.exp(-(e / 0.0035) ** 2)
    groove_at = np.where(boot, 0.007, 0.0095)
    groove = np.exp(-((e - groove_at) / 0.0007) ** 2) * shell * (cuff < 0.5)
    h += shell * (-bevel * 0.0009) - groove * 0.00045
    cavity -= groove * 0.55 + shell * bevel * 0.12
    col = col * (1 - 0.55 * groove[:, None])
    rough += groove * 0.2
    lit = groove * (trim > 0)
    em += lit[:, None] * glow * 1.0
    col = col * (1 - lit[:, None]) + glow * 0.35 * lit[:, None]

    # Accent field inside the groove on the larger plates: a contrasting panel stripe.
    panel = shell * (trim > 0) * (e > groove_at + 0.0045) * (cuff < 0.5)
    stripe = np.zeros(n)
    for t, s in (("L", 1.0), ("R", -1.0)):
        for a, b in ((J[f"UpperLeg.{t}"][0], J[f"LowerLeg.{t}"][0]), (J[f"LowerLeg.{t}"][0], J[f"Foot.{t}"][0]),
                     (J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0])):
            d = _unit(b - a)
            outer = _unit(np.cross([0, 1.0, 0], d))
            if outer[0] * s < 0:
                outer = -outer
            perp = _unit(np.cross(d, outer))
            tt, radial = _seg(P, a, b)
            # A stripe down the front of the limb plate.
            w = np.abs(radial @ outer)
            front = radial @ perp
            fwd = (front * (1 if perp[1] < 0 else -1)) > 0
            stripe = np.maximum(stripe, (tt > 0) & (tt < 1) & fwd & (w < 0.009))
    stripe = soft(1 - stripe.astype(float), 0.0, 0.5) * panel
    col = col * (1 - stripe[:, None]) + accent * stripe[:, None]
    rough += stripe * 0.08

    # Chest emblem: a chevron at the sternum (raised, with a light line in its groove).
    chest = (trim == 2) & shell
    if chest.any():
        hips, neck = J["Hips"][0], J["Neck"][0]
        zc = hips[2] + (neck[2] - hips[2]) * 0.74
        u = np.abs(P[:, 0])
        v = P[:, 2] - zc
        front = N[:, 1] < -0.3
        chev = np.abs(v + u * 0.75) < 0.0045
        chev = chev & (u < 0.034) & front & chest
        outline = (np.abs(np.abs(v + u * 0.75) - 0.0045) < 0.0009) & (u < 0.0345) & front & chest
        h += chev * 0.0007 - outline * 0.0003
        col = col * (1 - chev[:, None]) + accent * chev[:, None]
        em += outline[:, None] * glow * 1.2
        cavity -= outline * 0.3

    # Boots: moulded sole with lugs, a toe cap in the accent colour, an ankle strap.
    if boot.any():
        Pb = P[boot]
        z = Pb[:, 2]
        sole_top = 0.03
        sole = soft(z - sole_top, -0.0015, 0.0015)
        tread = np.clip((worley(Pb * np.array([1, 1, 0.0]), 0.008, seed + 13) - 0.4) * 4, 0, 1) * (N[boot, 2] < -0.3)
        ribs = 0.5 + 0.5 * np.sin(2 * np.pi * z / 0.0045)
        toe = np.zeros(len(Pb))
        for t in ("L", "R"):
            toe = np.maximum(toe, soft(dist_to(Pb, J[f"Toe.{t}"][0] + np.array([0, -0.02, 0.0])), 0.045, 0.05))
        hb = sole * (tread * 0.0009 + ribs * 0.0003 * (N[boot, 2] > -0.3)) - np.exp(-((z - sole_top) / 0.0012) ** 2) * 0.0005
        h[boot] += hb
        cb = col[boot] * (1 - toe[:, None]) + accent * toe[:, None]
        rubber = np.array([0.02, 0.02, 0.022])
        col[boot] = cb * (1 - sole[:, None]) + rubber * sole[:, None]
        rough[boot] = rough[boot] * (1 - sole) + 0.85 * sole
        cavity[boot] -= 0.3 * np.exp(-((z - sole_top) / 0.0015) ** 2)

    # Machined metal: ear pods (a light ring on the outer face), the back core (a glowing cell
    # behind vents).
    hw = pod | core
    if hw.any():
        brushed = fbm(P[hw] * np.array([1.0, 1.0, 30.0]), 500, 2, seed + 14)
        col[hw] = metal_c * (1 + 0.06 * brushed[:, None])
        rough[hw] = 0.28 + 0.06 * brushed
        metal[hw] = 1.0
    if pod.any():
        rel = P[pod] - centre[pod]
        side = np.sign(centre[pod, 0])
        outer = (N[pod, 0] * side) > 0.75
        rr = np.linalg.norm(rel[:, 1:], axis=1)
        ring = outer * np.exp(-((rr - 0.014) / 0.0012) ** 2)
        cap = outer * (rr < 0.0115)
        h[pod] += -ring * 0.0004 + cap * 0.0003
        em[pod] += ring[:, None] * glow * 1.4
        col[pod] = col[pod] * (1 - cap[:, None]) + armor * cap[:, None]
        metal[pod] *= 1 - cap
        rough[pod] = rough[pod] * (1 - cap) + style.armor_rough * cap
    if core.any():
        rel = P[core] - centre[core]
        back = N[core, 1] > 0.75
        win = back * band(np.abs(rel[:, 0]), -1, 0.032, 0.002) * band(np.abs(rel[:, 2]), -1, 0.052, 0.002)
        slats = 0.5 + 0.5 * np.cos(2 * np.pi * rel[:, 2] / 0.0085)
        vents = win * (slats > 0.45)
        h[core] += -win * 0.0006 + vents * 0.0005
        em[core] += (win * (1 - vents) * 2.0 + vents * 0.15)[:, None] * glow
        col[core] = col[core] * (1 - win[:, None]) + np.array([0.02, 0.02, 0.025]) * win[:, None]
        metal[core] *= 1 - 0.5 * win
        frame_bolt = back * (np.abs(np.abs(rel[:, 0]) - 0.044) < 0.004) & (np.abs(np.abs(rel[:, 2]) - 0.064) < 0.004)
        h[core] += frame_bolt * 0.0004
        cavity[core] -= frame_bolt * 0.2

    # Panel lines on the larger plates: fine recessed seams splitting each plate into moulded
    # sections, a few sections inset in the graphite accent, and small fasteners near the seams.
    inner = (plate | helmet) & (e > 0.016) & (cuff < 0.5)
    pb_, pv_ = cells(P, 0.10, seed + 41)
    pline = np.exp(-(pb_ / 0.0007) ** 2) * inner * soft(e, 0.016, 0.024)
    insert = inner * (pv_ < 0.16) * soft(pb_, 0.0016, 0.0026) * soft(e, 0.020, 0.026) * plate
    h += -pline * 0.00045 - insert * 0.0005
    cavity -= pline * 0.5
    col = col * (1 - 0.6 * pline[:, None])
    col = col * (1 - insert[:, None]) + accent * insert[:, None]
    rough += pline * 0.15 + insert * 0.12
    fb_, fv_ = cells(P, 0.022, seed + 43)
    del fb_
    fast_c = worley(P, 0.022, seed + 43) * 0.022
    fast = inner * (fv_ < 0.08) * np.exp(-(fast_c / 0.0016) ** 4) * (pb_ > 0.003)
    h += fast * 0.0003
    col = col * (1 - fast[:, None]) + metal_c * fast[:, None]
    metal = np.maximum(metal, fast)
    rough = rough * (1 - fast) + 0.3 * fast

    # Helmet: graphite side panels behind the visor (inset, with a light line along their front
    # edge), a raised crest ridge from the brow to the nape, and vent slots above the ear pods.
    if helmet.any():
        Ph, Nh = P[helmet], N[helmet]
        c = L.head.c
        rel = Ph - c
        r = np.linalg.norm(rel, axis=1)
        d = rel / r[:, None]
        az = np.degrees(np.arctan2(np.abs(d[:, 0]), -d[:, 1]))  # 0 front, 90 side, 180 back
        el = np.degrees(np.arcsin(np.clip(d[:, 2], -1, 1)))
        eh = e[helmet]
        side_panel = band(az, 74, 150, 3.0) * band(el, -40, 24 - 0.10 * np.abs(az - 110), 3.0) * (eh > 0.012)
        panel_edge = np.exp(-((az - 74) / 0.9) ** 2) * band(el, -38, 22, 3.0) * (eh > 0.012)
        crest = np.exp(-(np.abs(Ph[:, 0]) / 0.0075) ** 4) * (el > 8)
        crest_edge = np.exp(-((np.abs(Ph[:, 0]) - 0.0085) / 0.0008) ** 2) * (el > 8)
        vents = band(az, 96, 128, 1.0) * band(el, 18, 30, 1.0) * ((0.5 + 0.5 * np.cos(np.radians(az - 96) * 2 * np.pi / np.radians(6))) > 0.55)
        hh = side_panel * -0.0006 + crest * 0.0012 - crest_edge * 0.0004 - vents * 0.0009 - panel_edge * 0.0004
        h[helmet] += hh
        ch = col[helmet]
        ch = ch * (1 - side_panel[:, None]) + accent * side_panel[:, None]
        ch = ch * (1 - vents[:, None] * 0.8)
        col[helmet] = ch
        rough[helmet] += side_panel * 0.18 + crest_edge * 0.1
        cavity[helmet] -= crest_edge * 0.4 + panel_edge * 0.4 + vents * 0.5
        em[helmet] += (panel_edge * 1.1)[:, None] * glow
        del Nh

    # Ceramic micro-surface: faint orange peel in the clear coat, slight roughness variation.
    h += shell * fine * 0.000004
    col = col * (1 + 0.03 * mottle[:, None] * shell[:, None])
    rough += 0.03 * mottle
    # Rolled lips: tucked under, darker.
    col = col * (1 - 0.45 * cuff[:, None])
    cavity = cavity * (1 - 0.35 * cuff)
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": np.clip(metal, 0, 1),
            "height": h, "cavity": np.clip(cavity, 0, 1), "emissive": np.clip(em, 0, 1)}


def visor_maps(tx, P, N, L: Landmarks, style, kind, cuff, seed):
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
    # A dim HUD line just inside the top edge of the glass.
    top = glass & (np.abs(P[:, 2] - (L.visor[1] - 0.006)) < 0.0006) & (np.abs(P[:, 0]) < 0.035)
    em[top] = glow * 0.35
    # Glyphs: lit display segments.
    col[gl] = glow * 0.6
    em[gl] = glow
    rough[gl] = 0.2
    return {"color": np.clip(col, 0, 1), "rough": rough, "metal": metal, "height": h, "cavity": cavity,
            "emissive": np.clip(em, 0, 1)}
