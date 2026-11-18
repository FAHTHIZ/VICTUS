#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""VICTUS release helper (docs/RELEASE_PROCESS.md). Standard library only.

  release.py check --tag v0.3.0            tag, CMake project VERSION and CHANGELOG agree
  release.py notes 0.3.0                   print that version's CHANGELOG section
  release.py package --build build --config RelWithDebInfo --version 0.3.0 --platform linux-x86_64 --out dist
                                           stage binaries + docs, split debug symbols into a
                                           separate archive, write SHA256SUMS.txt

Packaging produces, in --out:
  victus-<version>-<platform>.tar.gz|zip          bin/ (stripped), samples/, LICENSE, notices, docs
  victus-<version>-<platform>-symbols.tar.gz|zip  Linux: .build-id/aa/rest.debug
                                                  macOS: <exe>.dSYM, Windows: <exe>.pdb
  SHA256SUMS.txt (appended, one line per archive)
Symbols are what tools/crash/symbolize.py needs to name frames in crash reports.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXECUTABLES = ["victus", "victus_editor", "victus_player"]
DOCS = ["README.md", "LICENSE", "CHANGELOG.md", "SECURITY.md"]
GUIDES = ["GETTING_STARTED.md", "USER_GUIDE.md", "DIAGNOSTICS.md", "KNOWN_LIMITATIONS.md", "PLATFORM_SUPPORT.md"]
SAMPLES = ["arena", "dungeon_crawler"]
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?$")


class ReleaseError(Exception):
    pass


