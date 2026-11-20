# SPDX-License-Identifier: Apache-2.0
"""Signed-distance sculpting kernel for the VICTUS starter characters (docs/CHARACTERS.md).

The characters are modelled as smooth unions of analytic primitives (round cones, ellipsoids,
rounded boxes), the same construction used by SDF sculpting tools: every muscle, bone landmark and
armour plate is a primitive with a blend radius, so forms flow into each other without the creases
a mesh boolean leaves. This module is pure NumPy so it can be unit-checked without Blender.

Distances follow Inigo Quilez's reference formulas (https://iquilezles.org/articles/distfunctions/,
MIT-licensed examples, re-derived here). Ellipsoids and smooth unions are not exact Euclidean
distances; `Field.sample_dense` therefore uses a safety factor when it skips empty blocks.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np


def _dot(a, b):
    return np.einsum("ij,ij->i", a, b)


def smin(a, b, k):
    """Polynomial smooth minimum (blend radius k, in metres)."""
    if k <= 0:
        return np.minimum(a, b)
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h - k * h * (1.0 - h)


def smax(a, b, k):
    return -smin(-a, -b, k)


def frame(axis, up=(0.0, 0.0, 1.0)):
    """Orthonormal rows (x, y, z) with z along `axis`; used to orient primitives."""
    z = np.asarray(axis, dtype=np.float64)
    z = z / np.linalg.norm(z)
    u = np.asarray(up, dtype=np.float64)
    if abs(float(np.dot(u, z))) > 0.95:
        u = np.array([0.0, 1.0, 0.0]) if abs(z[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    x = np.cross(u, z)
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    return np.stack([x, y, z])


def rot(axis, deg):
    a = np.asarray(axis, dtype=np.float64)
    a = a / np.linalg.norm(a)
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    x, y, z = a
    return np.array([
        [c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
        [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
        [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)],
    ])


@dataclass
class Prim:
    """One primitive. `fn(P)` returns distances for an (N, 3) array; centre/radius bound it."""

    fn: object
    centre: np.ndarray
    radius: float
    bone: str
    k: float = 0.02  # blend radius into the running union
    sub: bool = False  # smooth subtraction instead of union

    def lower_bound(self, c, r):
        return float(np.linalg.norm(self.centre - c)) - self.radius - r


def round_cone(a, b, ra, rb, bone, k=0.02, scale=None, sub=False):
    """Capsule with radius ra at a and rb at b. `scale` = (sx, sy) squashes the cross-section in
    the cone's own frame (x = side, y = front/back), giving oval limbs."""
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    F = frame(b - a, up=(0.0, -1.0, 0.0) if abs(b[2] - a[2]) > 0.5 * np.linalg.norm(b - a) else (0.0, 0.0, 1.0))
    ba = b - a
    l2 = float(np.dot(ba, ba))
    rr = ra - rb
    a2 = l2 - rr * rr
    il2 = 1.0 / l2
    inv = None if scale is None else np.array([1.0 / scale[0], 1.0 / scale[1], 1.0])
    smin_scale = 1.0 if scale is None else min(scale)

    def fn(P):
        pa = P - a
        if inv is not None:
            loc = pa @ F.T
            loc *= inv
            pa = loc @ F
        y = pa @ ba
        z = y - l2
        q = pa * l2 - np.outer(y, ba)
        x2 = _dot(q, q)
        y2 = y * y * l2
        z2 = z * z * l2
        kk = math.copysign(1.0, rr) * rr * rr * x2 if rr != 0 else np.zeros_like(x2)
        d1 = np.sqrt(x2 + z2) * il2 - rb
        d2 = np.sqrt(x2 + y2) * il2 - ra
        d3 = (np.sqrt(np.maximum(x2 * a2 * il2, 0.0)) + y * rr) * il2 - ra
        d = np.where(np.sign(z) * a2 * z2 > kk, d1, np.where(np.sign(y) * a2 * y2 < kk, d2, d3))
        return d * smin_scale

    c = 0.5 * (a + b)
    return Prim(fn, c, 0.5 * math.sqrt(l2) + max(ra, rb) * (1.0 if scale is None else max(1.0, *scale)), bone, k, sub)


def ellipsoid(c, r, bone, k=0.02, R=None, sub=False):
    """Ellipsoid centred at c with semi-axes r, optionally rotated by the 3x3 matrix R (local->world)."""
    c = np.asarray(c, float)
    r = np.asarray(r, float)
    Rt = None if R is None else np.asarray(R, float)

    def fn(P):
        p = P - c
        if Rt is not None:
            p = p @ Rt  # world -> local (R orthonormal)
        k0 = np.linalg.norm(p / r, axis=1)
        k1 = np.linalg.norm(p / (r * r), axis=1)
        return k0 * (k0 - 1.0) / np.maximum(k1, 1e-9)

    return Prim(fn, c, float(r.max()), bone, k, sub)


