# SPDX-License-Identifier: Apache-2.0
"""Regression tests for the benchmark capture comparison CLI."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(sys.argv[1])
BASE = {
    'schema': 'victus.bench.v1',
    'machine': {
        'platform': 'test',
        'architecture': 'test',
        'compiler': 'test',
        'build_type': 'Release',
        'hardware_threads': 4,
        'allocation_tracking': False,
    },
    'config': {'warmup': 0, 'repetitions': 15},
    'cases': [{'name': 'sample', 'items_per_rep': 8, 'p50_ms': 10, 'p95_ms': 20}],
}


class ComparisonTests(unittest.TestCase):
    def test_comparison_and_validation(self):
        scenarios = [
            ('unchanged', lambda data: None, 0),
            ('threshold equality', lambda data: data['cases'][0].update(p95_ms=22), 0),
            ('p95 regression', lambda data: data['cases'][0].update(p95_ms=23), 1),
            ('median regression', lambda data: data['cases'][0].update(p50_ms=12), 1),
            ('improvement', lambda data: data['cases'][0].update(p50_ms=7), 0),
            ('machine mismatch', lambda data: data['machine'].update(hardware_threads=8), 2),
            ('repetition mismatch', lambda data: data['config'].update(repetitions=3), 2),
            ('workload mismatch', lambda data: data['cases'][0].update(items_per_rep=32), 2),
            ('case mismatch', lambda data: data['cases'][0].update(name='other'), 2),
            ('duplicate case', lambda data: data['cases'].append(data['cases'][0]), 2),
            ('empty capture', lambda data: data['cases'].clear(), 2),
            ('non-finite metric', lambda data: data['cases'][0].update(p95_ms=float('nan')), 2),
            ('missing metric', lambda data: data['cases'][0].pop('p50_ms'), 2),
        ]
        with tempfile.TemporaryDirectory() as folder:
            before = Path(folder) / 'before.json'
            after = Path(folder) / 'after.json'
            before.write_text(json.dumps(BASE), encoding='utf-8')
            for name, change, expected in scenarios:
                with self.subTest(name=name):
                    report = copy.deepcopy(BASE)
                    change(report)
                    after.write_text(json.dumps(report), encoding='utf-8')
                    result = subprocess.run(
                        [sys.executable, str(TOOL), str(before), str(after), '--limit-pct', '10'],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)
                    if expected == 2:
                        self.assertIn('cannot compare captures:', result.stderr)
                    else:
                        self.assertIn('sample |', result.stdout)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
