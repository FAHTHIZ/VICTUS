#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare two victus.bench.v1 captures from the same device.
Exit 0: reported; 1: a timing crossed the optional limit; 2: incomparable.
Control GPU, driver, thermals and background load separately. Repeat any regression.
"""
import argparse
import json
import math
from pathlib import Path


def load(path):
    report = json.loads(path.read_text(encoding='utf-8'))
    if report['schema'] != 'victus.bench.v1':
        raise ValueError('unsupported benchmark schema')
    machine = report['machine']
    keys = ('platform', 'architecture', 'compiler', 'build_type', 'hardware_threads', 'allocation_tracking')
    settings = {key: machine[key] for key in keys}
    settings['cpu'] = machine.get('cpu')
    for key, minimum in [('warmup', 0), ('repetitions', 1)]:
        value = report['config'][key]
        if type(value) is not int or value < minimum:
            raise ValueError(f'invalid {key}')
        settings[key] = value
    rows = {}
    for case in report['cases']:
        name = case['name']
        if not isinstance(name, str) or not name or name in rows:
            raise ValueError(f'invalid or duplicate case name: {name!r}')
        if type(case['items_per_rep']) is not int or case['items_per_rep'] < 0:
            raise ValueError(f'invalid workload for {name}')
        for key in ('p50_ms', 'p95_ms'):
            value = case[key]
            if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
                raise ValueError(f'invalid {key} for {name}')
        if case['p95_ms'] < case['p50_ms']:
            raise ValueError(f'reversed percentiles for {name}')
        rows[name] = case
    if not rows:
        raise ValueError('empty capture')
    return settings, rows


def compare(before, after, limit):
    a, left = load(before)
    b, right = load(after)
    mismatch = [k for k in a if a[k] != b[k]]
    if mismatch:
        raise ValueError('capture settings differ: ' + ', '.join(mismatch))
    if left.keys() != right.keys():
        raise ValueError('case sets differ: ' + ', '.join(sorted(left.keys() ^ right.keys())))
    changed = [n for n in left if left[n]['items_per_rep'] != right[n]['items_per_rep']]
    if changed:
        raise ValueError('workload changed: ' + ', '.join(changed))
    failed = False
    print('case | median ms (before -> after) | p95 ms (before -> after)')
    for name in sorted(left):
        cells = [name]
        for key in ('p50_ms', 'p95_ms'):
            old, new = left[name][key], right[name][key]
            delta = (new - old) / old * 100
            exceeded = limit is not None and delta > limit and not math.isclose(delta, limit, rel_tol=1e-12)
            failed |= exceeded
            marker = ' *' if exceeded else ''
            cells.append(f'{old:.6g} -> {new:.6g} ({delta:+.2f}%){marker}')
        print(' | '.join(cells))
    return int(failed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--limit-pct', type=float, help='fail above this percent increase in median or p95')
    args = parser.parse_args()
    if args.limit_pct is not None and not (math.isfinite(args.limit_pct) and args.limit_pct >= 0):
        parser.error('--limit-pct must be finite and non-negative')
    try:
        raise SystemExit(compare(args.before, args.after, args.limit_pct))
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
        parser.exit(2, f'cannot compare captures: {error}\n')
