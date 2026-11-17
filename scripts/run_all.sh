#!/usr/bin/env bash
#
# make test：把 testcases/ 下的每个 dofile 各起一个进程跑一遍，日志写到 build/；
# 任何命令报错、解析失败或非零退出都让脚本失败。
# 之后依次运行读这些日志的断言脚本：
#   check_sdc.sh            SDC 语义
#   check_edges.sh          按数据边沿的约束检查（testcases/check_edges）
#   check_clock_edges.py    时钟 min/max × rise/fall（testcases/clock_edges）
#   check_nonunate_clock.py non_unate 时钟（testcases/nonunate_clock）
#   check_ocv.py            OCV 静态回归（testcases/ocv）
#   check_variation.py      AOCV/CPPR 回归（testcases/aocv）
#   check_phase1_fixes.py   Phase 1 修复项（网表/Liberty/SDC/引擎 11 组回归）
#   check_phase2_teaching.py Phase 2 模块化与教学命令（pin/flags/explain_arc/report_net/dump_graph）
#   check_phase3_plugins.py Phase 3 C99 插件接口、Elmore RC/PBA 示范插件与分级增量 swap_cell
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSTA="${ROOT}/build/msta"

if [[ ! -x "${MSTA}" ]]; then
    echo "错误：找不到 ${MSTA}，请先运行 make" >&2
    exit 1
fi

if [[ -z "$(find "${ROOT}/testcases" -name '*.dofile' -type f -print -quit)" ]]; then
    echo "错误：testcases/ 下没有找到 *.dofile" >&2
    exit 1
fi

count=0
while IFS= read -r dofile; do
    rel="${dofile#"${ROOT}/"}"
    # 日志名里保留目录，避免同名 dofile 互相覆盖。
    log="${ROOT}/build/$(printf '%s' "${rel}" | tr '/' '_').log"
    echo "==> ${rel}"
    "${MSTA}" -q "${dofile}" >"${log}" 2>&1
    tail -n 2 "${log}"
    count=$((count + 1))
done < <(find "${ROOT}/testcases" -name '*.dofile' -type f | sort)

echo "==> ${count} 个用例全部通过"
bash "${ROOT}/scripts/check_sdc.sh"
bash "${ROOT}/scripts/check_edges.sh"
python3 "${ROOT}/scripts/check_clock_edges.py"
python3 "${ROOT}/scripts/check_nonunate_clock.py"
python3 "${ROOT}/scripts/check_ocv.py"
python3 "${ROOT}/scripts/check_variation.py"
python3 "${ROOT}/scripts/check_phase1_fixes.py" "${MSTA}"
python3 "${ROOT}/scripts/check_phase2_teaching.py" "${MSTA}"
python3 "${ROOT}/scripts/check_phase3_plugins.py" "${MSTA}"
