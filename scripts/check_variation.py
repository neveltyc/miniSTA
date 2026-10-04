#!/usr/bin/env python3
"""AOCV/CPPR hand calculations, CPPR reference comparison, and atomic import tests."""
from pathlib import Path
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
AOCV = {
    'priority': (5.35, -1.925), 'data_check': (-4.225, 0), 'output_ref': (5.85, -2.275), 'input_ref': (7.3, -3.4), 'reload': (4.8, -3.1), 'clamp': (-3.025, 1.0375), 'short': (6.25, -1.875), 'long': (.125, .0875), 'mixed': (-.9375, -.91875),
    'data': (7.15, -.975), 'clock': (6.6, -1.75), 'scalar': (4.8, -3.1),
    'cell': (5.95, -1.925), 'instance': (5.35, -1.975),
    'disabled': (7.5, -.85), 'cppr': (6.65, -1.475), 'multiply': (3.5, -4.12),
}
CPPR = {
    'input_included': (7.4, -5.3), 'tied_clock': (2.4, -5.5), 'shared_contexts': (-147.6, 1.7), 'async_budget': (3.8, 0), 'async': (5.6, -2.3), 'gate': (5.6, -2.3), 'aliases': (5.6, -2.3), 'on': (5.6, -2.3), 'off': (4.8, -3.1), 'bc_wc': (7.2, -.7),
    'no_common': (3.2, -.7), 'same_pin': (-.6, 3.5), 'same_transition': (-1, 3.1),
    'generated': (5.6, -2.3), 'source': (5.6, -2.3), 'io_reference': (5.6, -2.3),
    'path_budget': (3.8, 0), 'ranking': (4.4, -1.5), 'reconvergent': (2, -5.9),
}


def slacks(text, reference=False):
    patterns = [rf'worst slack {corner}\s+([-\d.]+)' for corner in ('max', 'min')] if reference else [
        rf'^{kind}\s*:\s*WNS\s+([-\d.]+)' for kind in ('setup', 'hold')]
    return tuple(float(re.findall(p, text, re.M)[-1]) for p in patterns)


def check(name, actual, expected):
    if not all(math.isclose(a, b, abs_tol=.001, rel_tol=0) for a, b in zip(actual, expected)):
        raise AssertionError(f'{name}: got {actual}, expected {expected}')


