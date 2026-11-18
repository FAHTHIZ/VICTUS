#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn the module+offset frames of a VICTUS crash report into function names and lines.

    python3 tools/crash/symbolize.py crash-1795007111-31174-victus.txt [--symbols DIR ...]

A report frame looks like
    #3 0x561057937271 /path/bin/victus+0x162a271 [build-id c3bb1d...]
and is resolved with llvm-symbolizer or addr2line against, in order:
  1. DIR/.build-id/<aa>/<rest>.debug   (the layout `objcopy --only-keep-debug` + GDB use)
  2. DIR/<module file name>.debug, DIR/<module file name>
  3. the module path written in the report, if it still exists
for every --symbols DIR (release archives ship a `symbols/` folder; see docs/DIAGNOSTICS.md).
A file is used only if its GNU build-id matches the report's, so a rebuilt binary can never
give wrong names silently. Windows reports (module+offset without build-id) need the matching
.pdb; this script prints them unchanged (use WinDbg or llvm-symbolizer --obj=<exe> manually).
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

FRAME = re.compile(r"^(#\d+)\s+(0x[0-9a-fA-F]+)\s+(.+?)\+(0x[0-9a-fA-F]+)(.*?)(?:\s*\[build-id ([0-9a-f]+)\])?\s*$")


def elf_build_id(path: str) -> str | None:
    """GNU build-id of an ELF file, read directly (no readelf needed)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if data[:4] != b"\x7fELF":
        return None
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    try:
        if is64:
            shoff, = struct.unpack_from(end + "Q", data, 0x28)
            shentsize, shnum = struct.unpack_from(end + "HH", data, 0x3A)
        else:
            shoff, = struct.unpack_from(end + "I", data, 0x20)
            shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
        for i in range(shnum):
            base = shoff + i * shentsize
            if is64:
                sh_type, = struct.unpack_from(end + "I", data, base + 4)
                off, size = struct.unpack_from(end + "QQ", data, base + 0x18)
            else:
                sh_type, = struct.unpack_from(end + "I", data, base + 4)
                off, size = struct.unpack_from(end + "II", data, base + 0x10)
            if sh_type != 7:  # SHT_NOTE
                continue
            pos = off
            while pos + 12 <= off + size:
                namesz, descsz, ntype = struct.unpack_from(end + "III", data, pos)
                name_start = pos + 12
                desc_start = name_start + ((namesz + 3) & ~3)
                if ntype == 3 and data[name_start:name_start + namesz].rstrip(b"\0") == b"GNU":
                    return data[desc_start:desc_start + descsz].hex()
                pos = desc_start + ((descsz + 3) & ~3)
    except struct.error:
        return None
    return None


def candidates(module: str, build_id: str | None, dirs: list[str]) -> list[str]:
    out = []
    base = os.path.basename(module.replace("\\", "/"))
    for d in dirs:
        if build_id and len(build_id) > 2:
            out.append(os.path.join(d, ".build-id", build_id[:2], build_id[2:] + ".debug"))
        out.append(os.path.join(d, base + ".debug"))
        out.append(os.path.join(d, base))
    out.append(module)
    return out


def find_symbols(module: str, build_id: str | None, dirs: list[str], cache: dict) -> tuple[str | None, str]:
    key = (module, build_id)
    if key in cache:
        return cache[key]
    result: tuple[str | None, str] = (None, "no symbol file found")
    for c in candidates(module, build_id, dirs):
        if not os.path.isfile(c):
            continue
        got = elf_build_id(c)
        if build_id and got != build_id:
            result = (None, f"build-id mismatch in {c}")
            continue
        result = (c, "")
        break
    cache[key] = result
    return result


def symbolizer() -> list[str] | None:
    for name in ("llvm-symbolizer", "llvm-symbolizer-20", "llvm-symbolizer-19", "llvm-symbolizer-18"):
        if shutil.which(name):
            return [name, "--functions=linkage", "--demangle", "--inlining"]
    if shutil.which("addr2line"):
        return ["addr2line", "-f", "-C", "-i"]
    return None


def resolve(tool: list[str], obj: str, offset: int) -> list[str]:
    # Frames are return addresses: look up the call instruction (address - 1).
    addr = hex(max(offset - 1, 0))
    if tool[0].startswith("llvm-symbolizer"):
        cmd = tool + [f"--obj={obj}", addr]
    else:
        cmd = tool + ["-e", obj, addr]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    lines = [l for l in out.splitlines() if l.strip()]
    pairs = []
    for i in range(0, len(lines) - 1, 2):
        func, loc = lines[i].strip(), lines[i + 1].strip()
        if func == "??" and loc.startswith("??"):
            continue
        pairs.append(f"{func} at {loc}")
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("report", help="crash report (.txt) written by VICTUS")
    ap.add_argument("--symbols", action="append", default=[], help="folder with debug files (repeatable)")
    args = ap.parse_args()
    tool = symbolizer()
    if tool is None:
        print("symbolize: neither llvm-symbolizer nor addr2line is installed", file=sys.stderr)
        return 2
    try:
        with open(args.report, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print(f"symbolize: {e}", file=sys.stderr)
        return 2
    cache: dict = {}
    resolved = unresolved = 0
    warned: set[str] = set()
    for line in lines:
        m = FRAME.match(line)
        if not m:
            print(line)
            continue
        index, _, module, offset, _, build_id = m.groups()
        obj, why = find_symbols(module, build_id, args.symbols, cache)
        names = resolve(tool, obj, int(offset, 16)) if obj else []
        if names:
            resolved += 1
            print(f"{line}\n    {names[0]}")
            for inl in names[1:]:
                print(f"    (inlined into) {inl}")
        else:
            unresolved += 1
            print(line)
            if obj is None and module not in warned:
                warned.add(module)
                print(f"    [{why} for {os.path.basename(module)}]")
    print(f"--- symbolized {resolved} of {resolved + unresolved} frames ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
