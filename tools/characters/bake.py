# SPDX-License-Identifier: Apache-2.0
"""Texture baking for the VICTUS character pipeline (docs/CHARACTERS.md, ADR-0179).

`rasterize` maps every texel of a UV layout to the surface point it covers (triangle id and
barycentrics), so material maps are authored as functions of the 3D surface (position, normal,
curvature, region masks) and written into the character's real UV layout. Height detail becomes a
tangent-space normal map in the same per-vertex tangent frame the mesh carries (meshops.tangents,
glTF convention: B = cross(N, T) * w), which is the frame the engine's shader reconstructs.

Ambient occlusion is ray traced with Blender Cycles (`bake_ao`, needs the bpy module); everything
else is NumPy. Islands are dilated so mip maps do not bleed the background into seams.
"""
from __future__ import annotations

import io
import math

import numpy as np
from PIL import Image
from scipy import ndimage


class Texels:
    """Covered texels of a UV layout: pixel coordinates, triangle ids and barycentrics."""

    def __init__(self, size, ij, tri_id, bary, r):
        self.size, self.ij, self.tri_id, self.bary, self.r = size, ij, tri_id, bary, r

    def interp(self, attr):
        a = attr[self.r.tri[self.tri_id]]  # (n, 3, k)
        return np.einsum("nk,nkc->nc", self.bary, a.reshape(len(a), 3, -1))

    def image(self, values, fill=0.0):
        values = np.asarray(values, float)
        c = 1 if values.ndim == 1 else values.shape[1]
        img = np.full((self.size, self.size, c), fill, float)
        img[self.ij[:, 1], self.ij[:, 0]] = values.reshape(len(values), c)
        return img

    def mask(self):
        m = np.zeros((self.size, self.size), bool)
        m[self.ij[:, 1], self.ij[:, 0]] = True
        return m


def rasterize(r, size, chunk=200000):
    """Texel centres inside each UV triangle (v flipped: glTF UV origin is the top-left)."""
    uv = r.uv.copy()
    P = np.stack([uv[:, 0] * size - 0.5, uv[:, 1] * size - 0.5], 1)
    T = P[r.tri]
    lo = np.floor(T.min(1)).astype(int)
    hi = np.ceil(T.max(1)).astype(int)
    lo = np.clip(lo, 0, size - 1)
    hi = np.clip(hi, 0, size - 1)
    w = hi[:, 0] - lo[:, 0] + 1
    h = hi[:, 1] - lo[:, 1] + 1
    n = w * h
    out_ij, out_t, out_b = [], [], []
    tri_ids = np.arange(len(r.tri))
    start = 0
    cum = np.cumsum(n)
    while start < len(r.tri):
        end = int(np.searchsorted(cum, (cum[start - 1] if start else 0) + chunk, side="right"))
        end = max(end, start + 1)
        ids = tri_ids[start:end]
        cnt = n[ids]
        tid = np.repeat(ids, cnt)
        local = np.arange(cnt.sum()) - np.repeat(np.cumsum(cnt) - cnt, cnt)
        px = lo[tid, 0] + local % w[tid]
        py = lo[tid, 1] + local // w[tid]
        a, b, c = T[tid, 0], T[tid, 1], T[tid, 2]
        v0, v1 = b - a, c - a
        v2 = np.stack([px, py], 1) - a
        den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
        den = np.where(np.abs(den) < 1e-14, 1e-14, den)
        l1 = (v2[:, 0] * v1[:, 1] - v1[:, 0] * v2[:, 1]) / den
        l2 = (v0[:, 0] * v2[:, 1] - v2[:, 0] * v0[:, 1]) / den
        l0 = 1 - l1 - l2
        eps = -1e-4
        inside = (l0 >= eps) & (l1 >= eps) & (l2 >= eps)
        out_ij.append(np.stack([px[inside], py[inside]], 1))
        out_t.append(tid[inside])
        out_b.append(np.stack([l0, l1, l2], 1)[inside])
        start = end
    ij = np.concatenate(out_ij)
    t = np.concatenate(out_t)
    bary = np.clip(np.concatenate(out_b), 0, 1)
    bary /= bary.sum(1, keepdims=True)
    # One sample per texel (the first triangle wins on shared edges).
    key = ij[:, 1] * size + ij[:, 0]
    _, first = np.unique(key, return_index=True)
    return Texels(size, ij[first], t[first], bary[first], r)


def dilate(img, mask, steps=8):
    """Grows island colours outward into the background (seam padding for mip maps)."""
    img = img.copy()
    m = mask.copy()
    for _ in range(steps):
        grown = ndimage.binary_dilation(m)
        ring = grown & ~m
        if not ring.any():
            break
        acc = np.zeros_like(img)
        cnt = np.zeros(img.shape[:2])
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == dy == 0:
                    continue
                sm = np.roll(np.roll(m, dy, 0), dx, 1)
                si = np.roll(np.roll(img, dy, 0), dx, 1)
                acc += si * sm[..., None]
                cnt += sm
        sel = ring & (cnt > 0)
        img[sel] = acc[sel] / cnt[sel, None]
        m = m | sel
    # Remaining background: the mean colour (keeps the coarsest mips sane).
    if (~m).any() and m.any():
        img[~m] = img[m].mean(0)
    return img


# ---- noise -------------------------------------------------------------------------------


def _hash3(i, j, k, seed):
    h = (i * 73856093) ^ (j * 19349663) ^ (k * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF) / float(0xFFFFFF)