def run(command, env=None):
    result = subprocess.run(command, text=True, check=True, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    if re.search(r'^Error|约束作废|命令未建模或写错', result, re.M):
        raise AssertionError(result)
    return result


def parser_checks(*, compiler_flags=(), object_files=None):
    # Test public importer directly so failure can be followed by checking the old state.
    source = r'''
#include "msta_sdc.h"
#include <assert.h>
#include <math.h>
int main(int argc, char **argv) {
    MstaSdc S={0};
    assert(Msta_AocvRead(&S,argv[1],0));
    MstaCell C={0}; MstaInst I={0};
    C.Name=Msta_NameId("DFF"); I.Name=Msta_NameId("test"); I.pCell=&C;
    assert(fabs(Msta_AocvFactor(&S,&I,0,1,0,1)-.8)<1e-12);
    assert(fabs(Msta_AocvFactor(&S,&I,0,1,0,3)-.925)<1e-12);
    assert(fabs(Msta_AocvFactor(&S,&I,0,1,0,99)-.95)<1e-12);
    assert(fabs(Msta_AocvFactor(&S,&I,0,1,1,3)-1.075)<1e-12);
    S.fAocv=0;
    int N=S.vAocv.nSize;
    MstaAocvTable *Old=S.vAocv.pData;
    double Factor=Old[0].Factor[0];
    assert(!Msta_AocvRead(&S,argv[2],0));
    assert(S.vAocv.nSize==N && S.vAocv.pData==Old && S.fAocv==0 && Old[0].Factor[0]==Factor);
    MstaAocvTableArrayFree(&S.vAocv);
    return 0;
}
'''
    invalid = {
        'depth_fraction': 'design * data rise late 1.5 1.2\n',
        'depth_overflow': 'design * data rise late 999999999999999999999 1.2\n',
        'depth_zero': 'design * data rise late 0 1.2\n',
        'nan': 'design * data rise late 1 nan\n',
        'infinity': 'design * data rise late 1 inf\n',
        'negative': 'design * data rise late 1 -1\n',
        'early_gt_one': 'design * data rise early 1 1.2\n',
        'late_lt_one': 'design * data rise late 1 .8\n',
        'nonmonotonic': 'design * data rise late 1 1.1\ndesign * data rise late 2 1.2\n',
        'duplicate': 'design * data rise late 1 1.1\ndesign * data rise late 1 1.1\n',
        'trailing': 'design * data rise late 1 1.2 distance 30\n',
        'empty': '# empty\n',
        'long_line': 'x' * 3000 + '\n',
        'nul_trailing': 'design * data rise late 1 1.2\x00 hidden column\n',
        'nul_middle': 'design * data rise late 1\x00 1.2\n',
        'nul_comment': '# comment\x00 hidden\n',
        'too_many_points': ''.join(f'design * data rise late {d} 1.2\n' for d in range(1, 258)),
    }
    with tempfile.TemporaryDirectory(prefix='msta-aocv-', dir=ROOT / 'build') as directory:
        temp = Path(directory)
        (temp / 'test.c').write_text(source)
        objects = object_files or [ROOT / 'build/msta_aocv.o', ROOT / 'build/msta_util.o']
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *compiler_flags,
                        '-std=gnu99', '-I' + str(ROOT / 'src'), str(temp / 'test.c'),
                        *(str(obj) for obj in objects), '-lm', '-o', str(temp / 'test')], check=True)
        for name, text in invalid.items():
            (temp / 'bad.aocv').write_text(text)
            subprocess.run([str(temp / 'test'), str(ROOT / 'testcases/aocv/base.aocv'),
                            str(temp / 'bad.aocv')], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print(f'==> AOCV importer: {len(invalid)} invalid-file/atomic-state checks passed')


if __name__ == '__main__':
    if len(sys.argv) == 1:
        for directory, expected in (('aocv', AOCV), ('cppr', CPPR)):
            for name, values in expected.items():
                text = (ROOT / f'build/testcases_{directory}_{name}.dofile.log').read_text()
                check(directory + '/' + name, slacks(text), values)
        for name, credit in (('on', .8), ('no_common', 0), ('source', 1.2), ('reconvergent', .8), ('tied_clock', 0)):
            text = (ROOT / f'build/testcases_cppr_{name}.dofile.log').read_text()
            values = [abs(float(v)) for v in re.findall(r'clock reconvergence pessimism\s+([-\d.]+)', text)]
            if not values or any(not math.isclose(v, credit, abs_tol=.001) for v in values):
                raise AssertionError((name, values, credit))
        ranking = (ROOT / 'build/testcases_cppr_ranking.dofile.log').read_text()
        assert 'startpoint : u_launch' in ranking.split('===== hold path')[0]
        contexts = (ROOT / 'build/testcases_cppr_shared_contexts.dofile.log').read_text()
        assert 'launch contexts 1' in contexts
        # set_data_check turns two intermediate pins into actual AOCV endpoints.
        depths = (ROOT / 'build/testcases_aocv_data_check.dofile.log').read_text().split('===== hold path')[0]
        assert [int(v) for v in re.findall(r'AOCV structural depth: (\d+)', depths)] == [2, 2, 3, 4]
        parser_checks()
        print(f'==> AOCV/CPPR hand checks passed ({len(AOCV) + len(CPPR)} cases)')
    elif len(sys.argv) == 4:
        msta, sta, output = sys.argv[1:]
        out = Path(output) / 'cppr'
        out.mkdir(parents=True, exist_ok=True)
        for name, expected in CPPR.items():
            # Existing miniSTA path budgets are launch-relative; not OpenSTA's skew/check semantics.
            # Reconvergent clock GBA intentionally credits only identical physical root prefixes.
            if name in ('path_budget', 'async_budget', 'reconvergent', 'tied_clock'):
                continue
            dofile = ROOT / f'testcases/cppr/{name}.dofile'
            netlist = re.search(r'read_verilog (\w+)\.v', dofile.read_text())[1]
            mini = run([msta, '-q', str(dofile)])
            reference = run([sta, '-exit', str(ROOT / 'testcases/cppr/opensta.tcl')],
                            dict(os.environ, CPPR_CASE=name, CPPR_NETLIST=netlist))
            (out / f'{name}_msta.log').write_text(mini)
            (out / f'{name}_opensta.log').write_text(reference)
            check(name, slacks(mini), expected)
            check(name + ' OpenSTA', slacks(mini), slacks(reference, True))
            print(f'通过 CPPR 与 OpenSTA 对比：{name}')
    else:
        raise SystemExit('usage: check_variation.py [msta OpenSTA output]')
