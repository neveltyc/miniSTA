#!/usr/bin/env bash
#
# 拿 testcases/ 里的真实 sky130 网表跟 OpenSTA 对比。
#
# eth_sky130  : 2.5 万实例、三个时钟域。差值在容差内，做断言（msta 不做 CPPR，
#               偏悲观是预期的）。
# e902_sky130 : 6.7 千端点，5 ns 时钟下深度违例，关键路径 ~170 级，逐级建模差
#               累加到几 ns，所以只报告不断言。
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
    echo "error: msta not found at ${MSTA_BIN}; run make first" >&2
    exit 1
fi
if [[ ! -x "${OPENSTA_BIN}" ]]; then
    echo "error: OpenSTA not found at ${OPENSTA_BIN}; set OPENSTA_BIN" >&2
    exit 1
fi

mkdir -p "${OUT}"
printf '%-16s %16s %16s %14s %10s\n' "case" "msta setup" "OpenSTA setup" "delta" "status"

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
    status="$(awk -v d="${delta}" -v t="${TOLERANCE_NS}" 'BEGIN{print (d<=t) ? "ok" : "differs"}')"
    printf '%-16s %16s %16s %14s %10s\n' "${name}" "${msta_setup}" "${opensta_setup}" "${delta}" "${status}"
    echo "    hold WNS: msta ${msta_hold}  OpenSTA ${opensta_hold}" >> "${OUT}/summary.txt"
    echo "    setup WNS: msta ${msta_setup}  OpenSTA ${opensta_setup}  delta ${delta}" >> "${OUT}/summary.txt"

    if [[ "${name}" == "eth_sky130" && "${status}" != "ok" ]]; then
        failures=$((failures + 1))
    fi
}

: > "${OUT}/summary.txt"
run_case eth_sky130  eth_sky130   eth.dofile
run_case e902_sky130 e902_sky130  e902.dofile

printf '\nlogs: %s\n' "${OUT}"
echo "note: e902_sky130 is reported only; its ~170 stage paths accumulate"
echo "      per-stage modelling differences between the two tools."

if (( failures > 0 )); then
    echo "==> compare failed: eth_sky130 setup WNS delta over ${TOLERANCE_NS} ns" >&2
    exit 1
fi
echo "==> compare passed"
