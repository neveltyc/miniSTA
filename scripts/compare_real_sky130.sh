#!/usr/bin/env bash
#
# 拿 testcases/ 里的真实 sky130 网表跟 OpenSTA 对比。
#
# eth_sky130  : 2.5 万实例、三个时钟域，对 setup WNS 做容差断言（msta 不做 CPPR，
#               结果偏悲观）。
# e902_sky130 : 6.7 千端点、关键路径约 170 级，逐级的建模差异会累加，只报告不断言。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${COMPARE_OUT:-${ROOT}/build/compare_real_sky130}"
MSTA_BIN="${MSTA_BIN:-${ROOT}/build/msta}"
TOLERANCE_NS="${TOLERANCE_NS:-0.25}"

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
# 一个汉字占 3 字节、显示 2 列，含汉字的列宽按字节数放宽，保证表头和各行对齐。
printf '%-18s %16s %16s %16s %12s\n' "用例" "msta setup" "OpenSTA setup" "差值" "状态"

failures=0
run_case() {
    local name="$1" dir="$2" dofile="$3"
    "${MSTA_BIN}" -q "${ROOT}/testcases/${dir}/${dofile}" > "${OUT}/${name}.msta.log" 2>&1
    ( cd "${ROOT}/testcases/${dir}" && "${OPENSTA_BIN}" -no_splash -no_init opensta.tcl ) \
        > "${OUT}/${name}.opensta.log" 2>&1
    local msta_setup opensta_setup msta_hold opensta_hold
    msta_setup="$(awk '$1=="setup" && $2==":" {print $4}' "${OUT}/${name}.msta.log" | tail -1)"
    msta_hold="$(awk '$1=="hold" && $2==":" {print $4}' "${OUT}/${name}.msta.log" | tail -1)"
    opensta_setup="$(awk '$1=="worst" && $2=="slack" && $3=="max" {print $4}' "${OUT}/${name}.opensta.log")"
    opensta_hold="$(awk '$1=="worst" && $2=="slack" && $3=="min" {print $4}' "${OUT}/${name}.opensta.log")"

    local delta status
    delta="$(awk -v a="${msta_setup}" -v b="${opensta_setup}" 'BEGIN{d=a-b; if(d<0)d=-d; printf "%.6f", d}')"
    status="$(awk -v d="${delta}" -v t="${TOLERANCE_NS}" 'BEGIN{print (d<=t) ? "在容差内" : "超出容差"}')"
    printf '%-16s %16s %16s %14s %14s\n' "${name}" "${msta_setup}" "${opensta_setup}" "${delta}" "${status}"
    echo "    hold WNS：msta ${msta_hold}  OpenSTA ${opensta_hold}" >> "${OUT}/summary.txt"
    echo "    setup WNS：msta ${msta_setup}  OpenSTA ${opensta_setup}  差值 ${delta}" >> "${OUT}/summary.txt"

    if [[ "${name}" == "eth_sky130" && "${status}" != "在容差内" ]]; then
        failures=$((failures + 1))
    fi
}

: > "${OUT}/summary.txt"
run_case eth_sky130  eth_sky130   eth.dofile
run_case e902_sky130 e902_sky130  e902.dofile

printf '\n日志：%s\n' "${OUT}"
echo "说明：e902_sky130 只报告不断言。它的关键路径约 170 级，两个工具逐级的"
echo "      建模差异会沿路径累加。"

if (( failures > 0 )); then
    echo "==> 对比失败：eth_sky130 的 setup WNS 差值超过 ${TOLERANCE_NS} ns" >&2
    exit 1
fi
echo "==> 对比通过"
