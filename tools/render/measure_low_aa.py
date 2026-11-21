#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measures anti-aliasing and upscaling choices for the Low quality tier (ADR-0188).

Renders samples/reference_scenes with `victus run` at Low supersampled (render scale 2 with 4x
MSAA, the reference: the same features, so only anti-aliasing and resolution differ) and at Low
with each candidate configuration, then scores every candidate against the reference with
`victus compare` (FLIP, worst tile, temporal change) and records median frame time from
`--profile`.

  python3 tools/render/measure_low_aa.py --victus build/bin/victus --out /tmp/low_aa

Timings from a software rasteriser (lavapipe) mostly scale with shaded samples, so they rank
configurations by fragment work only; phone GPUs are often bandwidth bound (MSAA resolves on
tilers are cheap, TAA history reads are not), so decisions about phones still need device
measurements.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

SCENES = ["exterior", "interior", "effects", "photoreal"]
# name -> extra `victus run` arguments on top of --quality low
CANDIDATES = {
    "low_msaa4_fxaa_0.75": [],
    "low_taau_0.75": ["--taa", "on"],
    "low_taau_0.67": ["--taa", "on", "--render-scale", "0.67"],
    "low_msaa4_fxaa_0.6": ["--taa", "off", "--render-scale", "0.6"],
    "low_taau_0.6": ["--taa", "on", "--render-scale", "0.6"],
    "low_msaa4_fxaa_0.5": ["--taa", "off", "--render-scale", "0.5"],
    "low_taau_0.5": ["--taa", "on", "--render-scale", "0.5"],
    "low_fxaa_0.5": ["--taa", "off", "--msaa", "off", "--render-scale", "0.5"],
}


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def capture(args, project, scene, quality, extra, out_dir):
    cmd = [args.victus, "run", str(project), "--scene", f"scenes/{scene}.vscene", "--frames", str(args.frames),
           "--capture-dir", str(out_dir), "--capture-every", str(args.frames // 3), "--quality", quality,
           "--width", str(args.width), "--height", str(args.height), "--profile", f"{out_dir}.profile.json"] + extra
    rc, out = run(cmd)
    if rc != 0:
        sys.exit(f"victus run failed ({rc}): {' '.join(cmd)}\n{out}")


def frame_ms(profile_path):
    try:
        data = json.loads(pathlib.Path(profile_path).read_text())
    except (OSError, ValueError):
        return None
    return data.get("frame", {}).get("p50_ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--victus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--scenes", default=",".join(SCENES))
    ap.add_argument("--candidates", default=",".join(CANDIDATES), help="comma-separated subset of the candidates")
    args = ap.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    project = root / "samples" / "reference_scenes"
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for scene in args.scenes.split(","):
        ref = out / scene / "reference"
        capture(args, project, scene, "low", ["--taa", "off", "--msaa", "4", "--render-scale", "2"], ref)
        for name in args.candidates.split(","):
            extra = CANDIDATES[name]
            d = out / scene / name
            capture(args, project, scene, "low", extra, d)
            report = out / scene / f"{name}.json"
            rc, text = run([args.victus, "compare", str(ref), str(d), "--json", str(report)])
            if rc not in (0, 4):
                sys.exit(f"victus compare failed ({rc}):\n{text}")
            r = json.loads(report.read_text())
            results.setdefault(name, {})[scene] = {
                "flip_mean": r.get("flip_mean"),
                "flip_worst_tile": r.get("flip_worst_tile"),
                "ssim_worst_tile": r.get("ssim_worst_tile"),
                "temporal_error": r.get("temporal_error"),
                "frame_ms_p50": frame_ms(f"{d}.profile.json"),
            }
            print(f"{scene:10s} {name:22s} " + json.dumps(results[name][scene]), flush=True)
    (out / "summary.json").write_text(json.dumps(results, indent=2))
    print(f"wrote {out / 'summary.json'}")


if __name__ == "__main__":
    main()
