#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""VICTUS third-party dependency audit.

Single source of truth for fetched dependencies is cmake/VictusDependencies.cmake (FetchContent_Declare
blocks); tools/deps/dependencies.json adds licenses, vendored third_party/ sources and reviewed advisories. Commands:

  check            every fetched dependency is pinned by URL + SHA-256, every dependency is recorded in
                   docs/THIRD_PARTY_AND_LICENSES.md with its exact version, vendored files exist.
  sbom [--out F]   write a CycloneDX 1.6 JSON SBOM (stdout by default).
  osv              query https://api.osv.dev for known vulnerabilities of each pinned version (GIT
                   ecosystem by tag or commit). Advisories in tools/deps/dependencies.json "osv_ignore" (each
                   with the mitigation) are reported but do not fail. Needs network access.
  list             print the dependency table.

Exit status is non-zero on any failed check, so the script is used directly as a CI gate.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE_DEPS = ROOT / "cmake" / "VictusDependencies.cmake"
LICENSE_DOC = ROOT / "docs" / "THIRD_PARTY_AND_LICENSES.md"
DATA = Path(__file__).resolve().parent / "dependencies.json"
_DATA = json.loads(DATA.read_text(encoding="utf-8"))
FETCHED_LICENSES: dict[str, str] = _DATA["fetched_licenses"]
DOC_NAMES: dict[str, str] = _DATA["doc_names"]



@dataclass
class Dependency:
    name: str
    version: str
    license: str
    repo: str  # https://github.com/<owner>/<repo>
    origin: str  # "fetched" or "vendored"
    url: str = ""
    sha256: str = ""
    commit: str = ""
    path: str = ""
    files: list[str] = field(default_factory=list)

    @property
    def doc_name(self) -> str:
        return DOC_NAMES.get(self.name, self.name)



VENDORED = [Dependency(v["name"], v["version"], v["license"], v["repo"], "vendored", commit=v.get("commit", ""),
                       path=v["path"], files=list(v["files"])) for v in _DATA["vendored"]]

_DECL = re.compile(r"FetchContent_Declare\(\s*(\S+)(.*?)\)", re.S)
_URL = re.compile(r"\bURL\s+(\S+)")
_HASH = re.compile(r"\bURL_HASH\s+SHA256=([0-9a-fA-F]{64})")
_GITHUB = re.compile(r"https://github\.com/([^/]+)/([^/]+)/")


def _version_from_url(url: str) -> tuple[str, str]:
    """Returns (tag-or-version, commit) parsed from a GitHub archive/release URL."""
    m = re.search(r"/archive/refs/tags/(.+?)\.tar\.gz$", url)
    if m:
        return m.group(1), ""
    m = re.search(r"/releases/download/([^/]+)/", url)
    if m:
        return m.group(1), ""
    m = re.search(r"/archive/([0-9a-f]{40})\.tar\.gz$", url)
    if m:
        return m.group(1), m.group(1)
    return "", ""


def fetched_dependencies() -> tuple[list[Dependency], list[str]]:
    text = CMAKE_DEPS.read_text(encoding="utf-8")
    deps, errors = [], []
    for m in _DECL.finditer(text):
        name, body = m.group(1), m.group(2)
        url_m, hash_m = _URL.search(body), _HASH.search(body)
        if not url_m:
            errors.append(f"{name}: FetchContent_Declare without a URL (git clones are not reproducible)")
            continue
        if re.search(r"\bGIT_REPOSITORY\b", body):
            errors.append(f"{name}: uses GIT_REPOSITORY; pin a release tarball with URL_HASH instead")
        if not hash_m:
            errors.append(f"{name}: URL without URL_HASH SHA256=")
        url = url_m.group(1)
        version, commit = _version_from_url(url)
        if not version:
            errors.append(f"{name}: cannot derive a version from {url}")
        gh = _GITHUB.match(url)
        repo = f"https://github.com/{gh.group(1)}/{gh.group(2)}" if gh else ""
        lic = FETCHED_LICENSES.get(name)
        if lic is None:
            errors.append(f"{name}: no verified SPDX license in tools/deps/dependencies.json fetched_licenses")
        deps.append(Dependency(name, version, lic or "NOASSERTION", repo, "fetched", url=url,
                               sha256=hash_m.group(1).lower() if hash_m else "", commit=commit))
    if not deps:
        errors.append(f"no FetchContent_Declare blocks found in {CMAKE_DEPS}")
    return deps, errors


def all_dependencies() -> tuple[list[Dependency], list[str]]:
    deps, errors = fetched_dependencies()
    return deps + VENDORED, errors


def _plain_version(v: str) -> str:
    """'v1.3.18' -> '1.3.18', 'release-3.4.20' -> '3.4.20', 'vulkan-sdk-1.4.363.0' kept."""
    v = re.sub(r"^release-", "", v)
    return v[1:] if re.match(r"^v\d", v) else v


