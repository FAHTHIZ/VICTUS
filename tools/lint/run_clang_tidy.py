#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run clang-tidy (profile: .clang-tidy) over VICTUS-owned sources.

Usage:
  tools/lint/run_clang_tidy.py [-p BUILD_DIR] [-j N] [--clang-tidy EXE] [--changed-since REF] [PATH_FILTER...]

BUILD_DIR must contain compile_commands.json (every VICTUS CMake configure writes one). Only
translation units under engine/, editor/, tools/ and apps/ are analysed; fetched and vendored
third-party code, generated shader blobs and tests are skipped. PATH_FILTER restricts the run to
files whose repository-relative path contains one of the strings. Exit status is non-zero when any
diagnostic is reported. Uses only the Python standard library.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OWNED = ("engine/", "editor/", "tools/", "apps/")
SKIP = ("/generated/", "/third_party/", "_deps/")


def find_clang_tidy(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ["clang-tidy"] + [f"clang-tidy-{v}" for v in range(30, 14, -1)]:
        path = shutil.which(name)
        if path:
            return path
    sys.exit("error: clang-tidy not found (install clang-tidy or pass --clang-tidy)")


def changed_files(ref: str) -> set[str]:
    out = subprocess.run(["git", "diff", "--name-only", ref, "--"], cwd=ROOT, check=True, capture_output=True, text=True)
    return {line.strip() for line in out.stdout.splitlines() if line.strip()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--build-dir", default="build/dev")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--clang-tidy")
    ap.add_argument("--changed-since", help="only analyse files changed since this git ref")
    ap.add_argument("filters", nargs="*")
    args = ap.parse_args()

    db_path = (ROOT / args.build_dir / "compile_commands.json")
    if not db_path.exists():
        sys.exit(f"error: {db_path} not found; configure CMake first (e.g. cmake --preset dev)")
    with db_path.open(encoding="utf-8") as f:
        db = json.load(f)
    files: list[str] = []
    for entry in db:
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        try:
            rel = path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            continue
        if not rel.startswith(OWNED) or any(s in "/" + rel for s in SKIP) or not rel.endswith((".cpp", ".cc")):
            continue
        if args.filters and not any(flt in rel for flt in args.filters):
            continue
        files.append(rel)
    files = sorted(set(files))
    if args.changed_since:
        changed = changed_files(args.changed_since)
        files = [f for f in files if f in changed]
    if not files:
        print("no files to analyse")
        return 0

    exe = find_clang_tidy(args.clang_tidy)
    print(f"{Path(exe).name}: analysing {len(files)} files with {args.jobs} jobs", flush=True)

    def run(rel: str) -> tuple[str, int, str]:
        proc = subprocess.run([exe, "-p", str(db_path.parent), "--quiet", "--extra-arg=-Wno-unknown-warning-option", rel], cwd=ROOT, capture_output=True, text=True)
        out = "\n".join(l for l in (proc.stdout + proc.stderr).splitlines()
                        if l and not l.endswith("warnings generated.") and "Suppressed" not in l
                        and "Use -header-filter" not in l and "non-user code" not in l)
        return rel, proc.returncode, out

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, (rel, code, out) in enumerate(pool.map(run, files), 1):
            status = "ok" if code == 0 else "FAIL"
            print(f"[{i}/{len(files)}] {status} {rel}", flush=True)
            if code != 0:
                failed += 1
                if out:
                    print(out, flush=True)
    print(f"clang-tidy: {failed} of {len(files)} files with diagnostics")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
