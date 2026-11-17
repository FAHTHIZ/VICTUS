#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks that this machine can build and test VICTUS, and explains what is missing.

  tools/setup/check_env.py

Exit status 0 when every required tool is present; 1 otherwise. Optional tools only print notes.
"""
from __future__ import annotations

import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


def version_of(cmd: list[str]) -> str | None:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", out.stdout + out.stderr)
    return m.group(0) if m else None


def at_least(v: str | None, minimum: tuple[int, ...]) -> bool:
    if not v:
        return False
    parts = tuple(int(x) for x in v.split("."))
    return parts[: len(minimum)] >= minimum


def main() -> int:
    system = platform.system()
    failures = 0
    rows: list[tuple[str, str, str]] = []

    def check(name: str, ok: bool, detail: str, required: bool = True) -> None:
        nonlocal failures
        status = "ok" if ok else ("MISSING" if required else "optional")
        if not ok and required:
            failures += 1
        rows.append((status, name, detail))

    py = ".".join(map(str, sys.version_info[:3]))
    check("Python >= 3.10", sys.version_info >= (3, 10), py)
    cm = version_of(["cmake", "--version"]) if shutil.which("cmake") else None
    check("CMake >= 3.24", at_least(cm, (3, 24)), cm or "not found")
    nj = version_of(["ninja", "--version"]) if shutil.which("ninja") else None
    check("Ninja (CMake presets use it)", nj is not None, nj or "not found", required=system != "Windows")
    check("git", shutil.which("git") is not None, shutil.which("git") or "not found")

    if system == "Windows":
        cl = shutil.which("cl")
        check("MSVC (Visual Studio 2022 17.8+, 'Desktop development with C++')", cl is not None,
              cl or "run from a Developer PowerShell, or let CMake find Visual Studio", required=False)
    else:
        gxx = version_of(["g++", "--version"]) if shutil.which("g++") else None
        clangxx = version_of(["clang++", "--version"]) if shutil.which("clang++") else None
        ok = at_least(gxx, (13,)) or at_least(clangxx, (17,))
        check("C++20 compiler (GCC >= 13 or Clang >= 17)", ok, f"g++ {gxx or '-'}, clang++ {clangxx or '-'}")

    if system == "Linux":
        icd_dirs = [Path("/usr/share/vulkan/icd.d"), Path("/etc/vulkan/icd.d")]
        icds = [p.name for d in icd_dirs if d.is_dir() for p in d.glob("*.json")]
        check("Vulkan driver (GPU tests; Mesa lavapipe is enough)", bool(icds), ", ".join(icds) or "none",
              required=False)
        headers = Path("/usr/include/X11/Xlib.h").exists() or Path("/usr/include/wayland-client.h").exists()
        check("X11 or Wayland development headers (SDL3)", headers,
              "found" if headers else "run tools/setup/install_deps.sh")
        check("xvfb-run (windowed smoke tests)", shutil.which("xvfb-run") is not None,
              shutil.which("xvfb-run") or "not found", required=False)

    glslang = shutil.which("glslangValidator")
    check("glslangValidator + spirv-cross (only to edit built-in shaders)", glslang is not None,
          glslang or "not needed unless you change engine/render/shaders", required=False)
    tidy = shutil.which("clang-tidy")
    check("clang-tidy (tools/lint/run_clang_tidy.py)", tidy is not None, tidy or "install with --tidy",
          required=False)

    free = shutil.disk_usage(Path.cwd()).free / 2**30
    check("Free disk >= 8 GiB (a Debug build with tests is ~5.5 GiB)", free >= 8, f"{free:.1f} GiB", required=False)

    width = max(len(r[1]) for r in rows)
    for status, name, detail in rows:
        print(f"[{status:>8}] {name:<{width}}  {detail}")
    if failures:
        print(f"\n{failures} required item(s) missing. See docs/GETTING_STARTED.md.")
        return 1
    print("\nReady: cmake --preset dev && cmake --build --preset dev && ctest --preset dev")
    return 0


if __name__ == "__main__":
    sys.exit(main())
