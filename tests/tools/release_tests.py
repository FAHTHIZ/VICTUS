#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/release/release.py (version checks, changelog notes, packaging with split
symbols) and the symbol lookup in tools/crash/symbolize.py. Run: python3 tests/tools/release_tests.py"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # keep __pycache__ out of the source tree
sys.path.insert(0, str(ROOT / "tools" / "release"))
sys.path.insert(0, str(ROOT / "tools" / "crash"))
import release  # noqa: E402
import symbolize  # noqa: E402

CHANGELOG = """# Changelog

## [Unreleased]

### Added
- next thing

## [0.3.0] - 2026-12-01

### Added
- crash reports

### Fixed
- a bug

## [0.2.0] - 2026-11-30

- first

[Unreleased]: https://example.invalid
"""


def fake_root(tmp: Path, version: str = "0.3.0", changelog: str = CHANGELOG) -> Path:
    root = tmp / "src"
    root.mkdir()
    (root / "CMakeLists.txt").write_text(f"cmake_minimum_required(VERSION 3.24)\nproject(VICTUS VERSION {version} LANGUAGES C CXX)\n")
    (root / "CHANGELOG.md").write_text(changelog)
    (root / "LICENSE").write_text("license\n")
    (root / "README.md").write_text("readme\n")
    (root / "docs").mkdir()
    (root / "docs" / "USER_GUIDE.md").write_text("guide\n")
    (root / "samples" / "arena" / ".victus").mkdir(parents=True)
    (root / "samples" / "arena" / "victus.project").write_text("{}\n")
    (root / "samples" / "arena" / ".victus" / "cache.bin").write_text("cache\n")
    return root


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="victus_release_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_check_accepts_matching_tag(self):
        root = fake_root(self.tmp)
        self.assertEqual(release.check("v0.3.0", root), "0.3.0")

    def test_check_rejects_mismatches(self):
        root = fake_root(self.tmp)
        for tag, why in [("0.3.0", "look like"), ("v0.3", "semantic"), ("v0.4.0", "CMakeLists"), ("v0.3.0.1", "semantic")]:
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(release.ReleaseError, why):
                    release.check(tag, root)

    def test_check_needs_dated_nonempty_section(self):
        root = fake_root(self.tmp, changelog="# Changelog\n\n## [0.3.0]\n\n- x\n")
        with self.assertRaisesRegex(release.ReleaseError, "no release date"):
            release.check("v0.3.0", root)
        (root / "CHANGELOG.md").write_text("# Changelog\n\n## [0.3.0] - 2026-12-01\n\n## [0.2.0] - 2026-11-30\n- y\n")
        with self.assertRaisesRegex(release.ReleaseError, "empty"):
            release.check("v0.3.0", root)
        (root / "CHANGELOG.md").write_text("# Changelog\n")
        with self.assertRaisesRegex(release.ReleaseError, "no '## \\[0.3.0\\]"):
            release.check("v0.3.0", root)

    def test_prerelease_tag_uses_core_version(self):
        root = fake_root(self.tmp, changelog=CHANGELOG.replace("## [0.3.0] - 2026-12-01", "## [0.3.0-rc.1] - 2026-12-01"))
        self.assertEqual(release.check("v0.3.0-rc.1", root), "0.3.0-rc.1")

    def test_notes_are_the_section_body(self):
        root = fake_root(self.tmp)
        date, body = release.changelog_section("0.3.0", root)
        self.assertEqual(date, "2026-12-01")
        self.assertIn("- crash reports", body)
        self.assertIn("### Fixed", body)
        self.assertNotIn("first", body)
        _, last = release.changelog_section("0.2.0", root)
        self.assertEqual(last, "- first")  # link references are not part of the notes

    def test_repository_changelog_and_version_parse(self):
        self.assertRegex(release.cmake_version(), r"^\d+\.\d+\.\d+$")
        self.assertIn("## [Unreleased]", (ROOT / "CHANGELOG.md").read_text(encoding="utf-8"))


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cc") and shutil.which("objcopy"),
                     "needs a Linux C compiler and objcopy")
