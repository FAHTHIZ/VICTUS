# SPDX-License-Identifier: Apache-2.0
"""Modular equipment for the VICTUS starter characters (docs/CHARACTERS.md#equipment, ADR-0185).

Boots, gloves and the belt are shells grown from the surface they cover (the suit over the feet
and waist, the bare skin over the hands): the covered region is cut out along planes, every vertex
is pushed out along the rest normal by the layer's thickness and keeps the skin weights of the
vertex it came from, and the cut edge rolls back under itself (a tucked lip) the way the suit's
cuffs do. Because shell and surface share weights and topology, they deform together in every pose
and the shell cannot sink into what it covers; the covered surface is kept, so each item can be
hidden on its own. The backpack is a rounded box bound rigidly to Chest, placed behind the furthest
point of the back it spans.

All four are one "Gear" part (one material and texture set); the exporter writes each item as its
own mesh node (<Name>_Boots, _Gloves, _Belt, _Backpack) so scenes and scripts can toggle them.
"""
from __future__ import annotations

import numpy as np

from meshops import Quads, repack_islands, triangulate, vertex_normals

KINDS = ("Boots", "Gloves", "Belt", "Backpack")
# Face kinds per exported item (the pack's shoulder straps are kind 4, exported with the pack).
ITEM_KINDS = ((0,), (1,), (2,), (3, 4))


GLOVE_UNDER_SLEEVE = 0.03  # glove cuff starts this far up the forearm from the wrist


def boot_heights(J):
    ankle = 0.5 * (J["Foot.L"][0][2] + J["Foot.R"][0][2])
    return ankle, ankle + 0.10


def belt_heights(J):
    z = J["Hips"][0][2]
    return z + 0.04, z + 0.085


def covered(J, kind, P):
    """Mask of the points of the covered surface well inside item `kind` (away from its edges)."""
    if kind == 0:
        return P[:, 2] < boot_heights(J)[1] - 0.01
    if kind == 2:
        z0, z1 = belt_heights(J)
        return (P[:, 2] > z0 + 0.006) & (P[:, 2] < z1 - 0.006) & (np.abs(P[:, 0]) < 0.2)
    if kind == 1:
        m = np.zeros(len(P), bool)
        for t in ("L", "R"):
            wr, el = J[f"Hand.{t}"][0], J[f"LowerArm.{t}"][0]
            d = (wr - el) / np.linalg.norm(wr - el)
            m |= ((P - (wr - d * GLOVE_UNDER_SLEEVE)) @ d > 0.01) & (np.linalg.norm(P - wr, axis=1) < 0.3)
        return m
    return np.zeros(len(P), bool)


def pack_contact(gear_pos, gear_tri, gear_tri_kind, suit_pos, k=24):
    """Signed clearance of the back under the backpack (must stay negative: outside the pack)."""
    tri = gear_tri[gear_tri_kind == 3]
    pv = gear_pos[np.unique(tri)]
    lo, hi = pv.min(0), pv.max(0)
    m = (suit_pos[:, 1] > 0) & (suit_pos[:, 0] > lo[0] + 0.02) & (suit_pos[:, 0] < hi[0] - 0.02) & \
        (suit_pos[:, 2] > lo[2] + 0.02) & (suit_pos[:, 2] < hi[2] - 0.02)
    return _signed(gear_pos, tri, suit_pos[m], k)


def closest_on_triangles(p, a, b, c):
    """Closest points on triangles (a, b, c) to points p, all (n, 3) (Ericson, RTCD 5.1.5)."""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = p - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = p - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    den = np.where(np.abs(va + vb + vc) > 1e-30, va + vb + vc, 1e-30)
    v = vb / den
    w = vc / den
    out = a + ab * v[:, None] + ac * w[:, None]
    # Edge and vertex regions.
    m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
    t = d1 / np.where(d1 - d3 != 0, d1 - d3, 1e-30)
    out = np.where(m[:, None], a + ab * t[:, None], out)
    m2 = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
    t = d2 / np.where(d2 - d6 != 0, d2 - d6, 1e-30)
    out = np.where(m2[:, None], a + ac * t[:, None], out)
    m3 = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0)
    t = (d4 - d3) / np.where((d4 - d3) + (d5 - d6) != 0, (d4 - d3) + (d5 - d6), 1e-30)
    out = np.where(m3[:, None], b + (c - b) * t[:, None], out)
    out = np.where(((d1 <= 0) & (d2 <= 0))[:, None], a, out)
    out = np.where(((d3 >= 0) & (d4 <= d3))[:, None], b, out)
    out = np.where(((d6 >= 0) & (d5 <= d6))[:, None], c, out)
    return out


