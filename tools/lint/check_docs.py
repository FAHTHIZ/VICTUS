#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Documentation drift checks (GAP-42, docs/audits/ENGINE_LANDSCAPE_AND_GAP_AUDIT.md).

1. Every engine module (engine/<name>/CMakeLists.txt) is named as `<name>` in the module map of
   docs/ARCHITECTURE.md.
2. Every ADR from ADR-0149 on (when CHANGELOG.md started tracking ADRs) is cited in CHANGELOG.md,
   unless it is listed in INTERNAL_ADRS below with the reason it is not user-visible.
3. No ADR number is used twice in docs/TECHNOLOGY_DECISIONS.md except the known historical
   duplicates (cite those with their topic).

Usage: check_docs.py [repo_root]. Exits 1 and lists every problem when a check fails.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

FIRST_TRACKED_ADR = 149
INTERNAL_ADRS = {
    153: "FP32 Lambert/environment-BRDF terms in the FP16 variant; CI threshold fix, no user-visible change",
}
KNOWN_DUPLICATE_ADRS = {8, 37, 38}

ADR_HEADING = re.compile(r"^#{2,3} ?ADR-(\d{4})", re.MULTILINE)


def module_map(architecture: str) -> str:
    start = architecture.find("## Module map")
    if start < 0:
        return ""
    end = architecture.find("\n## ", start + 1)
    return architecture[start:end if end >= 0 else len(architecture)]


def check(root: Path) -> list[str]:
    problems: list[str] = []

    arch = module_map((root / "docs/ARCHITECTURE.md").read_text(encoding="utf-8"))
    if not arch:
        problems.append("docs/ARCHITECTURE.md has no '## Module map' section")
    for cmake in sorted((root / "engine").glob("*/CMakeLists.txt")):
        name = cmake.parent.name
        if f"`{name}`" not in arch:
            problems.append(f"engine module '{name}' is missing from the ARCHITECTURE.md module map")

    decisions = (root / "docs/TECHNOLOGY_DECISIONS.md").read_text(encoding="utf-8")
    numbers = [int(n) for n in ADR_HEADING.findall(decisions)]
    for number, count in sorted(Counter(numbers).items()):
        if count > 1 and number not in KNOWN_DUPLICATE_ADRS:
            problems.append(f"ADR-{number:04d} is used by {count} headings in TECHNOLOGY_DECISIONS.md")

    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    cited = {int(n) for n in re.findall(r"ADR-(\d{4})", changelog)}
    for number in sorted(set(numbers)):
        if number < FIRST_TRACKED_ADR or number in INTERNAL_ADRS or number in cited:
            continue
        problems.append(
            f"ADR-{number:04d} is not cited in CHANGELOG.md (add it to the matching entry, or to "
            "INTERNAL_ADRS in tools/lint/check_docs.py with the reason it is not user-visible)")
    return problems


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    problems = check(root)
    for p in problems:
        print(f"check_docs: {p}")
    if problems:
        return 1
    print("check_docs: module map, ADR numbering and CHANGELOG citations are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
