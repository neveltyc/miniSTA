#!/usr/bin/env python3
"""Hand-calculated clock edge regressions; optionally compare supported models with OpenSTA."""
from pathlib import Path
import math
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
case = root/'testcases/clock_edges'
expected = {
    'capture_rise': (6, -1.6), 'capture_fall': (-.2, 4.2),
    'capture_invert': (1, 3.4), 'capture_nonmonotonic': (6.4, -1.2),
    'launch_rise': (5.9, 3.1), 'launch_fall': (1.9, 7.1), 'launch_invert': (.9, 8.1),
    'source_slew': (5, -2.6), 'source_slew_invert': (3, -.6),
    'source_latency': (5.6, -2), 'ideal_source_slew': (4, .4),
    'ideal_corner_slew': (3.8, .4),
    'generated_buf': (6, -1.6), 'generated_buf_invert': (1, 3.4),
    'generated_qn': (6.6, -2.4), 'source_conflict': (5, -2.6),
    'ideal_zero_slew': (4.2, .6),
    'generated_ideal': (6.1, -1.5), 'generated_ideal_net': (7.1, -2.5),
    'generated_ideal_qn': (6.8, -2.2), 'generated_disable': (4.2, .6),
    'zero_output': (6.2, -1.4), 'launch_disable': (math.inf, math.inf),
    'ideal_edges': (4.3, -.4), 'reference_pin': (2.2, 2.6),
    'propagated_zero_slew': (3, -.6),
    'ideal_no_clock_slew': (3.2, -.4),
}

def close(actual, expected, tolerance):
    return actual == expected or (math.isfinite(actual) and math.isfinite(expected)
                                 and abs(actual-expected) <= tolerance)

def slacks(text):
    values = []
    for kind in ('setup','hold'):
        match = re.search(rf'^{kind}\s*: WNS\s+([-\d.]+)',text,re.M)
        if match:
            values.append(float(match[1]))
        elif re.search(rf'^{kind}\s*:\s*没有一条可分析的路径',text,re.M):
            values.append(math.inf)
        else:
            raise ValueError(f'missing {kind} timing result')
    return tuple(values)

if len(sys.argv) == 1:
    for name, values in expected.items():
        text = (root/f'build/testcases_clock_edges_{name}.dofile.log').read_text()
        assert all(close(a,b,1e-6) for a,b in zip(slacks(text),values)), name
    # Check capture arrival, table result, required and launch insertion directly.
    text = (root/'build/testcases_clock_edges_capture_rise.dofile.log').read_text()
    setup = text.split('===== setup path')[1].split('---------------- 时序汇总')[0]
    for pattern, value in ((r'capture clock: core @ ([-\d.]+)',12),
                           (r'- setup check \(from lib\)\s+([-\d.]+)',-4),
                           (r'data required time\s+([-\d.]+)',8),
                           (r'data arrival time\s+([-\d.]+)',2)):
        assert abs(float(re.search(pattern,setup)[1])-value) < 1e-6
    launch = (root/'build/testcases_clock_edges_launch_rise.dofile.log').read_text()
    assert 'launch clock : core @ 2.000 ns' in launch
    assert 'data arrival time                            4.100' in launch
    generated = (root/'build/testcases_clock_edges_generated_buf_invert.dofile.log').read_text()
    assert 'capture clock: derived @ 7.000 ns' in generated
    assert 'capture clock: derived @ -3.000 ns' in generated
    assert 'capture clock: core' not in generated
    qn = (root/'build/testcases_clock_edges_generated_qn.dofile.log').read_text()
    assert 'cannot derive sequential source latency' not in qn
    assert 'capture clock: derived @ 22.700 ns' in qn
    assert 'capture clock: derived @ 2.700 ns' in qn
    ideal = (root/'build/testcases_clock_edges_generated_ideal.dofile.log').read_text()
    assert 'capture clock: derived @ 12.500 ns' in ideal
    assert 'capture clock: derived @ 2.500 ns' in ideal
    disabled = (root/'build/testcases_clock_edges_generated_disable.dofile.log').read_text()
    assert 'cannot derive sequential source latency' in disabled
    assert 'capture clock: derived @ 20.000 ns' in disabled
    assert 'capture clock: derived @ 0.000 ns' in disabled
    zero = (root/'build/testcases_clock_edges_zero_output.dofile.log').read_text()
    assert '  - setup check (from lib)                        -3.800' in zero
    print(f'==> clock edge checks passed ({len(expected)} cases)')
else:
    msta, sta, output = sys.argv[1:]
    out = Path(output)/'clock_edges'
    out.mkdir(parents=True,exist_ok=True)
    for name, values in expected.items():
        # miniSTA explicitly models ideal annotations and input reference pins differently.
        # These two cases use hand assertions above, not reference-tool equivalence.
        if name in ('ideal_edges','reference_pin'): continue
        source = (case/f'{name}.dofile').read_text()
        lib = re.search(r'read_liberty (\w+)\.lib',source)[1]
        design = re.search(r'current_design (\w+)',source)[1]
        netlist = re.search(r'read_verilog (\w+)\.v',source)[1]
        env = dict(os.environ,CLOCK_EDGE_LIB=lib,CLOCK_EDGE_DESIGN=design,
                   CLOCK_EDGE_NETLIST=netlist,CLOCK_EDGE_CASE=name)
        mini = subprocess.run([msta,'-q',str(case/f'{name}.dofile')],text=True,check=True,
                              stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
        reference = subprocess.run([sta,'-exit',str(case/'opensta.tcl')],env=env,text=True,check=True,
                                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
        (out/f'{name}_msta.log').write_text(mini)
        (out/f'{name}_opensta.log').write_text(reference)
        actual = slacks(mini)
        reference_slack = tuple(float(re.search(rf'worst slack {corner}\s+([-\d.]+|INF)',reference,re.I)[1])
                                for corner in ('max','min'))
        for got, hand, ref in zip(actual,values,reference_slack):
            if not close(got,hand,1e-6) or not close(got,ref,.001):
                raise SystemExit(f'{name}: miniSTA={actual}, hand={values}, OpenSTA={reference_slack}')
        print(f'PASS clock edges vs OpenSTA: {name}')