def sphere(c, radius, bone, k=0.02, sub=False):
    c = np.asarray(c, float)
    return Prim(lambda P: np.linalg.norm(P - c, axis=1) - radius, c, radius, bone, k, sub)


def rbox(c, half, rounding, bone, k=0.02, R=None, sub=False):
    c = np.asarray(c, float)
    h = np.asarray(half, float) - rounding
    Rt = None if R is None else np.asarray(R, float)

    def fn(P):
        p = P - c
        if Rt is not None:
            p = p @ Rt
        q = np.abs(p) - h
        return np.linalg.norm(np.maximum(q, 0.0), axis=1) + np.minimum(q.max(axis=1), 0.0) - rounding

    return Prim(fn, c, float(np.linalg.norm(half)), bone, k, sub)


@dataclass
class Field:
    """Ordered smooth CSG of primitives: each one is blended into the running result."""

    prims: list = field(default_factory=list)
    floor: float | None = None  # clip everything below this Z (flat soles)
    # Smooth unions are not associative, so the left/right evaluation order would leave the
    # figure ~2 mm asymmetric. A symmetric field averages the chain with its mirror image (X).
    symmetric: bool = False

    def add(self, *ps):
        self.prims.extend(ps)

    def eval(self, P, cull=None):
        """Distances at P; cull = (centre, radius, band) skips primitives that cannot matter."""
        if not self.symmetric:
            return self._chain(P, cull)
        M = np.array([-1.0, 1.0, 1.0])
        mc = None if cull is None else (cull[0] * M, cull[1], cull[2])
        return 0.5 * (self._chain(P, cull) + self._chain(P * M, mc))

    def _chain(self, P, cull):
        prims = self.prims if cull is None else self.culled(*cull)
        d = np.full(len(P), 1.0)
        for p in prims:
            v = p.fn(P)
            d = smax(d, -v, p.k) if p.sub else smin(d, v, p.k)
        if self.floor is not None:
            d = smax(d, self.floor - P[:, 2], 0.004)
        return d

    def culled(self, c, r, band):
        return [p for p in self.prims if p.lower_bound(c, r) < band + p.k]

    def bounds(self, pad=0.01):
        lo = np.min([p.centre - p.radius for p in self.prims if not p.sub], axis=0) - pad
        hi = np.max([p.centre + p.radius for p in self.prims if not p.sub], axis=0) + pad
        if self.floor is not None:
            lo[2] = max(lo[2], self.floor - pad)
        return lo, hi

    def gradient(self, P, eps=4e-4):
        g = np.zeros_like(P)
        for i in range(3):
            o = np.zeros(3)
            o[i] = eps
            g[:, i] = self.eval(P + o) - self.eval(P - o)
        n = np.linalg.norm(g, axis=1, keepdims=True)
        return g / np.maximum(n, 1e-12)


def sample_dense(fn_block, lo, hi, voxel, block=24, coarse=None, safety=1.6):
    """Samples a field on a regular grid, evaluating only blocks near the surface.

    fn_block(P, centre, radius) -> distances; `coarse(P)` gives cheap block-centre distances
    (defaults to fn_block). Blocks whose centre is farther than `safety` x their half-diagonal
    from the surface are filled with a signed constant, which is all marching cubes needs."""
    coarse = coarse or (lambda P: fn_block(P, None, None))
    n = np.ceil((hi - lo) / voxel).astype(int) + 1
    nb = np.ceil(n / block).astype(int)
    grid = np.empty(tuple(n), dtype=np.float32)
    half = 0.5 * block * voxel * math.sqrt(3.0)
    idx = np.stack(np.meshgrid(*[np.arange(b) for b in nb], indexing="ij"), -1).reshape(-1, 3)
    centres = lo + (idx * block + 0.5 * block) * voxel
    dc = coarse(centres)
    for (bi, bj, bk), cc, d in zip(idx, centres, dc):
        s = (slice(bi * block, min((bi + 1) * block, n[0])), slice(bj * block, min((bj + 1) * block, n[1])),
             slice(bk * block, min((bk + 1) * block, n[2])))
        if abs(d) > safety * half + 0.01:
            grid[s] = math.copysign(0.05, d)
            continue
        axes = [lo[a] + np.arange(s[a].start, s[a].stop) * voxel for a in range(3)]
        P = np.stack(np.meshgrid(*axes, indexing="ij"), -1).reshape(-1, 3)
        grid[s] = fn_block(P, cc, half).reshape(tuple(len(a) for a in axes))
    return grid


def polygonize(grid, lo, voxel):
    from skimage import measure

    verts, faces, _, _ = measure.marching_cubes(grid, level=0.0, spacing=(voxel, voxel, voxel),
                                                allow_degenerate=False)
    # With the default gradient_direction ("descent") and this negative-inside field the
    # triangles come out wound counter-clockwise seen from outside (front faces point out).
    # This is checked empirically: tools/characters/test_sdf.py meshes a sphere and requires
    # every face normal to point away from the centre.
    return verts + lo, faces
