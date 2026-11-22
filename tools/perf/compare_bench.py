#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0

"""Compare victus\_bench\_\* --json captures from the same machine and settings.

Usage: python3 tools/perf/compare\_bench.py before.json after.json \[--limit-pct 10\]
Positive changes mean slower. Exit 0: reported; 1: a timing exceeded the
optional tolerance; 2: invalid inputs or incomparable captures. A threshold
crossing is not a significance test: repeat the run to check measurement noise.
GPU identity, driver, thermals and background load must be controlled separately.
"""
import argparse
import json
import math
import sys
from pathlib import Path

METRICS = ('p50\_ms', 'p95\_ms')

def load(path):
    report = json.loads(path.read\_text(encoding='utf-8'))
    if report\['schema'\] != 'victus.bench.v1':
        raise ValueError(f'{path}: unsupported schema')
    machine = report\['machine'\]
    keys = ('platform', 'architecture', 'compiler', 'build\_type', 'hardware\_threads', 'allocation\_tracking')
    settings = {k: machine\[k\] for k in keys}
    settings\['cpu'\] = machine.get('cpu')
    for k, minimum in \[('warmup', 0), ('repetitions', 1)\]:
        n = report\['config'\]\[k\]
        if type(n) is not int or n &lt; minimum:
            raise ValueError(f'{path}: invalid {k}')
        settings\[k\] = n
    rows = {}
    for case in report\['cases'\]:
        name = case\['name'\]
        if not isinstance(name, str) or not name or name in rows:
            raise ValueError(f'{path}: invalid or duplicate case name: {name!r}')
        if type(case\['items\_per\_rep'\]) is not int or case\['items\_per\_rep'\] &lt; 0:
            raise ValueError(f'{path}: invalid workload for {name}')
        for k in METRICS:
            v = case\[k\]
            if type(v) not in (int, float) or not math.isfinite(v) or v &lt;= 0:
                raise ValueError(f'{path}: {name} has no usable {k}')
        if case\['p95\_ms'\] &lt; case\['p50\_ms'\]:
            raise ValueError(f'{path}: reversed percentiles for {name}')
        rows\[name\] = case
    if not rows:
        raise ValueError(f'{path}: empty capture')
    return settings, rows

def compare(before, after, limit):
    a, left = load(before)
    b, right = load(after)
    mismatches = \[k for k in a if a\[k\] != b\[k\]\]
    if mismatches:
        raise ValueError('capture settings differ: ' + ', '.join(mismatches))
    if left.keys() != right.keys():
        raise ValueError('different case sets: ' + ', '.join(sorted(left.keys() ^ right.keys())))
    changed = \[n for n in left if left\[n\]\['items\_per\_rep'\] != right\[n\]\['items\_per\_rep'\]\]
    if changed:
        raise ValueError('workload changed: ' + ', '.join(changed))
    over = False
    print('case | median ms (before -&gt; after) | p95 ms (before -&gt; after)')
    for name in sorted(left):
        cells = \[name\]
        for metric in METRICS:
            old, new = left\[name\]\[metric\], right\[name\]\[metric\]
            delta = (new - old) / old \* 100
            exceeded = limit is not None and delta &gt; limit and not math.isclose(delta, limit, rel\_tol=1e-12)
            over |= exceeded
            cells.append(f'{old:.6g} -&gt; {new:.6g} ({delta:+.2f}%){" \*" if exceeded else ""}')
        print(' | '.join(cells))
    return int(over)

if **name** == '**main**':
    parser = argparse.ArgumentParser(description=**doc**, formatter\_class=argparse.RawDescriptionHelpFormatter)
    parser.add\_argument('before', type=Path)
    parser.add\_argument('after', type=Path)
    parser.add\_argument('--limit-pct', type=float, help='fail if either timing increases by more than this percent')
    args = parser.parse\_args()
    if args.limit\_pct is not None and not (math.isfinite(args.limit\_pct) and args.limit\_pct &gt;= 0):
        parser.error('--limit-pct must be finite and non-negative')
    try:
        sys.exit(compare(args.before, args.after, args.limit\_pct))
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
        parser.exit(2, f'cannot compare captures: {error}\\n')