def clearance(J, gear_pos, gear_tri, gear_tri_kind, surf_pos, kind, k=24):
    """Signed clearance (metres, > 0 = the covered surface stays inside the item) of every covered
    surface point under item `kind`: distance to the closest point of the item's triangles,
    signed by that triangle's outward normal."""
    m = covered(J, kind, surf_pos)
    tri = gear_tri[gear_tri_kind == kind]
    if not m.any() or not len(tri):
        return np.zeros(0)
    return _signed(gear_pos, tri, surf_pos[m], k)


def _signed(gear_pos, tri, q, k):
    from scipy.spatial import cKDTree

    A, B, C = gear_pos[tri[:, 0]], gear_pos[tri[:, 1]], gear_pos[tri[:, 2]]
    fn = np.cross(B - A, C - A)
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-20)
    _, cand = cKDTree((A + B + C) / 3).query(q, k=min(k, len(tri)))
    D = np.zeros(cand.shape)
    S = np.zeros(cand.shape)
    for j in range(cand.shape[1]):
        t = cand[:, j]
        cp = closest_on_triangles(q, A[t], B[t], C[t])
        D[:, j] = np.linalg.norm(q - cp, axis=1)
        S[:, j] = -np.sign(((q - cp) * fn[t]).sum(1))
    i = D.argmin(1)
    rows = np.arange(len(q))
    best, sign = D[rows, i], np.where(S[rows, i] == 0, 1, S[rows, i])
    # A point in a crease between two parts of the item (between gloved fingers) lies outside
    # one shell and inside the facing one: it is covered when a facing triangle about as close
    # has it on the inner side.
    n_best = fn[cand[rows, i]]
    opp = (fn[cand] * n_best[:, None, :]).sum(2) < -0.3
    inner = opp & (S > 0) & (D < best[:, None] + 0.002)
    sign = np.where((sign < 0) & inner.any(1), 1.0, sign)
    return best * sign


def shell(q: Quads, nrm, faces, planes, off, off_edge, tuck, floor=None):
    """Shell over q's `faces`: returns (Quads, src vertex index into q per shell vertex, per-face
    lip label). `planes` are (origin, normal) pairs bounding the region; boundary vertices snap
    onto the nearest one. `floor` clamps shell z from below (boot soles)."""
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
    if planes:
        d = np.stack([np.abs((base - o) @ n) for o, n in planes], 1)
        which = d.argmin(1)
        for i, (o, n) in enumerate(planes):
            m = bmask & (which == i)
            base[m] -= ((base[m] - o) @ n)[:, None] * n
    offs = np.where(bmask, off_edge, off)
    pos = base + nrm[used] * offs[:, None]
    if floor is not None:
        pos[:, 2] = np.maximum(pos[:, 2], floor)
    bverts = np.unique(edges[:, :2])
    low = -np.ones(len(q.v), np.int64)
    low[bverts] = len(pos) + np.arange(len(bverts))
    lip = base[remap[bverts]] + nrm[bverts] * tuck
    pos = np.concatenate([pos, lip])
    src = np.concatenate([used, bverts])
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
    return out, src, label


def _hull(points, off):
    from scipy.spatial import ConvexHull

    eq = ConvexHull(points).equations.copy()
    eq[:, 3] -= off  # dilate
    return eq


def _ray_exit(eq, c, d):
    """Distance along unit directions d from the interior point c to the boundary of the convex
    region {x : eq[:, :3] @ x + eq[:, 3] <= 0}."""
    nd = d @ eq[:, :3].T
    s = -(eq[:, :3] @ c + eq[:, 3])  # > 0 inside
    t = np.where(nd > 1e-12, s[None, :] / np.where(nd > 1e-12, nd, 1), np.inf)
    return t.min(1)


def cube_sphere(n):
    """Unit directions on a quad cube-sphere (n x n quads per side, welded, one UV island per side)."""
    q = rounded_box(np.zeros(3), (1.0, 1.0, 1.0), 0.0, n=n)
    # Equal-angle warp of the cube grid, then onto the sphere.
    v = np.tan(q.v * (np.pi / 4))
    v /= np.linalg.norm(v, axis=1, keepdims=True)
    return Quads(v, q.f, q.uv, q.fuv)


