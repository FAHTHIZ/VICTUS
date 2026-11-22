# SPDX-License-Identifier: Apache-2.0
"""Mesh operations for the VICTUS character pipeline (docs/CHARACTERS.md, ADR-0179). Pure NumPy/SciPy.

* `Quads`: a quad mesh with face-varying UVs (position index and UV index per corner), the form
  the MakeHuman topology arrives in.
* `catmull_clark`: one subdivision step expressed as a sparse linear operator S on vertex data,
  so positions, skin weights and blend-shape deltas all subdivide consistently (Catmull & Clark
  1978, with the usual boundary rules: boundary edges become midpoints, boundary vertices use the
  1-6-1 crease mask). UVs are interpolated linearly per face corner.
* `Renderable`: the GPU form (one vertex per unique position/UV pair, triangles), with smooth
  normals shared across UV seams and per-vertex tangents (Lengyel's method, orthonormalised,
  handedness in w), which is what the normal-map baker and the engine's TBN both use.
* `repack_islands`: rescales and shelf-packs UV islands into a new atlas, so a subset of the
  MakeHuman layout (the head and hands) can own a whole texture at higher texel density.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import scipy.sparse as sp


@dataclass
class Quads:
    v: np.ndarray  # (N, 3)
    f: np.ndarray  # (M, 4) position indices
    uv: np.ndarray  # (K, 2)
    fuv: np.ndarray  # (M, 4) uv indices

    def subset(self, faces):
        faces = np.asarray(faces)
        f = self.f[faces]
        fuv = self.fuv[faces]
        used = np.unique(f)
        remap = -np.ones(len(self.v), np.int64)
        remap[used] = np.arange(len(used))
        uused = np.unique(fuv)
        uremap = -np.ones(len(self.uv), np.int64)
        uremap[uused] = np.arange(len(uused))
        return Quads(self.v[used], remap[f], self.uv[uused], uremap[fuv]), used


def _edges(f):
    e = np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 3]], f[:, [3, 0]]])
    key = np.sort(e, axis=1)
    uniq, inv, counts = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    return uniq, inv.reshape(4, -1).T, counts  # inv[(face, k)] = edge of side k (k: 0-1,1-2,2-3,3-0)


def catmull_clark(q: Quads):
    """Returns (Quads, S): the subdivided mesh and the (N', N) operator mapping vertex data."""
    N, M = len(q.v), len(q.f)
    E, fe, ecount = _edges(q.f)
    nE = len(E)
    boundary_e = ecount == 1
    rows, cols, vals = [], [], []

    def add(r, c, w):
        rows.append(np.asarray(r).reshape(-1))
        cols.append(np.asarray(c).reshape(-1))
        vals.append(np.broadcast_to(np.asarray(w, float), np.asarray(r).shape).reshape(-1))

    # Face points (rows N + nE + face).
    fp_row = N + nE + np.arange(M)
    for k in range(4):
        add(fp_row, q.f[:, k], 0.25)
    # Edge points: interior (v0 + v1 + fpA + fpB) / 4, boundary midpoint.
    ep_row = N + np.arange(nE)
    w_end = np.where(boundary_e, 0.5, 0.25)
    add(ep_row, E[:, 0], w_end)
    add(ep_row, E[:, 1], w_end)
    for k in range(4):
        e = fe[:, k]
        inner = ~boundary_e[e]
        for j in range(4):
            add(ep_row[e[inner]], q.f[inner, j], 0.25 * 0.25)
    # Vertex points.
    valence = np.bincount(E.reshape(-1), minlength=N).astype(float)
    nfaces = np.bincount(q.f.reshape(-1), minlength=N).astype(float)
    on_boundary = np.zeros(N, bool)
    on_boundary[E[boundary_e].reshape(-1)] = True
    inner_v = ~on_boundary
    n = np.maximum(valence, 1)
    # Interior: (F + 2R + (n - 3)P) / n with F the mean face point, R the mean edge midpoint.
    idx = np.arange(N)
    add(idx[inner_v], idx[inner_v], (n[inner_v] - 3) / n[inner_v])
    # 2R/n: each incident edge midpoint contributes (v + w)/2 * 2/(n*n) = (v + w)/(n*n).
    for a, b in ((0, 1), (1, 0)):
        va, vb = E[:, a], E[:, b]
        m = inner_v[va]
        add(va[m], va[m], 1.0 / (n[va[m]] ** 2))
        add(va[m], vb[m], 1.0 / (n[va[m]] ** 2))
    # F/n: each incident face's point (mean of its 4 verts) / (nfaces * n).
    for k in range(4):
        v = q.f[:, k]
        m = inner_v[v]
        for j in range(4):
            add(v[m], q.f[m, j], 0.25 / (nfaces[v[m]] * n[v[m]]))
    # Boundary vertices: (v_prev + 6v + v_next) / 8. A non-manifold vertex (two face fans meeting
    # at one point: more than two boundary edges) is kept as a corner, or its weights would not
    # sum to one and it would fly off.
    nb = np.bincount(E[boundary_e].reshape(-1), minlength=N)
    regular = on_boundary & (nb == 2)
    corner = on_boundary & (nb != 2)
    add(idx[regular], idx[regular], 0.75)
    add(idx[corner], idx[corner], 1.0)
    be = E[boundary_e]
    for a, b in ((0, 1), (1, 0)):
        m = regular[be[:, a]]
        add(be[m, a], be[m, b], 0.125)
    S = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(N + nE + M, N))
    S.sum_duplicates()

    # Topology: each quad (a b c d) -> 4 quads around its face point.
    a, b, c, d = q.f.T
    e01, e12, e23, e30 = (N + fe[:, k] for k in range(4))
    fp = N + nE + np.arange(M)
    nf = np.concatenate([
        np.stack([a, e01, fp, e30], 1), np.stack([b, e12, fp, e01], 1),
        np.stack([c, e23, fp, e12], 1), np.stack([d, e30, fp, e23], 1)])
    # Face-varying UVs: corners keep theirs, edge midpoints and face centres are new per face side.
    U = q.uv[q.fuv]  # (M, 4, 2)
    uv_c = q.fuv
    K = len(q.uv)
    mid = 0.5 * (U + np.roll(U, -1, axis=1))  # side k midpoint
    cen = U.mean(1)
    # Deduplicate the per-face midpoint UVs so seams that agree share one UV.
    allmid = mid.reshape(-1, 2)
    allm_key = np.round(allmid * 1e6).astype(np.int64)
    side_edge = (fe.reshape(-1))
    keys = np.concatenate([side_edge[:, None], allm_key], 1)
    uk, minv = np.unique(keys, axis=0, return_inverse=True)
    minv = minv.reshape(-1)
    muv = np.zeros((len(uk), 2))
    muv[minv] = allmid
    mid_idx = (K + minv).reshape(M, 4)
    cen_idx = K + len(uk) + np.arange(M)
    new_uv = np.concatenate([q.uv, muv, cen])
    m01, m12, m23, m30 = (mid_idx[:, k] for k in range(4))
    nfuv = np.concatenate([
        np.stack([uv_c[:, 0], m01, cen_idx, m30], 1), np.stack([uv_c[:, 1], m12, cen_idx, m01], 1),
        np.stack([uv_c[:, 2], m23, cen_idx, m12], 1), np.stack([uv_c[:, 3], m30, cen_idx, m23], 1)])
    return Quads(S @ q.v, nf, new_uv, nfuv), S


