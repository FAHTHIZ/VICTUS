# SPDX-License-Identifier: Apache-2.0
"""Deterministic glTF 2.0 binary (GLB) writer for the VICTUS character pipeline (ADR-0179).

Supports what the characters need and nothing else: node trees, skins, skinned triangle meshes
with several primitives (one per material), explicit normals/tangents/UVs, sparse morph targets,
PBR materials with embedded PNG/JPEG textures, MSFT_lod level-of-detail chains and animations
(rotation/translation/scale and morph weights, linear interpolation). Output bytes depend only
on the input, so regenerated assets diff cleanly.
"""
from __future__ import annotations

import json
import struct

import numpy as np

FLOAT, USHORT, UINT, UBYTE, SHORT, BYTE = 5126, 5123, 5125, 5121, 5122, 5120
ARRAY, ELEMENT = 34962, 34963
TYPES = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}


class Writer:
    def __init__(self):
        self.j = {"asset": {"version": "2.0", "generator": "VICTUS character pipeline (tools/characters)"},
                  "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
                  "textures": [], "images": [], "samplers": [], "skins": [], "animations": [],
                  "accessors": [], "bufferViews": [], "buffers": []}
        self.bin = bytearray()
        self.used_ext = set()
        self.required_ext = set()

    # ---- raw data ---------------------------------------------------------------------------
    def view(self, data: bytes, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin.extend(data)
        self.j["bufferViews"].append(v)
        return len(self.j["bufferViews"]) - 1

    def accessor(self, arr, ctype=FLOAT, target=None, minmax=False, normalized=False):
        arr = np.ascontiguousarray(arr)
        comps = 1 if arr.ndim == 1 else arr.shape[1]
        dt = {FLOAT: "<f4", USHORT: "<u2", UINT: "<u4", UBYTE: "u1"}[ctype]
        a = {"bufferView": self.view(arr.astype(dt).tobytes(), target), "componentType": ctype,
             "count": int(arr.shape[0]), "type": TYPES[comps]}
        if normalized:
            a["normalized"] = True
        if minmax:
            m = arr.reshape(len(arr), -1).astype(np.float64)
            a["min"] = [float(x) for x in m.min(0)]
            a["max"] = [float(x) for x in m.max(0)]
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    def sparse_vec3(self, count, idx, values, quant=None):
        """VEC3 accessor of `count` zeros with `values` at `idx` (morph target deltas).

        quant: None (float), "short" or "byte" - normalised signed integers in [-1, 1]
        (KHR_mesh_quantization), 0.03 mm resolution for position deltas, 1/127 for normal deltas."""
        idx = np.asarray(idx, np.int64)
        values = np.asarray(values, float).reshape(-1, 3)
        if len(idx) == 0:
            idx = np.zeros(1, np.int64)
            values = np.zeros((1, 3))
        itype, idt = (USHORT, "<u2") if count <= 65535 else (UINT, "<u4")
        if quant is None:
            ctype, raw = FLOAT, values.astype("<f4").tobytes()
            deq = values.astype(np.float32)
        else:
            scale, ctype, dt = {"short": (32767, SHORT, "<i2"), "byte": (127, BYTE, "i1")}[quant]
            qv = np.clip(np.rint(values * scale), -scale, scale).astype(dt)
            raw, deq = qv.tobytes(), qv.astype(np.float64) / scale
            self.used_ext.add("KHR_mesh_quantization")
            self.required_ext.add("KHR_mesh_quantization")
        a = {"componentType": ctype, "count": int(count), "type": "VEC3",
             "min": [min(0.0, float(x)) for x in deq.min(0)], "max": [max(0.0, float(x)) for x in deq.max(0)],
             "sparse": {"count": int(len(idx)),
                        "indices": {"bufferView": self.view(idx.astype(idt).tobytes()), "componentType": itype},
                        "values": {"bufferView": self.view(raw)}}}
        if quant is not None:
            a["normalized"] = True
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    # ---- scene ---------------------------------------------------------------------------
    def node(self, name, parent=None, t=None, r=None, s=None, root=False, **extra):
        n = {"name": name}
        if t is not None and np.any(np.abs(t) > 0):
            n["translation"] = [float(x) for x in t]
        if r is not None and abs(r[3] - 1.0) > 1e-9:
            n["rotation"] = [float(x) for x in r]
        if s is not None and np.any(np.abs(np.asarray(s) - 1) > 1e-9):
            n["scale"] = [float(x) for x in s]
        n.update(extra)
        self.j["nodes"].append(n)
        i = len(self.j["nodes"]) - 1
        if parent is not None:
            self.j["nodes"][parent].setdefault("children", []).append(i)
        elif root:
            self.j["scenes"][0]["nodes"].append(i)
        return i

    def image(self, name, data: bytes, mime):
        self.j["images"].append({"name": name, "bufferView": self.view(data), "mimeType": mime})
        return len(self.j["images"]) - 1

    def texture(self, image):
        if not self.j["samplers"]:
            self.j["samplers"].append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})
        self.j["textures"].append({"sampler": 0, "source": image})
        return len(self.j["textures"]) - 1

    def material(self, m: dict):
        self.j["materials"].append(m)
        return len(self.j["materials"]) - 1

    def mesh(self, name, prims, target_names=None):
        m = {"name": name, "primitives": prims}
        if target_names:
            m["extras"] = {"targetNames": list(target_names)}
            m["weights"] = [0.0] * len(target_names)
        self.j["meshes"].append(m)
        return len(self.j["meshes"]) - 1

    def skinned_primitive(self, pos, nrm, uv, tan, tri, joints, weights, material, morphs=None, color=None, morph_quant=True):
        """morphs: list of (idx, dpos, dnrm|None) aligned with the mesh's target names."""
        attrs = {"POSITION": self.accessor(pos, target=ARRAY, minmax=True),
                 "NORMAL": self.accessor(nrm, target=ARRAY),
                 "TANGENT": self.accessor(tan, target=ARRAY),
                 "TEXCOORD_0": self.accessor(uv, target=ARRAY),
                 "JOINTS_0": self.accessor(joints, UBYTE if joints.dtype == np.uint8 else USHORT, target=ARRAY),
                 "WEIGHTS_0": (self.accessor(weights, UBYTE, target=ARRAY, normalized=True) if weights.dtype == np.uint8
                               else self.accessor(weights, target=ARRAY))}
        if color is not None:
            attrs["COLOR_0"] = self.accessor(color, target=ARRAY)
        itype = USHORT if len(pos) <= 65535 else UINT
        p = {"attributes": attrs, "indices": self.accessor(np.asarray(tri).reshape(-1), itype, target=ELEMENT),
             "material": material, "mode": 4}
        if morphs:
            tg = []
            for idx, dp, dn in morphs:
                t = {"POSITION": self.sparse_vec3(len(pos), idx, dp, quant=morph_quant and "short")}
                if dn is not None:
                    t["NORMAL"] = self.sparse_vec3(len(pos), idx, dn, quant=morph_quant and "byte")
                tg.append(t)
            p["targets"] = tg
        return p

    def skin(self, name, joints, ibms, skeleton=None):
        s = {"name": name, "joints": list(joints),
             "inverseBindMatrices": self.accessor(np.asarray(ibms, np.float32).reshape(len(joints), 16))}
        if skeleton is not None:
            s["skeleton"] = skeleton
        self.j["skins"].append(s)
        return len(self.j["skins"]) - 1

    def animation(self, name, channels):
        """channels: list of (node, path, times, values) with values (K, comps) or (K*targets,)."""
        samplers, chans = [], []
        cache = {}
        for node, path, times, values in channels:
            key = (len(times), float(times[-1]))
            if key not in cache:
                cache[key] = self.accessor(np.asarray(times, np.float32), minmax=True)
            samplers.append({"input": cache[key], "output": self.accessor(np.asarray(values, np.float32)),
                             "interpolation": "LINEAR"})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
        self.j["animations"].append({"name": name, "samplers": samplers, "channels": chans})

    def lod(self, node, lod_nodes, coverage):
        """MSFT_lod: `node` is LOD 0; `lod_nodes` are progressively coarser stand-ins (not in the
        scene tree). `coverage` = screen-coverage thresholds, one per level (MSFT_screencoverage)."""
        n = self.j["nodes"][node]
        n.setdefault("extensions", {})["MSFT_lod"] = {"ids": list(lod_nodes)}
        n.setdefault("extras", {})["MSFT_screencoverage"] = [float(c) for c in coverage]
        self.used_ext.add("MSFT_lod")

    def tobytes(self, extras=None):
        j = dict(self.j)
        for k in ("meshes", "materials", "textures", "images", "samplers", "skins", "animations"):
            if not j[k]:
                del j[k]
        if self.used_ext:
            j["extensionsUsed"] = sorted(self.used_ext)
        if self.required_ext:
            j["extensionsRequired"] = sorted(self.required_ext)
        if extras:
            j["extras"] = extras
        while len(self.bin) % 4:
            self.bin.append(0)
        j["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(j, separators=(",", ":"), sort_keys=False).encode()
        while len(js) % 4:
            js += b" "
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js
        out += struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin)
        return out


# ---- math helpers --------------------------------------------------------------------------


def quat_from_matrix(R):
    m = R
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = np.sqrt(tr + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    q /= np.linalg.norm(q)
    if q[3] < 0:
        q = -q
    return q


def to_gl(p):
    """VICTUS authoring (Z up, -Y forward) -> glTF (Y up, +Z forward)."""
    p = np.asarray(p, float)
    return np.stack([p[..., 0], p[..., 2], -p[..., 1]], -1)


def to_gl_tangent(t):
    return np.concatenate([to_gl(t[:, :3]), t[:, 3:4]], 1)
