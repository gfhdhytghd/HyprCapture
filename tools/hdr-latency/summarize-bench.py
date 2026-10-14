#!/usr/bin/env python3
"""Summarize complete five-trial CSVs, excluding trial 0 from warm statistics."""
import csv
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
result = {}
for name in ('ui', 'gpu'):
    groups = {}
    with (root / f'{name}.csv').open() as f:
        for row in csv.DictReader(f):
            groups.setdefault((row['size'], row['event']), []).append((int(row['trial']), float(row['ms'])))
    result[name] = {}
    for (size, event), trials in groups.items():
        warm = [value for trial, value in trials if trial > 0]
        if warm and sorted(trial for trial, _ in trials) != [0, 1, 2, 3, 4]:
            raise SystemExit(f'Incomplete trials: {name}/{size}/{event}')
        stats = dict(cold_ms=next((v for t, v in trials if t <= 0), None))
        if warm:
            stats.update(warm_trials=len(warm), median_ms=statistics.median(warm), min_ms=min(warm), max_ms=max(warm))
        result[name].setdefault(size, {})[event] = stats
(root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