class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="victus_package_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def build_fake(self, names):
        build = self.tmp / "build"
        (build / "bin").mkdir(parents=True)
        src = self.tmp / "main.c"
        src.write_text("#include <stdio.h>\nstatic int helper(int x) { return x * 3; }\nint main(void) { printf(\"%d\\n\", helper(14)); return 0; }\n")
        for n in names:
            subprocess.run(["cc", "-g", "-O0", "-Wl,--build-id", str(src), "-o", str(build / "bin" / n)], check=True)
        (build / "engine" / "player").mkdir(parents=True)
        (build / "engine" / "player" / "THIRD_PARTY_NOTICES.txt").write_text("notices\n")
        return build

    def test_package_splits_symbols(self):
        root = fake_root(self.tmp)
        build = self.build_fake(["victus", "victus_player"])
        original_id = symbolize.elf_build_id(str(build / "bin" / "victus"))
        self.assertTrue(original_id)
        out = self.tmp / "dist"
        out.mkdir()
        with self.assertRaisesRegex(release.ReleaseError, "victus_editor not found"):
            release.package(build, "RelWithDebInfo", "0.3.0", "linux-x86_64", out, release.EXECUTABLES, True, root)
        made = release.package(build, "RelWithDebInfo", "0.3.0", "linux-x86_64", out, release.EXECUTABLES, False, root)
        self.assertEqual([m.name for m in made], ["victus-0.3.0-linux-x86_64.tar.gz", "victus-0.3.0-linux-x86_64-symbols.tar.gz"])

        stage = out / "victus-0.3.0-linux-x86_64"
        syms = out / "victus-0.3.0-linux-x86_64-symbols"
        exe = stage / "bin" / "victus"
        self.assertTrue(os.access(exe, os.X_OK))
        self.assertEqual(subprocess.run([str(exe)], capture_output=True, text=True).stdout, "42\n")  # still runs
        sections = subprocess.run(["objdump", "-h", str(exe)], capture_output=True, text=True).stdout if shutil.which("objdump") else ""
        if sections:
            self.assertNotIn(".debug_info", sections)
            self.assertIn(".gnu_debuglink", sections)
        self.assertEqual(symbolize.elf_build_id(str(exe)), original_id)  # same build-id after stripping
        debug = syms / ".build-id" / original_id[:2] / (original_id[2:] + ".debug")
        self.assertTrue(debug.is_file())
        self.assertEqual(symbolize.elf_build_id(str(debug)), original_id)
        self.assertFalse((syms / "victus.debug").exists())  # one copy only
        self.assertIn(f"{original_id[2:]}.debug (victus)", (syms / "README.txt").read_text())

        for f in ["LICENSE", "README.md", "CHANGELOG.md", "THIRD_PARTY_NOTICES.txt", "docs/USER_GUIDE.md",
                  "samples/arena/victus.project", "RELEASE_NOTES.md", "bin/victus_player"]:
            self.assertTrue((stage / f).is_file(), f)
        self.assertFalse((stage / "samples" / "arena" / ".victus").exists())
        self.assertIn("- crash reports", (stage / "RELEASE_NOTES.md").read_text())

        with tarfile.open(made[0]) as t:
            names = t.getnames()
        self.assertIn("victus-0.3.0-linux-x86_64/bin/victus", names)
        sums = (out / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual(len(sums), 2)
        for line in sums:
            digest, name = line.split("  ")
            self.assertEqual(hashlib.sha256((out / name).read_bytes()).hexdigest(), digest)

        # The symbolizer finds the split debug file by build-id and refuses a mismatched one.
        cache: dict = {}
        found, _ = symbolize.find_symbols("/gone/victus", original_id, [str(syms)], cache)
        self.assertEqual(found, str(debug))
        other = self.tmp / "other_symbols"
        other.mkdir()
        shutil.copy2(debug, other / "victus.debug")  # right name, wrong build
        found, why = symbolize.find_symbols("/gone/victus", "00" * 20, [str(other)], {})
        self.assertIsNone(found)
        self.assertIn("mismatch", why)


class BenchmarkComparisonTests(unittest.TestCase):
    def test_comparison_regressions(self):
        test_script = ROOT / "tests" / "perf" / "test_compare_bench.py"
        comparator = ROOT / "tools" / "perf" / "compare_bench.py"
        result = subprocess.run(
            [sys.executable, str(test_script), str(comparator)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