def value_noise(P, freq, seed=0):
    X = P * freq
    i = np.floor(X).astype(np.int64)
    f = X - i
    f = f * f * (3 - 2 * f)
    out = 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                out = out + w * _hash3(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz, seed)
    return out * 2 - 1


def fbm(P, freq, octaves=4, seed=0, gain=0.5):
    out, amp, tot = 0.0, 1.0, 0.0
    for o in range(octaves):
        out = out + amp * value_noise(P, freq * (2 ** o), seed + 17 * o)
        tot += amp
        amp *= gain
    return out / tot


def worley(P, spacing, seed=0):
    """Distance to the nearest of jittered feature points (one per cell of `spacing`), / spacing."""
    from scipy.spatial import cKDTree

    lo = P.min(0) - spacing
    hi = P.max(0) + spacing
    # Feature points only in cells near the samples.
    cells = np.unique(np.floor((P - lo) / spacing).astype(np.int64), axis=0)
    off = np.array([[a, b, c] for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)])
    cells = np.unique((cells[:, None, :] + off[None]).reshape(-1, 3), axis=0)
    jit = np.stack([_hash3(cells[:, 0], cells[:, 1], cells[:, 2], seed + k) for k in range(3)], 1)
    pts = lo + (cells + jit) * spacing
    d, _ = cKDTree(pts).query(P, k=1)
    del hi
    return d / spacing


# ---- height -> normal ------------------------------------------------------------------------


def height_to_normal(tx: Texels, height_world, strength=1.0):
    """Tangent-space normals from a height field (metres) defined per covered texel."""
    size = tx.size
    H = tx.image(height_world)[..., 0]
    m = tx.mask()
    H = dilate(H[..., None], m, steps=3)[..., 0]
    # World metres per texel along u and v, from the interpolated UV Jacobian.
    r = tx.r
    p = r.pos[r.tri]
    t = r.uv[r.tri] * size
    e1, e2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    a3 = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
    d1, d2 = t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]
    a2 = 0.5 * np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0])
    texel = np.sqrt(a3 / np.maximum(a2, 1e-12))[tx.tri_id]  # metres per texel
    gx = (np.roll(H, -1, 1) - np.roll(H, 1, 1)) * 0.5
    gy = (np.roll(H, -1, 0) - np.roll(H, 1, 0)) * 0.5
    gx = gx[tx.ij[:, 1], tx.ij[:, 0]] / texel * strength
    gy = gy[tx.ij[:, 1], tx.ij[:, 0]] / texel * strength
    # Texture rows grow with v; the tangent follows +u and the bitangent points up the image (-v).
    n = np.stack([-gx, gy, np.ones_like(gx)], 1)
    return n / np.linalg.norm(n, axis=1, keepdims=True)


def encode_normal(n):
    return np.clip(n * 0.5 + 0.5, 0, 1)


# ---- ambient occlusion (Blender Cycles) ------------------------------------------------------


def bake_ao(r, size, occluders=(), samples=48, distance=0.12):
    """Ray-traced AO in r's UV layout; `occluders` are extra (pos, tri) meshes casting occlusion."""
    import bpy

    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = samples
    sc.world = bpy.data.worlds.new("w")

    def make(name, pos, tri, uv=None):
        me = bpy.data.meshes.new(name)
        me.from_pydata(pos.tolist(), [], tri.tolist())
        if uv is not None:
            layer = me.uv_layers.new(name="UV")
            loops = tri.reshape(-1)
            layer.data.foreach_set("uv", np.stack([uv[loops, 0], 1.0 - uv[loops, 1]], 1).reshape(-1))
        me.update()
        for poly in me.polygons:
            poly.use_smooth = True
        ob = bpy.data.objects.new(name, me)
        sc.collection.objects.link(ob)
        return ob

    target = make("target", r.pos, r.tri, r.uv)
    for i, (pp, tt) in enumerate(occluders):
        make(f"occ{i}", pp, tt)
    img = bpy.data.images.new("ao", size, size, float_buffer=True)
    mat = bpy.data.materials.new("bake")
    mat.use_nodes = True
    node = mat.node_tree.nodes.new("ShaderNodeTexImage")
    node.image = img
    mat.node_tree.nodes.active = node
    target.data.materials.append(mat)
    for ob in sc.objects:
        ob.select_set(ob is target)
    bpy.context.view_layer.objects.active = target
    sc.render.bake.margin = 8
    sc.world.light_settings.distance = distance
    bpy.ops.object.bake(type="AO")
    px = np.array(img.pixels[:]).reshape(size, size, 4)[::-1, :, 0]
    return px


# ---- encoding -----------------------------------------------------------------------------


def to_u8(img):
    return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)


def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def png(img):
    b = io.BytesIO()
    a = to_u8(img)
    Image.fromarray(a if a.shape[2] != 1 else a[..., 0]).save(b, "PNG", optimize=True)
    return b.getvalue()


def jpeg(img, quality=92):
    b = io.BytesIO()
    Image.fromarray(to_u8(img[..., :3])).save(b, "JPEG", quality=quality, subsampling=0, optimize=True)
    return b.getvalue()


def dist_to(P, c):
    return np.linalg.norm(P - np.asarray(c), axis=1)


def soft(d, r0, r1):
    """1 inside r0, 0 beyond r1, smooth between."""
    t = np.clip((d - r0) / max(r1 - r0, 1e-9), 0, 1)
    return 1 - t * t * (3 - 2 * t)


def band(x, a, b, fade):
    return np.clip((x - a) / fade + 1, 0, 1) * np.clip((b - x) / fade + 1, 0, 1)


del math