BOOT_GRID = 22
BOOT_OFFSET = 0.005
BOOT_FOOT_OFFSET = 0.013  # chunky armoured boots: the foot hull stands well off the suit


def boot(J, t, suit_v, suit_n):
    """A boot as the radial graph, about a point inside the ankle, of the union of two dilated
    convex hulls: the foot (sole, toe box and heel) and the lower leg up to the boot top. A union
    of convex sets that share an interior point is star-shaped about it, so the radial graph is a
    fold-free closed surface that contains the whole covered foot (toes included) by construction.
    Relaxed with Laplacian steps re-projected radially, cut at the boot top and closed by a lip
    that tucks onto the leg. Returns (Quads, per-face lip label, nearest suit vertex per vertex)."""
    from scipy.spatial import cKDTree

    ankle, top = boot_heights(J)
    fx = J[f"Foot.{t}"][0][0]
    side = np.sign(suit_v[:, 0]) == np.sign(fx)
    foot_pts = suit_v[side & (suit_v[:, 2] < ankle + 0.05)]
    leg_pts = suit_v[side & (suit_v[:, 2] > ankle - 0.01) & (suit_v[:, 2] < top + 0.02)]
    H1, H2 = _hull(foot_pts, BOOT_FOOT_OFFSET), _hull(leg_pts, BOOT_OFFSET + 0.004)
    ring = leg_pts[np.abs(leg_pts[:, 2] - (ankle + 0.02)) < 0.01]
    c = np.array([ring[:, 0].mean(), ring[:, 1].mean(), ankle + 0.02])
    sph = cube_sphere(BOOT_GRID)
    e = np.concatenate([sph.f[:, [0, 1]], sph.f[:, [1, 2]], sph.f[:, [2, 3]], sph.f[:, [3, 0]]])
    e = np.unique(np.sort(e, 1), axis=0)
    deg = np.bincount(e.reshape(-1), minlength=len(sph.v)).astype(float)

    def project(d):
        d = d / np.linalg.norm(d, axis=1, keepdims=True)
        r = np.maximum(_ray_exit(H1, c, d), _ray_exit(H2, c, d))
        return c + d * r[:, None]

    p = project(sph.v)
    for _ in range(30):  # spread the samples evenly over the surface
        acc = np.zeros_like(p)
        np.add.at(acc, e[:, 0], p[e[:, 1]])
        np.add.at(acc, e[:, 1], p[e[:, 0]])
        p = project(p + 0.5 * (acc / deg[:, None] - p) - c)
    q = _orient_outward(Quads(p, sph.f, sph.uv, sph.fuv), c)
    keep = np.where(q.v[q.f].mean(1)[:, 2] < top)[0]
    nrm = vertex_normals(q.v, triangulate(q.f))
    q, src, lab = shell(q, nrm, keep, [(np.array([0, 0, top]), np.array([0, 0, 1.0]))], 0.0, 0.0, 0.0)
    q.v[:, 2] = np.maximum(q.v[:, 2], -0.0035)
    tree = cKDTree(suit_v)
    lip = np.unique(q.f[lab == 1][:, 2:4])
    _, nl = tree.query(q.v[lip], k=1)
    q.v[lip] = suit_v[nl] + suit_n[nl] * 0.0008
    q.v[lip, 2] = top
    _, near = tree.query(q.v, k=1)
    return q, lab, near


