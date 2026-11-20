# SPDX-License-Identifier: Apache-2.0
"""Turns the character fields of anatomy.py into triangle meshes, skin weights and normals (NumPy).

Pipeline per character:
  1. body field -> marching cubes (sdf.sample_dense evaluates only blocks near the surface);
  2. every plate field -> marching cubes inside the plate's bounds;
  3. hidden surfaces are dropped: body triangles under a plate and plate triangles inside the body,
     so no triangle is spent where it can never be seen;
  4. skin weights come from the primitives: each vertex is weighted toward the bones whose
     primitives shape it (falloff on the primitive's distance), four influences at most.
Decimation and normals are applied afterwards in Blender (build_characters.py).
"""
from __future__ import annotations

import time

import numpy as np

import sdf


def body_mesh(field, voxel):
    lo, hi = field.bounds()

    def block(P, c, r):
        return field.eval(P, None if c is None else (c, r, 0.02))

    t = time.time()
    grid = sdf.sample_dense(block, lo, hi, voxel)
    v, f = sdf.polygonize(grid, lo, voxel)
    print(f"  body: {len(f)} triangles in {time.time() - t:.1f}s")
    return v, f


def plate_fn(field, plate):
    def fn(P, cull=None):
        return sdf.smax(field.eval(P, cull) - plate.offset, plate.region(P), plate.edge)

    return fn


def plate_mesh(field, plate, voxel):
    lo, hi = field.bounds()
    lo = np.maximum(lo, plate.centre - plate.radius)
    hi = np.minimum(hi, plate.centre + plate.radius)
    fn = plate_fn(field, plate)

    def block(P, c, r):
        return fn(P, None if c is None else (c, r, 0.02))

    grid = sdf.sample_dense(block, lo, hi, voxel)
    if grid.min() >= 0 or grid.max() <= 0:
        return np.zeros((0, 3)), np.zeros((0, 3), int)
    return sdf.polygonize(grid, lo, voxel)


def keep_faces(v, f, mask):
    f = f[mask]
    used = np.unique(f)
    remap = -np.ones(len(v), int)
    remap[used] = np.arange(len(used))
    return v[used], remap[f]


def build(field, plates, voxel=0.0025):
    """Returns pieces: list of dicts(name, bone|None, material, verts, faces)."""
    pieces = []
    bv, bf = body_mesh(field, voxel)
    fns = [plate_fn(field, p) for p in plates]
    centroids = bv[bf].mean(axis=1)
    hidden = np.zeros(len(bf), bool)
    for p, fn in zip(plates, fns):
        near = np.all(np.abs(centroids - p.centre) < p.radius, axis=1)
        if near.any():
            idx = np.nonzero(near)[0]
            hidden[idx] |= fn(centroids[idx]) < -0.0012
    bv, bf = keep_faces(bv, bf, ~hidden)
    pieces.append(dict(name="Body", bone=None, material=1, verts=bv, faces=bf))
    for p, fn in zip(plates, fns):
        t = time.time()
        v, f = plate_mesh(field, p, voxel)
        if len(f) == 0:
            print(f"  plate {p.name}: empty")
            continue
        c = v[f].mean(axis=1)
        inside = field.eval(c) < -0.0015
        # Also drop faces buried under a plate that stands prouder (eye lenses, hair caps).
        for q, qfn in zip(plates, fns):
            if q is p or q.offset <= p.offset:
                continue
            near = np.all(np.abs(c - q.centre) < q.radius, axis=1) & ~inside
            if near.any():
                idx = np.nonzero(near)[0]
                inside[idx] |= qfn(c[idx]) < -0.0008
        v, f = keep_faces(v, f, ~inside)
        print(f"  plate {p.name}: {len(f)} triangles in {time.time() - t:.1f}s")
        pieces.append(dict(name=p.name, bone=p.bone, material=p.material, verts=v, faces=f))
    return pieces


def skin_weights(field, P, falloff=0.012, max_influences=4):
    """(bone names, (N, max_influences) indices, weights) from primitive proximity."""
    bones = sorted({p.bone for p in field.prims})
    bi = {b: i for i, b in enumerate(bones)}
    W = np.zeros((len(P), len(bones)))
    for p in field.prims:
        if p.sub:
            continue
        d = np.maximum(p.fn(P), 0.0)
        W[:, bi[p.bone]] += np.exp(-d / falloff)
    order = np.argsort(-W, axis=1)[:, :max_influences]
    w = np.take_along_axis(W, order, axis=1)
    w[w < 0.02 * w[:, :1]] = 0.0
    w /= np.maximum(w.sum(axis=1, keepdims=True), 1e-12)
    return bones, order, w
