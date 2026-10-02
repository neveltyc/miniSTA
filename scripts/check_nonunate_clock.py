#!/usr/bin/env python3
"""non_unate 时钟回归：源边沿标签、汇聚、selector case 分析与源边沿例外。

不带参数时检查 build/ 下的日志；带 <msta> <sta> <输出目录> 参数时逐个运行并对比 setup/hold WNS。
"""
from pathlib import Path
import math
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
case = root/'testcases/nonunate_clock'
expected = {
    'xor_capture': (1, -1.6), 'xor_violation': (-1, -1.6), 'xor_launch': (.9, 3.1),
    'mux_capture': (3.1, -3.6), 'mux_ideal': (-.9, .5),
    'xor_sel_zero': (6, -1.6), 'xor_sel_one': (1, 3.4),
    'mux_sel_zero': (8, -3.6), 'mux_sel_one': (3.1, 1.3),
    'capture_cut_fall': (6, -1.6), 'capture_cut_rise': (1, 3.4),
    'capture_pin_cut_fall': (3.4, -1.6), 'capture_uncertainty': (.6, -1.6),
    'collection_rise': (1, -1.6), 'collection_fall': (1, -1.6), 'collection_sel_zero': (6, -1.6),
    'xor_async': (1, -1.6), 'xor_gate': (1, -1.6), 'xor_generated': (6, -1.6),
    'xor_slew': (.6, -1.6), 'bool_precedence': (-1, -1.6),
}

def slacks(text):
    return tuple(float(re.search(rf'^{kind}\s*: WNS\s+([-\d.]+)',text,re.M)[1])
                 for kind in ('setup','hold'))

def check(name, text):
    assert all(abs(a-b) < 1e-6 for a,b in zip(slacks(text),expected[name])), name
    if name == 'xor_capture':
        setup = text.split('===== setup path')[1].split('---------------- 时序汇总')[0]
        assert 'capture source edge: fall' in setup
        assert 'capture clock: core @ 7.000 ns' in setup
        assert 'data required time                           3.000' in setup
        assert 'data arrival time                            2.000' in setup
        hold = text.split('===== hold path')[1]
        assert 'capture source edge: rise' in hold
    if name == 'xor_launch':
        setup = text.split('===== setup path')[1].split('---------------- 时序汇总')[0]
        assert 'launch source edge : fall' in setup
        assert 'launch clock : core @ 7.000 ns' in setup
    if name == 'xor_slew':
        setup = text.split('===== setup path')[1].split('---------------- 时序汇总')[0]
        assert 'capture source edge: fall' in setup
        assert '  - setup check (from lib)                        -4.400' in setup
    if name == 'capture_uncertainty':
        assert '  - clock uncertainty                       -0.400' in text
    if name == 'xor_gate':
        assert '时钟门控 : 1 条检查' in text
    if name == 'xor_violation':
        assert 'setup : WNS   -1.000 ns   TNS    -1.000 ns   违例端点 1 / 2' in text

if len(sys.argv) == 1:
    for name in expected:
        check(name,(root/f'build/testcases_nonunate_clock_{name}.dofile.log').read_text())
    print(f'==> non-unate 时钟检查全部通过（{len(expected)} 个用例）')
else:
    msta, sta, output = sys.argv[1:]
    out = Path(output)/'nonunate_clock'
    out.mkdir(parents=True,exist_ok=True)
    for name in expected:
        source = (case/f'{name}.dofile').read_text()
        lib = re.search(r'read_liberty (\w+)\.lib',source)[1]
        design = re.search(r'current_design (\w+)',source)[1]
        mini = subprocess.run([msta,'-q',str(case/f'{name}.dofile')],text=True,check=True,
                              stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
        (out/f'{name}_msta.log').write_text(mini)
        check(name,mini)
        if name == 'xor_slew':
            # msta 为每个时钟源标签单独保留 slew；按引脚合并 rise/fall min/max slew 的模型
            # 得到的约束值不同，所以这个用例只做手算检查，不做 WNS 对比。
            print('通过 按源边沿标签区分 slew 的手算检查：xor_slew（不参与 WNS 对比）')
            continue
        env = dict(os.environ,NONUNATE_DESIGN=design,NONUNATE_CASE=name,NONUNATE_LIB=lib)
        reference = subprocess.run([sta,'-exit',str(case/'opensta.tcl')],env=env,text=True,check=True,
                                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
        (out/f'{name}_opensta.log').write_text(reference)
        reference_slack = tuple(float(re.search(rf'worst slack {corner}\s+([-\d.]+)',reference)[1])
                                for corner in ('max','min'))
        if any(abs(a-b) > .001 for a,b in zip(slacks(mini),reference_slack)):
            raise SystemExit(f'{name}：miniSTA={slacks(mini)}，OpenSTA={reference_slack}')
        print(f'通过 non-unate 时钟与 OpenSTA 对比：{name}')