def rounded_box(center, half, radius, n=10):
    """Closed quad mesh of a box with rounded edges (welded corners, one UV island per side)."""
    t = np.linspace(-1.0, 1.0, n + 1)
    pts, faces, uvs, fuvs = [], [], [], []
    sides = [(0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)]
    for axis, sgn in sides:
        a, b = [k for k in range(3) if k != axis]
        if sgn < 0:
            a, b = b, a
        U, V = np.meshgrid(t, t, indexing="ij")
        s = np.zeros((n + 1, n + 1, 3))
        s[..., axis] = sgn
        s[..., a] = U
        s[..., b] = V
        base = len(pts) and sum(len(p) for p in pts)
        pts.append(s.reshape(-1, 3))
        ub = sum(len(u) for u in uvs)
        uvs.append(np.stack([(U.reshape(-1) + 1) / 2, (V.reshape(-1) + 1) / 2], 1))
        idx = np.arange((n + 1) ** 2).reshape(n + 1, n + 1)
        quad = np.stack([idx[:-1, :-1], idx[1:, :-1], idx[1:, 1:], idx[:-1, 1:]], -1).reshape(-1, 4)
        sp = s.reshape(-1, 3)
        nq = np.cross(sp[quad[0, 1]] - sp[quad[0, 0]], sp[quad[0, 3]] - sp[quad[0, 0]])
        if nq[axis] * sgn < 0:  # every side wound counter-clockwise seen from outside
            quad = quad[:, ::-1]
        faces.append(quad + base)
        fuvs.append(quad + ub)
    s = np.concatenate(pts)
    f = np.concatenate(faces)
    # Weld the shared cube-edge points.
    key = np.round(s * 1e5).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    s = s[first]
    f = inv.reshape(-1)[f]
    half = np.asarray(half, float)
    p = s * half
    inner = half - radius
    c = np.clip(p, -inner, inner)
    d = p - c
    ln = np.linalg.norm(d, axis=1, keepdims=True)
    p = np.where(ln > 1e-9, c + d / np.maximum(ln, 1e-9) * radius, p)
    return Quads(p + np.asarray(center, float), f, np.concatenate(uvs), np.concatenate(fuvs))


def _orient_outward(q: Quads, center):
    c = q.v[q.f].mean(1)
    n = vertex_normals(q.v, triangulate(q.f))
    fn = n[q.f].mean(1)
    if ((c - center) * fn).sum() < 0:
        q = Quads(q.v, q.f[:, ::-1].copy(), q.uv, q.fuv[:, ::-1].copy())
    return q


