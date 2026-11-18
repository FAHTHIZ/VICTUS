#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run clang-tidy (profile: .clang-tidy) over VICTUS-owned sources.

Usage:
  tools/lint/run_clang_tidy.py [-p BUILD_DIR] [-j N] [--clang-tidy EXE] [--changed-since REF] [--require-complete] [PATH_FILTER...]

BUILD_DIR must contain compile_commands.json (every VICTUS CMake configure writes one). Only
translation units under engine/, editor/, tools/ and apps/ are analysed; fetched and vendored
third-party code, generated shader blobs and tests are skipped. PATH_FILTER restricts the run to
files whose repository-relative path contains one of the strings. Exit status is non-zero when any
diagnostic is reported. Uses only the Python standard library.

Sources the host build does not compile (the Android entry point) are analysed with the compile
command of a host source that has the same includes (HOST_PROXIES). --require-complete (used by
CI) fails when a git-tracked engine/editor/tools/apps .cpp file is neither in the compile
database nor proxied, so new platform-specific files cannot escape the gate unnoticed.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OWNED = ("engine/", "editor/", "tools/", "apps/")
SKIP = ("/generated/", "/third_party/", "_deps/")
# Not built on desktop hosts -> analysed with the flags of a host file with the same dependencies.
# The first file's command is used; include paths of the others are added (SDL is a private
# dependency of the platform module, so no single host file sees both it and the player headers).
HOST_PROXIES = {
    "apps/android/main.cpp": ["apps/player/main.cpp", "engine/platform/src/platform.cpp"],  # Gradle/NDK build only
}


def include_flags(args: list[str]) -> list[str]:
    out, i = [], 0
    while i < len(args):
        a = args[i]
        if a in ("-I", "-isystem") and i + 1 < len(args):
            out += [a, args[i + 1]]
            i += 2
            continue
        if a.startswith("-I") and len(a) > 2:
            out.append(a)
        i += 1
    return out


def tracked_sources() -> list[str]:
    out = subprocess.run(["git", "ls-files", "--", *(o + "*.cpp" for o in OWNED), *(o + "*.cc" for o in OWNED)],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    return sorted(f for f in out if not any(s in "/" + f for s in SKIP))


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
    ap.add_argument("--require-complete", action="store_true",
                    help="fail if a tracked owned source is missing from the compile database")
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
    by_file = {}
    for entry in db:
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        try:
            by_file[path.resolve().relative_to(ROOT).as_posix()] = entry
        except ValueError:
            pass
    proxied: list[dict] = []
    for target, proxies in HOST_PROXIES.items():
        proxy = proxies[0]
        if target in by_file or any(p not in by_file for p in proxies) or not (ROOT / target).is_file():
            continue
        def argv(e: dict) -> list[str]:
            return list(e["arguments"]) if "arguments" in e else shlex.split(e["command"])
        base = argv(by_file[proxy])
        src = str((ROOT / proxy).resolve())
        dst = str((ROOT / target).resolve())
        extra = [f for p in proxies[1:] for f in include_flags(argv(by_file[p]))]
        cmd = [dst if (a == src or a.endswith("/" + proxy) or a == proxy) else a for a in base]
        cmd = cmd[:1] + extra + cmd[1:]
        entry = {"directory": by_file[proxy]["directory"], "file": dst, "arguments": cmd}
        entry.pop("output", None)
        proxied.append(entry)
        by_file[target] = entry
        if not args.filters or any(flt in target for flt in args.filters):
            files.append(target)
    files = sorted(set(files))
    if args.require_complete:
        missing = [f for f in tracked_sources() if f not in by_file]
        if missing:
            print("error: tracked sources not in the compile database (configure with every module "
                  "enabled, or add a HOST_PROXIES entry):\n  " + "\n  ".join(missing), file=sys.stderr)
            return 1
    if proxied:
        # clang-tidy reads flags from a compile database: write one that adds the proxied entries.
        tidy_dir = ROOT / args.build_dir / "clang-tidy-db"
        tidy_dir.mkdir(exist_ok=True)
        with (tidy_dir / "compile_commands.json").open("w", encoding="utf-8") as f:
            json.dump(db + proxied, f)
        db_path = tidy_dir / "compile_commands.json"
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