def cmake_version(root: Path = ROOT) -> str:
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(\s*VICTUS\s+VERSION\s+([0-9.]+)", text)
    if not m:
        raise ReleaseError("CMakeLists.txt has no project(VICTUS VERSION x.y.z)")
    return m.group(1)


def changelog_section(version: str, root: Path = ROOT) -> tuple[str, str]:
    """(date, body) of `## [version] - YYYY-MM-DD` in CHANGELOG.md."""
    path = root / "CHANGELOG.md"
    if not path.is_file():
        raise ReleaseError("CHANGELOG.md is missing")
    lines = path.read_text(encoding="utf-8").splitlines()
    head = re.compile(r"^## \[" + re.escape(version) + r"\](?: - (\d{4}-\d{2}-\d{2}))?\s*$")
    for i, line in enumerate(lines):
        m = head.match(line)
        if not m:
            continue
        body = []
        for nxt in lines[i + 1:]:
            if nxt.startswith("## ") or re.match(r"^\[[^\]]+\]: ", nxt):
                break
            body.append(nxt)
        return (m.group(1) or "", "\n".join(body).strip())
    raise ReleaseError(f"CHANGELOG.md has no '## [{version}] - YYYY-MM-DD' section")


def check(tag: str, root: Path = ROOT) -> str:
    if not tag.startswith("v"):
        raise ReleaseError(f"release tags look like v1.2.3, got '{tag}'")
    version = tag[1:]
    m = SEMVER.match(version)
    if not m:
        raise ReleaseError(f"'{version}' is not a semantic version")
    core = ".".join(m.groups()[:3])
    have = cmake_version(root)
    if have != core:
        raise ReleaseError(f"tag {tag} but CMakeLists.txt says VERSION {have}")
    date, body = changelog_section(version, root)
    if not date:
        raise ReleaseError(f"CHANGELOG.md section [{version}] has no release date")
    if not body:
        raise ReleaseError(f"CHANGELOG.md section [{version}] is empty")
    return version


def find_binary(build: Path, config: str, name: str, windows: bool) -> Path | None:
    exe = name + (".exe" if windows else "")
    for d in (build / "bin" / config, build / "bin"):
        if (d / exe).is_file():
            return d / exe
    return None


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise ReleaseError(f"{' '.join(cmd)} failed:\n{r.stdout}{r.stderr}")


def elf_build_id(path: Path) -> str | None:
    sys.path.insert(0, str(ROOT / "tools" / "crash"))
    sys.dont_write_bytecode = True
    try:
        from symbolize import elf_build_id as read_id  # type: ignore
    finally:
        sys.path.pop(0)
    return read_id(str(path))


def split_symbols(exe: Path, staged: Path, symbols: Path, kind: str) -> list[str]:
    """Copy `exe` to `staged` without debug info; debug info goes under `symbols`."""
    produced = []
    if kind == "linux":
        objcopy = shutil.which("objcopy")
        if not objcopy:
            raise ReleaseError("objcopy is required to split Linux symbols")
        # One copy, in the .build-id layout GDB, LLDB and tools/crash/symbolize.py search;
        # <exe>.debug only when the binary has no build-id.
        bid = elf_build_id(exe)
        debug = symbols / ".build-id" / bid[:2] / (bid[2:] + ".debug") if bid and len(bid) > 2 else symbols / (exe.name + ".debug")
        debug.parent.mkdir(parents=True, exist_ok=True)
        run([objcopy, "--only-keep-debug", str(exe), str(debug)])
        run([objcopy, "--strip-debug", "--add-gnu-debuglink=" + str(debug), str(exe), str(staged)])
        produced.append(f"{debug.relative_to(symbols).as_posix()} ({exe.name})")
        os.chmod(staged, 0o755)
    elif kind == "macos":
        dsym = symbols / (exe.name + ".dSYM")
        run(["dsymutil", str(exe), "-o", str(dsym)])
        shutil.copy2(exe, staged)
        run(["strip", "-S", str(staged)])
        produced.append(dsym.name)
    else:
        shutil.copy2(exe, staged)
        pdb = exe.with_suffix(".pdb")
        if pdb.is_file():
            shutil.copy2(pdb, symbols / pdb.name)
            produced.append(pdb.name)
    return produced


def archive(src: Path, out_dir: Path, use_zip: bool) -> Path:
    if use_zip:
        dest = out_dir / (src.name + ".zip")
        with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted(src.rglob("*")):
                z.write(p, Path(src.name) / p.relative_to(src))
    else:
        dest = out_dir / (src.name + ".tar.gz")
        with tarfile.open(dest, "w:gz") as t:
            t.add(src, arcname=src.name)
    return dest


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def package(build: Path, config: str, version: str, platform: str, out: Path, executables: list[str],
            require_all: bool, root: Path = ROOT) -> list[Path]:
    kind = "windows" if platform.startswith("windows") else "macos" if platform.startswith("macos") else "linux"
    name = f"victus-{version}-{platform}"
    stage = out / name
    syms = out / (name + "-symbols")
    for d in (stage, syms):
        if d.exists():
            shutil.rmtree(d)
    (stage / "bin").mkdir(parents=True)
    syms.mkdir(parents=True)

    found = 0
    symbol_files: list[str] = []
    for exe_name in executables:
        exe = find_binary(build, config, exe_name, kind == "windows")
        if exe is None:
            if require_all:
                raise ReleaseError(f"{exe_name} not found under {build}/bin")
            print(f"release: skipping {exe_name} (not built)")
            continue
        symbol_files += split_symbols(exe, stage / "bin" / exe.name, syms, kind)
        found += 1
    if found == 0:
        raise ReleaseError(f"no executables found under {build}/bin")
    if kind == "windows":  # runtime DLLs next to the executables (SDL3 when linked shared)
        exe_dir = find_binary(build, config, executables[0], True)
        for dll in (exe_dir.parent.glob("*.dll") if exe_dir else []):
            shutil.copy2(dll, stage / "bin" / dll.name)

    for doc in DOCS:
        if (root / doc).is_file():
            shutil.copy2(root / doc, stage / doc)
    (stage / "docs").mkdir()
    for g in GUIDES:
        if (root / "docs" / g).is_file():
            shutil.copy2(root / "docs" / g, stage / "docs" / g)
    notices = build / "engine" / "player" / "THIRD_PARTY_NOTICES.txt"
    if notices.is_file():
        shutil.copy2(notices, stage / "THIRD_PARTY_NOTICES.txt")
    elif require_all:
        raise ReleaseError(f"{notices} is missing (configure the build first)")
    for s in SAMPLES:
        if (root / "samples" / s).is_dir():
            shutil.copytree(root / "samples" / s, stage / "samples" / s,
                            ignore=shutil.ignore_patterns(".victus", "*.vpak", "build"))
    try:
        _, notes = changelog_section(version, root)
    except ReleaseError:
        notes = "(no CHANGELOG section for this version)"
    (stage / "RELEASE_NOTES.md").write_text(f"# VICTUS {version} ({platform})\n\n{notes}\n\n"
                                            "Samples: point the editor at them with VICTUS_SAMPLES_DIR=<this folder>/samples.\n"
                                            f"Debug symbols for crash reports: {name}-symbols archive "
                                            "(see docs/DIAGNOSTICS.md).\n", encoding="utf-8")
    (syms / "README.txt").write_text(
        f"Debug symbols for VICTUS {version} ({platform}). Keep them for as long as this build is in use.\n"
        "Resolve a crash report: python3 tools/crash/symbolize.py crash.txt --symbols <this folder>\n"
        "Files: " + ", ".join(symbol_files) + "\n", encoding="utf-8")

    use_zip = kind == "windows"
    made = [archive(stage, out, use_zip), archive(syms, out, use_zip)]
    with open(out / "SHA256SUMS.txt", "a", encoding="utf-8") as f:
        for a in made:
            f.write(f"{sha256(a)}  {a.name}\n")
    return made


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="VICTUS release helper")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--tag", required=True)
    n = sub.add_parser("notes")
    n.add_argument("version")
    p = sub.add_parser("package")
    p.add_argument("--build", required=True, type=Path)
    p.add_argument("--config", default="RelWithDebInfo")
    p.add_argument("--version", required=True)
    p.add_argument("--platform", required=True, help="e.g. linux-x86_64, windows-x86_64, macos-arm64")
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--exe", action="append", help="executable to ship (default: victus, victus_editor, victus_player)")
    p.add_argument("--allow-missing", action="store_true", help="skip executables that were not built")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "check":
            print(f"release {check(args.tag)}: tag, CMakeLists.txt and CHANGELOG.md agree")
        elif args.cmd == "notes":
            print(changelog_section(args.version)[1])
        else:
            args.out.mkdir(parents=True, exist_ok=True)
            for a in package(args.build, args.config, args.version, args.platform, args.out,
                             args.exe or EXECUTABLES, not args.allow_missing):
                print(f"wrote {a} ({a.stat().st_size} bytes)")
    except ReleaseError as e:
        print(f"release: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
