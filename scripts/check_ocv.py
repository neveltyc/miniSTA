#!/usr/bin/env python3
"""OCV 手算回归；传入 msta、OpenSTA 和输出目录时进行参考工具对照。"""
from pathlib import Path
import math
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
CASE = ROOT / 'testcases/ocv'
# 时间单位 ns；推导见 testcases/ocv/README.md。
EXPECTED = {
    'baseline': (7.5, -.85), 'global': (4.8, -3.1),
    'split': (5.15, -2.975), 'split_reverse': (5.15, -2.975),
    'obj': (3.9, -3.2), 'obj_clock': (4.7, -2.85),
    'edge': (6.1, -1), 'check': (6.3, -1.07),
    'both_types': (4.6, -3.08), 'ocv_mode': (4.8, -3.1),
    'bc_wc': (7.2, -.7), 'single': (7.2, -.7), 'switch': (4.8, -3.1),
    'overwrite': (6.8, -1.1), 'obj_fallback': (4.2, -3.1),
    'unset': (7.5, -.85), 'obj_data_only': (4.8, -3.1),
    'clock_edge': (5.5, -2.85), 'check_obj': (4.3, -3.07),
    'invert': (8.65, -.6), 'nonunate': (6.875, -1.1),
    'ideal': (2.8, -1.1), 'generated': (4.8, -3.1),
    'io_reference': (4.8, -3.1), 'clock_object': (5.5, -2.85),
    'corners_ocv': (4.5, -3.425), 'corners_bc_wc': (7.5, -.425),
}


def slacks(text, reference=False):
    patterns = (r'worst slack max\s+([-\d.]+)', r'worst slack min\s+([-\d.]+)') if reference else (
        r'^setup\s*:\s*WNS\s+([-\d.]+)', r'^hold\s*:\s*WNS\s+([-\d.]+)')
    return tuple(float(re.search(pattern, text, re.M)[1]) for pattern in patterns)


def check(name, actual, expected, tolerance=.001):
    if not all(math.isclose(a, b, abs_tol=tolerance, rel_tol=0)
               for a, b in zip(actual, expected)):
        raise AssertionError(f'{name}: 实际 {actual}, 预期 {expected}')


def run(command, env=None):
    text = subprocess.run(command, text=True, check=True, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    if re.search(r'^Error|约束作废|命令未建模或写错', text, re.M):
        raise AssertionError(text)
    return text


if len(sys.argv) == 1:
    for name, expected in EXPECTED.items():
        text = (ROOT / f'build/testcases_ocv_{name}.dofile.log').read_text()
        check(name, slacks(text), expected, 1e-6)
    text = (ROOT / 'build/testcases_ocv_global.dofile.log').read_text()
    setup, hold = text.split('===== setup path')[1].split('===== hold path')
    for segment, values in ((setup, (9, 13.8)), (hold, (4.2, 7.3))):
        for label, value in zip(('data arrival time', 'data required time'), values):
            check(label, (float(re.search(label + r'\s+([-\d.]+)', segment)[1]),), (value,), 1e-6)
    print(f'==> OCV 手算检查全部通过（{len(EXPECTED)} 个用例）')
elif len(sys.argv) == 4:
    msta, sta, output = sys.argv[1:]
    out = Path(output) / 'ocv'
    out.mkdir(parents=True, exist_ok=True)
    for name, expected in EXPECTED.items():
        # 时钟对象 derate 是 miniSTA 的已有扩展；OpenSTA 仅接受实例/库单元/网络。
        if name == 'clock_object':
            continue
        netlist = 'invert' if name == 'invert' else 'nonunate' if name == 'nonunate' else 'ocv'
        mini = run([msta, '-q', str(CASE / f'{name}.dofile')])
        reference = run([sta, '-exit', str(CASE / 'opensta.tcl')],
                        dict(os.environ, OCV_CASE=name, OCV_NETLIST=netlist))
        (out / f'{name}_msta.log').write_text(mini)
        (out / f'{name}_opensta.log').write_text(reference)
        check(name, slacks(mini), expected)
        check(name + ' OpenSTA', slacks(mini), slacks(reference, True))
        print(f'通过 OCV 与 OpenSTA 对比：{name}')
else:
    raise SystemExit('用法: check_ocv.py [msta OpenSTA 输出目录]')