def build(J, joint_names, skin_q, skin_W, suit_q, suit_W, suit_nrm0):
    """The Gear part's geometry: (Quads, W, face kind, face lip label)."""
    pieces = []  # (Quads, W rows, kind index, lip label)
    # Boots: one closed shell per foot (boot()), weighted like the nearest suit point.
    ankle, top = boot_heights(J)
    suit_n = vertex_normals(suit_q.v, triangulate(suit_q.f))
    c = suit_q.v[suit_q.f].mean(1)
    for t in ("L", "R"):
        q, lab, near = boot(J, t, suit_q.v, suit_n)
        pieces.append((q, suit_W[near], 0, lab))
    # Gloves: the bare hand beyond a plane 3 cm up the forearm (under the sleeve), 1.8 mm thick.
    skin_n = vertex_normals(skin_q.v, triangulate(skin_q.f))
    cs = skin_q.v[skin_q.f].mean(1)
    for t in ("L", "R"):
        wr, el = J[f"Hand.{t}"][0], J[f"LowerArm.{t}"][0]
        d = (wr - el) / np.linalg.norm(wr - el)
        o = wr - d * GLOVE_UNDER_SLEEVE
        sel = np.where(((cs - o) @ d > 0) & (np.linalg.norm(cs - wr, axis=1) < 0.3))[0]
        if len(sel):
            q, src, lab = shell(skin_q, skin_n, sel, [(o, d)], 0.0018, 0.0018, 0.0003)
            pieces.append((q, skin_W[src], 1, lab))
    # Belt: a 4.5 cm band of the suit around the waist seam, 5 mm over the suit.
    hips = J["Hips"][0]
    z0, z1 = belt_heights(J)
    belt = np.where((c[:, 2] > z0) & (c[:, 2] < z1) & (np.abs(c[:, 0]) < 0.24))[0]
    if len(belt):
        planes = [(np.array([0, 0, z0]), np.array([0, 0, -1.0])), (np.array([0, 0, z1]), np.array([0, 0, 1.0]))]
        q, src, lab = shell(suit_q, suit_n, belt, planes, 0.005, 0.006, 0.0008)
        pieces.append((q, suit_W[src], 2, lab))
    # Backpack: behind the furthest point of the back it spans (authoring +Y is the back).
    chest = J["Chest"][0]
    hx, hy, hz = 0.135, 0.055, 0.155
    cz = chest[2] + 0.0
    # The pack leans on the back: its front plane is the line in the side profile (furthest back
    # point per height across the pack's width) that touches the back from behind with the least
    # mean gap, plus 8 mm of padding.
    zs = np.arange(cz - hz, cz + hz + 1e-6, 0.01)
    prof = []
    for z in zs:
        m = (np.abs(suit_q.v[:, 0]) < hx + 0.01) & (np.abs(suit_q.v[:, 2] - z) < 0.008) & (suit_q.v[:, 1] > 0)
        prof.append(suit_q.v[m][:, 1].max() if m.any() else -np.inf)
    prof = np.array(prof)
    ok = np.isfinite(prof)
    zs, prof = zs[ok], prof[ok]
    best = None
    for slope in np.linspace(-0.35, 0.35, 141):
        a0 = (prof - slope * (zs - cz)).max()
        gap = (a0 + slope * (zs - cz) - prof).mean()
        if best is None or gap < best[0]:
            best = (gap, slope, a0)
    _, slope, a0 = best
    ang = np.arctan(slope)
    center = np.array([0.0, a0 + 0.008 + hy / np.cos(ang), cz])
    q = _orient_outward(rounded_box(np.zeros(3), (hx, hy, hz), 0.03, n=8), np.zeros(3))
    ca, sa = np.cos(ang), np.sin(ang)
    R = np.array([[1, 0, 0], [0, ca, sa], [0, -sa, ca]])  # front face y = -hy + slope * z
    local = q.v
    v = local @ R.T + center
    # The back panel is padded and conforms: front vertices move onto the back's height field
    # (furthest back suit point within 2.5 cm across x and z), keeping the padding.
    from scipy.spatial import cKDTree

    sv = suit_q.v[(suit_q.v[:, 1] > 0) & (np.abs(suit_q.v[:, 0]) < hx + 0.05)]
    tree = cKDTree(sv[:, [0, 2]])
    w = np.clip(-local[:, 1] / hy, 0, 1) ** 2
    target = np.full(len(v), -np.inf)
    for i, nb in enumerate(tree.query_ball_point(v[:, [0, 2]], 0.025)):
        if nb:
            target[i] = sv[nb, 1].max()
    plane = a0 + slope * (v[:, 2] - cz)
    delta = np.where(np.isfinite(target), np.minimum(target - plane, 0.0), 0.0)
    e = np.concatenate([q.f[:, [0, 1]], q.f[:, [1, 2]], q.f[:, [2, 3]], q.f[:, [3, 0]]])
    e = np.unique(np.sort(e, 1), axis=0)
    deg = np.bincount(e.reshape(-1), minlength=len(v)).astype(float)
    for _ in range(6):
        acc = np.zeros(len(v))
        np.add.at(acc, e[:, 0], delta[e[:, 1]])
        np.add.at(acc, e[:, 1], delta[e[:, 0]])
        delta = np.maximum(0.5 * delta + 0.5 * acc / deg, delta)  # never deeper into the back
    v[:, 1] += w * delta
    q = Quads(v, q.f, q.uv, q.fuv)
    W = np.zeros((len(q.v), len(joint_names)))
    W[:, joint_names.index("Chest")] = 1.0
    pieces.append((q, W, 3, np.zeros(len(q.f), np.int32)))
    # Shoulder straps: 4 cm webbing shells over the shoulders, from behind the pack down the chest.
    zb = chest[2] - 0.11
    for s in (-1.0, 1.0):
        xs = s * 0.095
        sel = np.where((np.abs(c[:, 0] - xs) < 0.02) & (c[:, 2] > zb) & (c[:, 2] < hips[2] + 0.5) &
                       ((c[:, 1] < 0) | (c[:, 2] > cz + hz - 0.04)))[0]
        if len(sel):
            planes = [(np.array([xs - 0.02, 0, 0]), np.array([-1.0, 0, 0])), (np.array([xs + 0.02, 0, 0]), np.array([1.0, 0, 0])),
                      (np.array([0, 0, zb]), np.array([0, 0, -1.0]))]
            q, src, lab = shell(suit_q, suit_n, sel, planes, 0.0045, 0.0055, 0.0008)
            pieces.append((q, suit_W[src], 4, lab))

    # Merge.
    vs, fs, uvs, fuvs, Ws, kinds, labs = [], [], [], [], [], [], []
    nv = nu = 0
    for q, W, k, lab in pieces:
        vs.append(q.v)
        fs.append(q.f + nv)
        uvs.append(q.uv)
        fuvs.append(q.fuv + nu)
        Ws.append(W)
        kinds.append(np.full(len(q.f), k, np.int32))
        labs.append(lab)
        nv += len(q.v)
        nu += len(q.uv)
    gq = Quads(np.concatenate(vs), np.concatenate(fs), np.concatenate(uvs), np.concatenate(fuvs))
    gq = repack_islands(gq)
    return gq, np.concatenate(Ws), np.concatenate(kinds), np.concatenate(labs)
