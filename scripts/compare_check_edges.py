#!/usr/bin/env python3
"""Compare independent edge-check fixtures with OpenSTA, including path exceptions."""
from pathlib import Path
import math
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
msta, opensta, output = sys.argv[1:]
case = root / 'testcases' / 'check_edges'
output = Path(output) / 'check_edges'
output.mkdir(parents=True, exist_ok=True)
for variant in ('normal', 'swapped', 'rise_only', 'data_1d', 'negative', 'cut_rise', 'cut_fall',
                'to_rise', 'to_fall', 'from_rise', 'from_fall',
                'clock_to_rise', 'clock_to_fall', 'output_clock_to_rise', 'output_clock_to_fall', 'async_phase', 'gating_phase'):
    env = dict(os.environ)
    env['CHECK_EDGE_LIB'] = variant if (case/f'{variant}.lib').exists() else 'normal'
    env['CHECK_EDGE_SDC'] = variant if (case/f'{variant}.sdc').exists() else 'common'
    mini = subprocess.run([msta, '-q', str(case / f'{variant}.dofile')],
                          check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    reference = subprocess.run([opensta, '-exit', str(case / (f'{variant}.tcl' if variant in ('async_phase', 'gating_phase') else 'opensta.tcl'))], env=env,
                               check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    (output / f'{variant}_msta.log').write_text(mini)
    (output / f'{variant}_opensta.log').write_text(reference)
    for kind, corner in (('setup', 'max'), ('hold', 'min')):
        match = re.search(rf'^{kind}\s*: WNS\s+([-\d.]+)', mini, re.M)
        if match is None and f'{kind} : 没有一条可分析的路径' not in mini and f'{kind}  : 没有一条可分析的路径' not in mini:
            raise SystemExit(f'{variant}: missing miniSTA {kind} result')
        actual = float(match.group(1)) if match else math.inf
        expected = float(re.search(rf'worst slack {corner}\s+([-\d.]+|inf)', reference,re.I).group(1))
        if actual != expected and (not math.isfinite(actual) or not math.isfinite(expected) or abs(actual - expected) > .001):
            raise SystemExit(f'{variant} {kind}: miniSTA={actual}, OpenSTA={expected}')
    print(f'PASS edge checks vs OpenSTA: {variant}')
