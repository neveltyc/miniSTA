#!/usr/bin/env bash
#
# 用 scripts/synth_sky130.sh 从 testcases/synth_sky130/rtl/pipe_demo.v 综合出的
# sky130 网表和 OpenSTA 对比。
#
# 不跟 OpenTimer 比：它的 Verilog 前端读不了总线位选，sky130 库也只认得一部分
# 单元，会报 no critical path。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CASE="${ROOT}/testcases/synth_sky130"
OUT="${COMPARE_OUT:-${ROOT}/build/compare_synth_sky130}"
MSTA_BIN="${MSTA_BIN:-${ROOT}/build/msta}"
TOLERANCE_NS="${TOLERANCE_NS:-0.05}"

if [[ -z "${OPENSTA_BIN:-}" ]]; then
    if [[ -x "${ROOT}/vendor/opensta-build/sta" ]]; then
        OPENSTA_BIN="${ROOT}/vendor/opensta-build/sta"
    else
        OPENSTA_BIN="${ROOT}/vendor/build/opensta/sta"
    fi
fi

if [[ ! -x "${MSTA_BIN}" ]]; then
    echo "错误：找不到 msta（${MSTA_BIN}），请先运行 make" >&2
    exit 1
fi
if [[ ! -x "${OPENSTA_BIN}" ]]; then
    echo "错误：找不到 OpenSTA（${OPENSTA_BIN}），请设置 OPENSTA_BIN" >&2
    exit 1
fi

mkdir -p "${OUT}"

echo "==> msta"
"${MSTA_BIN}" -q "${CASE}/pipe_demo.dofile" > "${OUT}/msta.log" 2>&1
MSTA_SETUP_WNS="$(awk '$1=="setup" && $2==":" {print $4}' "${OUT}/msta.log" | tail -1)"
MSTA_HOLD_WNS="$(awk '$1=="hold" && $2==":" {print $4}' "${OUT}/msta.log" | tail -1)"

echo "==> OpenSTA"
( cd "${CASE}" && "${OPENSTA_BIN}" -no_splash -no_init opensta.tcl ) > "${OUT}/opensta.log" 2>&1
OPENSTA_SETUP_WNS="$(awk '$1=="worst" && $2=="slack" && $3=="max" {print $4}' "${OUT}/opensta.log")"
OPENSTA_HOLD_WNS="$(awk '$1=="worst" && $2=="slack" && $3=="min" {print $4}' "${OUT}/opensta.log")"

# 一个汉字占 3 字节、显示 2 列，表头第一列宽度多给 2 才能和下面的行对齐。
printf '\n%-14s %14s %14s\n' "工具" "setup WNS" "hold WNS"
printf '%-12s %14s %14s\n' msta "${MSTA_SETUP_WNS:-n/a}" "${MSTA_HOLD_WNS:-n/a}"
printf '%-12s %14s %14s\n' OpenSTA "${OPENSTA_SETUP_WNS:-n/a}" "${OPENSTA_HOLD_WNS:-n/a}"
printf '\n日志：%s\n' "${OUT}"

compare_value() {
    awk -v label="$1" -v a="$2" -v b="$3" -v tolerance="$4" 'BEGIN {
        delta = a - b;
        if (delta < 0) delta = -delta;
        if (delta > tolerance) {
            printf "失败 %s：msta=%s 参考=%s 差值=%.6f ns（容差 %s ns）\n",
                   label, a, b, delta, tolerance;
            exit 1;
        }
        printf "通过 %s：msta=%s 参考=%s 差值=%.6f ns\n", label, a, b, delta;
    }'
}

failures=0
compare_value "setup WNS 与 OpenSTA 对比" "${MSTA_SETUP_WNS}" "${OPENSTA_SETUP_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
compare_value "hold WNS 与 OpenSTA 对比" "${MSTA_HOLD_WNS}" "${OPENSTA_HOLD_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))

if (( failures > 0 )); then
    echo "==> 对比失败：${failures} 项超出容差" >&2
    exit 1
fi
echo "==> 对比通过"