def cmd_check(_: argparse.Namespace) -> int:
    deps, errors = all_dependencies()
    doc = LICENSE_DOC.read_text(encoding="utf-8")
    rows = [line for line in doc.splitlines() if line.startswith("| ")]
    for d in deps:
        row = next((r for r in rows if r.split("|")[1].strip().startswith(d.doc_name)), None)
        if row is None:
            errors.append(f"{d.name}: no row starting with '{d.doc_name}' in {LICENSE_DOC.relative_to(ROOT)}")
            continue
        wanted = d.commit or _plain_version(d.version)
        if wanted not in row:
            errors.append(f"{d.name}: version '{wanted}' not mentioned in its license-table row (stale docs?)")
        if d.sha256 and d.sha256 not in row:
            errors.append(f"{d.name}: SHA-256 {d.sha256} not recorded in its license-table row")
        for f in d.files:
            if not (ROOT / d.path / f).is_file():
                errors.append(f"{d.name}: vendored file {d.path}/{f} is missing")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{len(deps)} dependencies checked, {len(errors)} problem(s)")
    return 1 if errors else 0


def _purl(d: Dependency) -> str:
    m = re.match(r"https://github\.com/([^/]+)/([^/]+)", d.repo)
    if not m:
        return ""
    return f"pkg:github/{m.group(1).lower()}/{m.group(2).lower()}@{d.commit or d.version}"


def build_sbom(deps: list[Dependency]) -> dict:
    version = re.search(r"project\(VICTUS VERSION (\S+)", (ROOT / "CMakeLists.txt").read_text()).group(1)
    comps = []
    for d in deps:
        c = {
            "type": "library" if "font" not in d.name.lower() else "data",
            "bom-ref": f"{d.origin}:{d.name}",
            "name": d.name,
            "version": d.version,
            "licenses": [{"expression": d.license}],
            "externalReferences": [{"type": "vcs", "url": d.repo}] if d.repo else [],
            "properties": [{"name": "victus:origin", "value": d.origin}],
        }
        if d.url:
            c["externalReferences"].append({"type": "distribution", "url": d.url,
                                            "hashes": [{"alg": "SHA-256", "content": d.sha256}]})
        if d.path:
            c["properties"].append({"name": "victus:path", "value": d.path})
        purl = _purl(d)
        if purl:
            c["purl"] = purl
        comps.append(c)
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'victus-' + version + json.dumps([d.name + d.version for d in deps]))}",
        "version": 1,
        "metadata": {"component": {"type": "application", "bom-ref": "victus", "name": "VICTUS", "version": version,
                                   "licenses": [{"license": {"id": "Apache-2.0"}}]}},
        "components": comps,
        "dependencies": [{"ref": "victus", "dependsOn": [c["bom-ref"] for c in comps]}],
    }


def cmd_sbom(args: argparse.Namespace) -> int:
    deps, errors = all_dependencies()
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 1
    text = json.dumps(build_sbom(deps), indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"wrote {args.out} ({len(deps)} components)")
    else:
        sys.stdout.write(text)
    return 0


def _osv_query(d: Dependency) -> list[dict]:
    query: dict = {"package": {"name": d.repo + ".git", "ecosystem": "GIT"}}
    if d.commit:
        query = {"commit": d.commit}
    else:
        query["version"] = d.version
    req = urllib.request.Request("https://api.osv.dev/v1/query", data=json.dumps(query).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp).get("vulns", [])


def cmd_osv(_: argparse.Namespace) -> int:
    deps, errors = all_dependencies()
    allow: dict[str, str] = _DATA.get("osv_ignore", {})
    failing = 0
    for d in deps:
        if not d.repo or (not d.version and not d.commit):
            continue
        try:
            vulns = _osv_query(d)
        except (urllib.error.URLError, TimeoutError) as e:
            print(f"error: {d.name}: OSV query failed: {e}", file=sys.stderr)
            return 2
        for v in vulns:
            vid = v.get("id", "?")
            why = allow.get(vid)
            status = f"allowed ({why})" if why else "FAIL"
            failing += 0 if why else 1
            print(f"{d.name} {d.version}: {vid} {v.get('summary', '')} -> {status}")
        if not vulns:
            print(f"{d.name} {d.version}: no known vulnerabilities")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    return 1 if failing or errors else 0


def cmd_list(_: argparse.Namespace) -> int:
    deps, errors = all_dependencies()
    for d in deps:
        print(f"{d.origin:9} {d.name:18} {d.version:44} {d.license}")
    return 1 if errors else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check").set_defaults(fn=cmd_check)
    s = sub.add_parser("sbom")
    s.add_argument("--out")
    s.set_defaults(fn=cmd_sbom)
    sub.add_parser("osv").set_defaults(fn=cmd_osv)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    args = p.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