# ---- GPU form ---------------------------------------------------------------------------------


@dataclass
class Renderable:
    pos: np.ndarray  # (V, 3)
    nrm: np.ndarray
    uv: np.ndarray
    tan: np.ndarray  # (V, 4)
    tri: np.ndarray  # (T, 3)
    src: np.ndarray  # (V,) index into the source position array (for weights / morphs)


def triangulate(f):
    """Quads -> triangles along the shorter diagonal is decided by caller positions; here 0-2."""
    return np.concatenate([f[:, [0, 1, 2]], f[:, [0, 2, 3]]])


def vertex_normals(v, tri):
    n = np.cross(v[tri[:, 1]] - v[tri[:, 0]], v[tri[:, 2]] - v[tri[:, 0]])
    out = np.zeros_like(v)
    for k in range(3):
        np.add.at(out, tri[:, k], n)
    return out / np.maximum(np.linalg.norm(out, axis=1, keepdims=True), 1e-12)


def tangents(pos, nrm, uv, tri):
    """Per-vertex tangents in glTF convention: T follows +u; the bitangent cross(N, T) * w points
    toward decreasing v (texture 'up', as MikkTSpace computes it on the un-flipped V axis)."""
    uv = uv * np.array([1.0, -1.0])
    p0, p1, p2 = pos[tri[:, 0]], pos[tri[:, 1]], pos[tri[:, 2]]
    w0, w1, w2 = uv[tri[:, 0]], uv[tri[:, 1]], uv[tri[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = w1 - w0, w2 - w0
    r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(r) < 1e-12, 1e-12, r)
    sdir = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / r[:, None]
    tdir = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) / r[:, None]
    T = np.zeros_like(pos)
    B = np.zeros_like(pos)
    for k in range(3):
        np.add.at(T, tri[:, k], sdir)
        np.add.at(B, tri[:, k], tdir)
    T = T - nrm * np.sum(nrm * T, 1, keepdims=True)
    bad = np.linalg.norm(T, axis=1) < 1e-12
    if bad.any():
        alt = np.where(np.abs(nrm[bad, 0:1]) < 0.9, np.array([[1.0, 0, 0]]), np.array([[0, 1.0, 0]]))
        T[bad] = alt - nrm[bad] * np.sum(nrm[bad] * alt, 1, keepdims=True)
    T /= np.linalg.norm(T, axis=1, keepdims=True)
    w = np.where(np.sum(np.cross(nrm, T) * B, 1) < 0, -1.0, 1.0)
    return np.concatenate([T, w[:, None]], 1)


