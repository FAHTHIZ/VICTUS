# SPDX-License-Identifier: Apache-2.0
"""Unit checks for the VICTUS character pipeline (docs/CHARACTERS.md, ADR-0179).

Run with the pipeline's Python: `python tools/characters/test_pipeline.py` (needs NumPy, SciPy and
Pillow; not bpy). Covers the pieces whose mistakes are invisible in a render: the subdivision
operator, tangent handedness, quaternion conversion, the glTF writer's quantised sparse morphs and
skin streams, the MakeHuman loaders and the skeleton layout's backward compatibility.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anim  # noqa: E402
import fit  # noqa: E402
import gear  # noqa: E402
import gltf  # noqa: E402
import meshops  # noqa: E402
import mh  # noqa: E402

ORIGINAL_21 = ["Hips", "Spine", "Chest", "Neck", "Head", "Shoulder.L", "UpperArm.L", "LowerArm.L", "Hand.L", "Shoulder.R",
               "UpperArm.R", "LowerArm.R", "Hand.R", "UpperLeg.L", "LowerLeg.L", "Foot.L", "Toe.L", "UpperLeg.R",
               "LowerLeg.R", "Foot.R", "Toe.R"]


def cube():
    v = np.array([[x, y, z] for z in (0, 1) for y in (0, 1) for x in (0, 1)], float)
    f = np.array([[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]])
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float)
    return meshops.Quads(v, f, uv, np.tile([0, 1, 2, 3], (6, 1)))


def _outward_fraction(q, center):
    tri = meshops.triangulate(q.f)
    a, b, c = q.v[tri[:, 0]], q.v[tri[:, 1]], q.v[tri[:, 2]]
    n = np.cross(b - a, c - a)
    return float(((((a + b + c) / 3 - center) * n).sum(1) > 0).mean())


def test_gear_closed_surfaces_wind_outward():
    # Every side of the rounded box and the cube-sphere winds counter-clockwise from outside
    # (a flipped side renders inside-out and breaks the clearance sign).
    box = gear.rounded_box(np.array([1.0, 2.0, 3.0]), (0.2, 0.1, 0.3), 0.03, n=6)
    assert _outward_fraction(box, np.array([1.0, 2.0, 3.0])) == 1.0
    sph = gear.cube_sphere(5)
    assert _outward_fraction(sph, np.zeros(3)) == 1.0
    assert np.allclose(np.linalg.norm(sph.v, axis=1), 1.0)
    # Welded: a closed surface has every edge on exactly two faces.
    e = np.sort(np.concatenate([sph.f[:, [0, 1]], sph.f[:, [1, 2]], sph.f[:, [2, 3]], sph.f[:, [3, 0]]]), 1)
    _, counts = np.unique(e, axis=0, return_counts=True)
    assert (counts == 2).all()


def test_gear_closest_point_and_signed_clearance():
    a, b, c = np.array([[0.0, 0, 0]]), np.array([[1.0, 0, 0]]), np.array([[0.0, 1, 0]])
    pts = np.array([[0.2, 0.2, 0.5], [2.0, -1.0, 0.0], [-1.0, -1.0, 1.0], [1.0, 1.0, 0.0]])
    cp = gear.closest_on_triangles(pts, np.repeat(a, 4, 0), np.repeat(b, 4, 0), np.repeat(c, 4, 0))
    assert np.allclose(cp, [[0.2, 0.2, 0], [1, 0, 0], [0, 0, 0], [0.5, 0.5, 0]])
    # A point inside a closed box has positive clearance (covered), outside negative.
    box = gear.rounded_box(np.zeros(3), (0.1, 0.1, 0.1), 0.01, n=6)
    tri = meshops.triangulate(box.f)
    s = gear._signed(box.v, tri, np.array([[0.0, 0.0, 0.05], [0.0, 0.0, 0.13]]), 24)
    assert abs(s[0] - 0.05) < 2e-3 and abs(s[1] + 0.03) < 2e-3


def test_catmull_clark_operator():
    q = cube()
    q2, S = meshops.catmull_clark(q)
    assert len(q2.f) == 24 and len(q2.v) == 8 + 12 + 6
    rows = np.asarray(S.sum(1)).ravel()
    assert np.allclose(rows, 1.0), "affine: every row of S sums to one"
    assert np.allclose(S @ q.v, q2.v)
    c = q2.v.mean(0)
    assert np.allclose(c, 0.5), "symmetric subdivision keeps the centroid"
    r = np.linalg.norm(q2.v - 0.5, axis=1)
    assert r.max() < np.sqrt(3) / 2, "the limit surface shrinks inside the control cage"
    # Weights carried by S stay normalised.
    w = np.random.default_rng(1).random((8, 4))
    w /= w.sum(1, keepdims=True)
    assert np.allclose((S @ w).sum(1), 1.0)


def test_catmull_clark_bowtie_vertex_stays_put():
    # Two quads touching at one vertex (what culling hidden faces under a plate can leave): the
    # shared vertex has four boundary edges; it must stay a corner, not be scaled away.
    v = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [2, 1, 0], [2, 2, 0], [1, 2, 0]], float) + 5.0
    f = np.array([[0, 1, 2, 3], [2, 4, 5, 6]])
    q = meshops.Quads(v, f, v[:, :2].copy(), f.copy())
    q2, S = meshops.catmull_clark(q)
    assert np.allclose(np.asarray(S.sum(1)).ravel(), 1.0)
    assert np.allclose(q2.v[2], v[2])


def test_catmull_clark_nonmanifold_edge_is_a_crease():
    # Three quads sharing one edge (a fin a thin plate's shell can leave): the edge point must
    # stay on the edge, not average three face points with weights summing to 1.5.
    v = np.array([[0, 0, 0], [0, 0, 1], [1, 0, 0], [1, 0, 1], [-1, 0, 0], [-1, 0, 1], [0, 1, 0], [0, 1, 1]], float)
    f = np.array([[0, 2, 3, 1], [1, 5, 4, 0], [0, 1, 7, 6]])
    q = meshops.Quads(v, f, v[:, :2].copy(), f.copy())
    q2, S = meshops.catmull_clark(q)
    assert np.allclose(np.asarray(S.sum(1)).ravel(), 1.0)
    assert np.abs(q2.v[:, :2]).max() <= 1.0 + 1e-9


def test_tangent_handedness():
    # A +Z-facing quad with u along +X and v along -Y (texture rows go down the image) is the
    # glTF convention: bitangent cross(N, T) * w must point toward decreasing v, i.e. +Y.
    pos = np.array([[0, 0, 0], [1, 0, 0], [1, -1, 0], [0, -1, 0]], float)
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float)
    tri = np.array([[0, 2, 1], [0, 3, 2]])  # counter-clockwise seen from +Z
    r = meshops.from_triangles(pos, tri, uv)
    assert np.allclose(r.nrm, [0, 0, 1])
    assert np.allclose(r.tan[:, :3], [1, 0, 0])
    b = np.cross(r.nrm, r.tan[:, :3]) * r.tan[:, 3:4]
    assert np.allclose(b, [0, 1, 0])
    # Mirrored UVs flip w.
    r2 = meshops.from_triangles(pos, tri, uv * [-1, 1] + [1, 0])
    assert np.all(r2.tan[:, 3] == -r.tan[:, 3])


def test_quaternions():
    rng = np.random.default_rng(3)
    for _ in range(50):
        axis = rng.normal(size=3)
        axis /= np.linalg.norm(axis)
        ang = rng.uniform(-np.pi, np.pi)
        K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
        R = np.eye(3) + np.sin(ang) * K + (1 - np.cos(ang)) * K @ K
        q = gltf.quat_from_matrix(R)
        x, y, z, w = q
        R2 = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                       [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                       [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        assert np.allclose(R, R2, atol=1e-9)
    # anim's [x, y, z, w] helpers agree with the matrix form.
    q = anim.q_axis((0, 0, 1), 90)
    assert np.allclose(q, [0, 0, np.sqrt(0.5), np.sqrt(0.5)])


def parse_glb(b: bytes):
    magic, version, length = struct.unpack("<III", b[:12])
    assert magic == 0x46546C67 and version == 2 and length == len(b)
    jl, jt = struct.unpack("<II", b[12:20])
    assert jt == 0x4E4F534A and jl % 4 == 0
    j = json.loads(b[20:20 + jl])
    bl, bt = struct.unpack("<II", b[20 + jl:28 + jl])
    assert bt == 0x004E4942 and bl % 4 == 0
    return j, b[28 + jl:28 + jl + bl]


def test_gltf_writer_quantised_morphs_and_skin():
    w = gltf.Writer()
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], float)
    nrm = np.tile([0, 0, 1.0], (3, 1))
    uv = np.zeros((3, 2))
    tan = np.tile([1, 0, 0, 1.0], (3, 1))
    joints = np.array([[0, 1, 0, 0]] * 3, np.uint8)
    weights = np.array([[178, 77, 0, 0]] * 3, np.uint8)
    morph_p = np.zeros((3, 3))
    morph_p[1] = [0.004, -0.002, 0.0]
    morph_n = np.zeros((3, 3))
    morph_n[1] = [0.1, 0, 0]
    mat = w.material({"name": "M"})
    prim = w.skinned_primitive(pos, nrm, uv, tan, np.array([[0, 1, 2]]), joints, weights, mat,
                               morphs=[([1], morph_p[1:], morph_n[1:])])
    w.mesh("Body", [prim], target_names=["a"])
    w.node("Rig", root=True)
    j, bin_ = parse_glb(w.tobytes())
    assert j["extensionsRequired"] == ["KHR_mesh_quantization"]
    p = j["meshes"][0]["primitives"][0]
    acc = j["accessors"]
    jw = acc[p["attributes"]["WEIGHTS_0"]]
    assert jw["componentType"] == gltf.UBYTE and jw["normalized"]
    view = j["bufferViews"][jw["bufferView"]]
    raw = np.frombuffer(bin_, np.uint8, view["byteLength"], view.get("byteOffset", 0)).reshape(-1, 4)
    assert np.all(raw.astype(int).sum(1) == 255), "UBYTE weights sum exactly to 255"
    t = acc[p["targets"][0]["POSITION"]]
    assert t["normalized"] and t["componentType"] == gltf.SHORT and t["sparse"]["count"] == 1
    sv = j["bufferViews"][t["sparse"]["values"]["bufferView"]]
    val = np.frombuffer(bin_, "<i2", 3, sv.get("byteOffset", 0)) / 32767.0
    assert np.abs(val - morph_p[1]).max() < 1.0 / 32767 + 1e-9
    for v in j["bufferViews"]:
        assert v.get("byteOffset", 0) % 4 == 0


def test_skeleton_layout_backward_compatible():
    layout = fit.skeleton_layout()
    names = [j[0] for j in layout]
    assert len(names) == 60 and len(set(names)) == 60
    assert names[:21] == ORIGINAL_21, "the original 21-joint layout leads, in its original order"
    parents = dict(layout)
    for i, (n, p) in enumerate(layout):
        assert p is None or names.index(p) < i, f"{n}: parents precede children"
    for side in "LR":
        assert parents[f"Index1.{side}"] == f"Hand.{side}" and parents[f"Index3.{side}"] == f"Index2.{side}"
        assert parents[f"LowerArmTwist.{side}"] == f"LowerArm.{side}"
    assert parents["Jaw"] == "Head" and parents["Eye.L"] == "Head"


def test_makehuman_assets_load():
    base = mh.load_obj("base.obj")
    assert len(base.v) == 19158, "MakeHuman hm08 base mesh"
    assert "body" in base.groups
    assert len(mh.face_unit_names()) >= 44


def test_clip_library_keeps_original_durations():
    clips = {c[0]: c[1] for c in anim.CLIPS}
    for name, sec in {"Idle": 4.0, "Walk": 0.8, "Run": 0.55, "Jump": 0.6, "Fall": 0.8, "Land": 0.4, "Interact": 0.9}.items():
        assert abs(clips[name] - sec) < 1e-6, name
    assert len(clips) >= 40


def test_facet_pulls_a_dome_onto_planes():
    """plates.facet turns a smooth dome into flat panels: after faceting, almost every vertex lies
    on one of the k planes, offsets stay inside the clamp, and mix 0 leaves the surface alone."""
    import plates
    from meshops import triangulate, vertex_normals

    n = 24
    u = np.linspace(-1, 1, n + 1)
    U, V = np.meshgrid(u, u, indexing="ij")
    base = np.stack([U.reshape(-1) * 0.1, V.reshape(-1) * 0.1, np.zeros((n + 1) ** 2)], 1)
    nrm = np.tile([0, 0, 1.0], (len(base), 1))
    dome = 0.02 * (1 - 0.5 * (U.reshape(-1) ** 2 + V.reshape(-1) ** 2))
    pos = base + nrm * dome[:, None]
    idx = np.arange((n + 1) ** 2).reshape(n + 1, n + 1)
    f = np.stack([idx[:-1, :-1], idx[1:, :-1], idx[1:, 1:], idx[:-1, 1:]], -1).reshape(-1, 4)
    w = np.ones(len(pos))
    out = plates.facet(pos, base, nrm, f, 5, 1.0, lo=0.002, hi=0.05, w=w)
    t = out[:, 2]
    assert (t >= 0.002 - 1e-9).all() and (t <= 0.05 + 1e-9).all()
    N = vertex_normals(out, triangulate(f))
    C, lab = plates._kmeans_dirs(N, w, 5)
    # Planar: within each cluster the points fit one plane to a fraction of a millimetre.
    for i in range(5):
        P = out[lab == i]
        if len(P) < 10:
            continue
        P = P - P.mean(0)
        assert np.linalg.svd(P, compute_uv=False)[-1] / np.sqrt(len(P)) < 0.0012, i
    same = plates.facet(pos, base, nrm, f, 5, 0.0, lo=0.002, hi=0.05, w=w)
    assert np.allclose(same, pos)


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print("ok", t.__name__)
    print(f"{len(tests)}/{len(tests)} passed")
