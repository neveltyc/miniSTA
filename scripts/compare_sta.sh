#!/usr/bin/env bash
#
# 在小用例 testcases/sta_compare 上把 msta 和 OpenSTA/OpenTimer 对比。
# 缺哪个工具就跳过哪个；在的工具必须和 msta 的 WNS 对得上。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CASE="${ROOT}/testcases/sta_compare"
OUT="${COMPARE_OUT:-${ROOT}/build/compare_sta}"
MSTA_BIN="${MSTA_BIN:-${ROOT}/build/msta}"
OPENSTA_BIN="${OPENSTA_BIN:-${ROOT}/vendor/build/opensta/sta}"
TOLERANCE_NS="${TOLERANCE_NS:-0.05}"

mkdir -p "${OUT}"

if [[ ! -x "${MSTA_BIN}" ]]; then
    echo "error: msta not found at ${MSTA_BIN}; run make first" >&2
    exit 1
fi

# 本地 vendor 构建存在时才用它自带的 sysroot。
if [[ -d "${ROOT}/vendor/build/cudd-install/lib" ]]; then
    export LD_LIBRARY_PATH="${ROOT}/vendor/build/cudd-install/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
if [[ -d "${ROOT}/vendor/sysroot/usr/lib/x86_64-linux-gnu" ]]; then
    export LD_LIBRARY_PATH="${ROOT}/vendor/sysroot/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

if [[ -z "${OPENTIMER_BIN:-}" ]]; then
    OPENTIMER_BIN="$(find "${ROOT}/vendor/opentimer" -type f -path '*/bin/ot-shell' 2>/dev/null | head -n 1 || true)"
fi
if [[ ! -x "${OPENSTA_BIN}" && -x "${ROOT}/vendor/opensta-build/sta" ]]; then
    OPENSTA_BIN="${ROOT}/vendor/opensta-build/sta"
fi

parse_msta() {
    awk -v corner="$2" '
        $0 ~ "^" corner "[[:space:]]*:" {
            for (i = 1; i <= NF; i++) {
                if ($i == "WNS") wns = $(i + 1);
                if ($i == "TNS") tns = $(i + 1);
            }
        }
        END { print wns, tns }
    ' "$1"
}

parse_opensta() {
    awk -v kind="$1" '
        $0 ~ "worst slack " kind "[[:space:]]" { wns = $4 }
        $0 ~ "tns " kind "[[:space:]]" { tns = $3 }
        END { print wns, tns }
    ' "$2"
}

parse_opentimer() {
    awk '
        /Analysis type[[:space:]]*:/ {
            split($0, a, ":");
            kind = a[2];
            gsub(/[[:space:]]/, "", kind);
        }
        /^[[:space:]]*slack[[:space:]]/ {
            if (kind == "max" && (max == "" || $2 < max)) max = $2;
            if (kind == "min" && (min == "" || $2 < min)) min = $2;
        }
        END { print max, min }
    ' "$1"
}

compare_value() {
    awk -v label="$1" -v a="$2" -v b="$3" -v tolerance="$4" 'BEGIN {
        delta = a - b;
        if (delta < 0) delta = -delta;
        if (delta > tolerance) {
            printf "FAIL %s: msta=%s reference=%s delta=%.6f ns (limit=%s ns)\n",
                   label, a, b, delta, tolerance;
            exit 1;
        }
        printf "PASS %s: msta=%s reference=%s delta=%.6f ns\n", label, a, b, delta;
    }'
}

echo "==> msta"
"${MSTA_BIN}" -q "${CASE}/msta.dofile" > "${OUT}/msta.log" 2>&1
read -r MSTA_SETUP_WNS MSTA_SETUP_TNS < <(parse_msta "${OUT}/msta.log" setup)
read -r MSTA_HOLD_WNS MSTA_HOLD_TNS < <(parse_msta "${OUT}/msta.log" hold)

echo "==> OpenSTA"
if [[ -x "${OPENSTA_BIN}" ]]; then
    (cd "${CASE}" && "${OPENSTA_BIN}" -no_splash -no_init opensta.tcl) \
        > "${OUT}/opensta.log" 2>&1
    read -r OPENSTA_SETUP_WNS OPENSTA_SETUP_TNS < <(parse_opensta max "${OUT}/opensta.log")
    read -r OPENSTA_HOLD_WNS OPENSTA_HOLD_TNS < <(parse_opensta min "${OUT}/opensta.log")
else
    echo "SKIP: OpenSTA not found (set OPENSTA_BIN to enable)"
fi

echo "==> OpenTimer"
if [[ -n "${OPENTIMER_BIN:-}" && -x "${OPENTIMER_BIN}" ]]; then
    (cd "${CASE}" && "${OPENTIMER_BIN}" -q -i opentimer.cmd) \
        > "${OUT}/opentimer.log" 2>&1
    read -r OPENTIMER_SETUP_WNS OPENTIMER_HOLD_WNS < <(parse_opentimer "${OUT}/opentimer.log")
else
    echo "SKIP: OpenTimer not found (set OPENTIMER_BIN to enable)"
fi

printf "\n%-12s %14s %14s\n" "tool" "setup WNS" "hold WNS"
printf "%-12s %14s %14s\n" msta "${MSTA_SETUP_WNS:-n/a}" "${MSTA_HOLD_WNS:-n/a}"
if [[ -n "${OPENSTA_SETUP_WNS:-}" ]]; then
    printf "%-12s %14s %14s\n" OpenSTA "${OPENSTA_SETUP_WNS}" "${OPENSTA_HOLD_WNS}"
fi
if [[ -n "${OPENTIMER_SETUP_WNS:-}" ]]; then
    printf "%-12s %14s %14s\n" OpenTimer "${OPENTIMER_SETUP_WNS}" "${OPENTIMER_HOLD_WNS}"
fi
printf "\nlogs: %s\n" "${OUT}"

failures=0
if [[ -n "${OPENSTA_SETUP_WNS:-}" ]]; then
    compare_value "setup WNS vs OpenSTA" "${MSTA_SETUP_WNS}" "${OPENSTA_SETUP_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
    compare_value "hold WNS vs OpenSTA" "${MSTA_HOLD_WNS}" "${OPENSTA_HOLD_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
fi
if [[ -n "${OPENTIMER_SETUP_WNS:-}" ]]; then
    compare_value "setup WNS vs OpenTimer" "${MSTA_SETUP_WNS}" "${OPENTIMER_SETUP_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
    compare_value "hold WNS vs OpenTimer" "${MSTA_HOLD_WNS}" "${OPENTIMER_HOLD_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
fi

if (( failures > 0 )); then
    echo "==> compare failed: ${failures} difference(s)" >&2
    exit 1
fi
if [[ -x "${OPENSTA_BIN}" ]]; then
    python3 "${ROOT}/scripts/compare_check_edges.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
    python3 "${ROOT}/scripts/check_clock_edges.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
    python3 "${ROOT}/scripts/check_nonunate_clock.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
fi
echo "==> compare passed"