def renderable(q: Quads, normals=None) -> Renderable:
    """Unique (position, uv) vertices; normals are smooth over positions (seamless across UV seams)."""
    tri_p = triangulate(q.f)
    tri_u = triangulate(q.fuv)
    pairs = np.stack([tri_p.reshape(-1), tri_u.reshape(-1)], 1)
    uniq, inv = np.unique(pairs, axis=0, return_inverse=True)
    tri = inv.reshape(-1, 3)
    pn = vertex_normals(q.v, tri_p) if normals is None else normals
    pos = q.v[uniq[:, 0]]
    nrm = pn[uniq[:, 0]]
    uv = q.uv[uniq[:, 1]]
    return Renderable(pos, nrm, uv, tangents(pos, nrm, uv, tri), tri, uniq[:, 0])


def from_triangles(pos, tri, uv=None, nrm=None, src=None):
    if nrm is None:
        nrm = vertex_normals(pos, tri)
    if uv is None:
        uv = np.zeros((len(pos), 2))
    return Renderable(pos, nrm, uv, tangents(pos, nrm, uv, tri), tri, np.arange(len(pos)) if src is None else src)


# ---- UV islands ---------------------------------------------------------------------------


def uv_islands(fuv, nuv):
    """Connected components of faces sharing UV vertices."""
    from scipy.sparse.csgraph import connected_components

    M = len(fuv)
    r = np.repeat(np.arange(M), fuv.shape[1])
    g = sp.csr_matrix((np.ones(len(r)), (r, fuv.reshape(-1))), shape=(M, nuv))
    adj = g @ g.T
    _, lab = connected_components(adj, directed=False)
    return lab


def repack_islands(q: Quads, density=None, padding=0.006):
    """Packs the UV islands of q into [0,1]^2, each scaled to equal texel density times
    `density[face]` (mean over the island). Returns a new Quads sharing q's topology."""
    lab = uv_islands(q.fuv, len(q.uv))
    nisl = lab.max() + 1
    uv = q.uv.copy()
    boxes = []
    for i in range(nisl):
        fs = np.where(lab == i)[0]
        ids = np.unique(q.fuv[fs])
        p = q.v[q.f[fs]]
        t = q.uv[q.fuv[fs]]
        a3 = 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1) + \
            0.5 * np.linalg.norm(np.cross(p[:, 2] - p[:, 0], p[:, 3] - p[:, 0]), axis=1)
        e1, e2, e3 = t[:, 1] - t[:, 0], t[:, 2] - t[:, 0], t[:, 3] - t[:, 0]
        a2 = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) + 0.5 * np.abs(e2[:, 0] * e3[:, 1] - e2[:, 1] * e3[:, 0])
        k = np.sqrt(a3.sum() / max(a2.sum(), 1e-12))
        if density is not None:
            k *= float(np.mean(density[fs]))
        lo = uv[ids].min(0)
        uv[ids] = (uv[ids] - lo) * k
        boxes.append((i, ids, uv[ids].max(0)))
    # Shelf packing with a binary search on the global scale.
    order = sorted(boxes, key=lambda b: -b[2][1])

    def pack(scale):
        x = y = shelf = 0.0
        place = {}
        for i, ids, ext in order:
            w, h = ext * scale
            if x + w + padding > 1.0:
                x = 0.0
                y += shelf + padding
                shelf = 0.0
            place[i] = (x + padding * 0.5, y + padding * 0.5)
            x += w + padding
            shelf = max(shelf, h)
        return place, y + shelf + padding <= 1.0

    lo_s, hi_s = 0.01, 100.0
    for _ in range(40):
        mid = 0.5 * (lo_s + hi_s)
        if pack(mid)[1]:
            lo_s = mid
        else:
            hi_s = mid
    place, ok = pack(lo_s)
    assert ok
    out = uv.copy()
    for i, ids, _ in order:
        out[ids] = uv[ids] * lo_s + np.array(place[i])
    return Quads(q.v, q.f, out, q.fuv)
