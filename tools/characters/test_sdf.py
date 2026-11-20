# SPDX-License-Identifier: Apache-2.0
"""Checks for the character sculpting kernel (python tools/characters/test_sdf.py; NumPy and
scikit-image). Run by hand before regenerating the starter characters."""
import sys
import os

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anatomy  # noqa: E402
import sdf  # noqa: E402


def test_winding_points_out():
    f = sdf.Field([sdf.sphere((0, 0, 0), 0.1, "x")])
    lo, hi = f.bounds()
    v, fa = sdf.polygonize(sdf.sample_dense(lambda P, c, r: f.eval(P), lo, hi, 0.01), lo, 0.01)
    n = np.cross(v[fa[:, 1]] - v[fa[:, 0]], v[fa[:, 2]] - v[fa[:, 0]])
    assert np.mean((n * v[fa].mean(1)).sum(1) > 0) > 0.99


def test_round_cone_distances():
    c = sdf.round_cone((0, 0, 0), (0, 0, 1), 0.2, 0.1, "x")
    P = np.array([[0.2, 0, 0], [0.1, 0, 1], [0, 0, -0.2], [0, 0, 1.1], [0, 0, 0.5]])
    d = c.fn(P)
    assert np.allclose(d[:4], 0, atol=0.01), d
    assert d[4] < -0.1


def test_sparse_sampling_matches_dense():
    f = anatomy.body_field(anatomy.ORION)
    lo = np.array([-0.1, -0.12, 1.45])
    hi = np.array([0.1, 0.1, 1.72])
    sparse = sdf.sample_dense(lambda P, c, r: f.eval(P, None if c is None else (c, r, 0.02)), lo, hi, 0.004)
    dense = sdf.sample_dense(lambda P, c, r: f.eval(P), lo, hi, 0.004, safety=1e9)
    near = np.abs(dense) < 0.01
    assert np.allclose(sparse[near], dense[near], atol=1e-5)
    assert np.all(np.sign(sparse[~near]) == np.sign(dense[~near]))


def test_variants_are_symmetric():
    for v in anatomy.VARIANTS.values():
        f = anatomy.body_field(v)
        P = np.random.default_rng(1).uniform([-0.7, -0.2, 0.0], [0.7, 0.2, 1.72], (4000, 3))
        Q = P * np.array([-1, 1, 1])
        assert np.allclose(f.eval(P), f.eval(Q), atol=1e-6)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
