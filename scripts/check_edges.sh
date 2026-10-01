#!/usr/bin/env bash
# 按数据边沿的约束检查手算回归；日志由 run_all.sh 生成。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "${ROOT}/build" <<'PY'
from pathlib import Path
import re
import sys
expect = {
    # setup/hold: (slack, edge, arrival, constraint)
    'normal': ((4.1, 'fall', 2, 3.9), (.5, 'rise', 3, 2.5)),
    'swapped': ((4.1, 'fall', 2, 3.9), (.5, 'rise', 3, 2.5)),
    'rise_only': ((6.5, 'rise', 3, .5), (.5, 'rise', 3, 2.5)),
    'data_1d': ((4.5, 'fall', 2, 3.5), (.7, 'rise', 3, 2.3)),
    'negative': ((7.2, 'rise', 3, -.2), (2.4, 'fall', 2, -.4)),
    'cut_rise': ((4.1, 'fall', 2, 3.9), (1.1, 'fall', 2, .9)),
    'cut_fall': ((6.5, 'rise', 3, .5), (.5, 'rise', 3, 2.5)),
    'to_rise': ((4.1, 'fall', 2, 3.9), (1.1, 'fall', 2, .9)),
    'to_fall': ((6.5, 'rise', 3, .5), (.5, 'rise', 3, 2.5)),
    'from_rise': ((4.1, 'fall', 2, 3.9), (1.1, 'fall', 2, .9)),
    'from_fall': ((6.5, 'rise', 3, .5), (.5, 'rise', 3, 2.5)),
    'clock_to_fall': ((4.1, 'fall', 2, 3.9), (.5, 'rise', 3, 2.5)),
}
for name, corners in expect.items():
    text = (Path(sys.argv[1])/f'testcases_check_edges_{name}.dofile.log').read_text()
    for kind, (slack, edge, arrival, check) in zip(('setup', 'hold'), corners):
        path = text.split(f'===== {kind} path')[1].split('---------------- 时序汇总')[0]
        def number(pattern):
            return float(re.search(pattern, path).group(1))
        assert f'data edge    : {edge}' in path, (name, kind, path)
        assert abs(number(r'slack \(MET\)\s+([-\d.]+)')-slack) < 1e-6, (name, kind)
        assert abs(number(r'data arrival time\s+([-\d.]+)')-arrival) < 1e-6, (name, kind)
        pattern = r'- setup check \(from lib\)\s+([-\d.]+)' if kind == 'setup' else r'\+ hold check\s+([-\d.]+)'
        assert abs(number(pattern)-(-check if kind == 'setup' else check)) < 1e-6, (name, kind)
        # 路径快照必须也是获胜边沿上的 arrival，不能沿合并前驱报告。
        assert abs(number(r'u_buf/Y\s+[-\d.]+\s+([-\d.]+)')-arrival) < 1e-6, (name, kind)
base = Path(sys.argv[1])
clock_rise = (base/'testcases_check_edges_clock_to_rise.dofile.log').read_text()
assert 'setup : 没有一条可分析的路径' in clock_rise
assert 'hold  : 没有一条可分析的路径' in clock_rise
async_text = (base/'testcases_check_edges_async_phase.dofile.log').read_text()
recovery = async_text.split('===== recovery path')[1].split('===== setup path')[0]
removal = async_text.split('===== removal path')[1].split('---------------- 时序汇总')[0]
for path, start, edge, launch, capture, arrival, slack in (
    (recovery, 'a', 'fall', 0, 5, 2, 3),
    (removal, 'b', 'rise', 5, 5, 6, -1),
):
    assert f'startpoint : {start}' in path
    assert f'data edge    : {edge}' in path
    for pattern, expected in (
        (r'launch clock : core @ ([-\d.]+)', launch),
        (r'capture clock: core @ ([-\d.]+)', capture),
        (r'data arrival time\s+([-\d.]+)', arrival),
        (r'slack \(\w+\)\s+([-\d.]+)', slack),
    ):
        assert abs(float(re.search(pattern,path).group(1))-expected) < 1e-6
assert 'setup : WNS    3.000 ns' in async_text
assert 'hold  : WNS   -1.000 ns' in async_text
gate = (base/'testcases_check_edges_gating_phase.dofile.log').read_text()
assert '时钟门控 : 1 条检查   setup 最差 3.000 ns' in gate
assert 'hold 最差 -1.000 ns' in gate
for name, setup, hold in (
    ('output_clock_to_rise', 2.9, 5.1),
    ('output_clock_to_fall', 4.1, .5),
):
    text = (base/f'testcases_check_edges_{name}.dofile.log').read_text()
    for kind, expected in (('setup',setup),('hold',hold)):
        actual = float(re.search(rf'^{kind}\s*: WNS\s+([-\d.]+)',text,re.M).group(1))
        assert abs(actual-expected) < 1e-6, (name,kind)
    if name.endswith('rise'):
        assert 'to register' not in text
        assert 'endpoint   : q' in text
    else:
        assert 'to output port' not in text
print('==> edge constraint checks passed (17 cases)')
PY
