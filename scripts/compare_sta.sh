#!/usr/bin/env bash
#
# make compare：用本地编译的参考工具检查 msta（说明见 docs/compare_sta.md）。
#   1. testcases/sta_compare：setup/hold WNS 与 OpenSTA、OpenTimer 比较，
#      容差 TOLERANCE_NS（默认 0.05 ns）。
#   2. 有 OpenSTA 时再跑三组小用例，逐个比较 setup/hold WNS（容差 0.001 ns）：
#      testcases/check_edges（compare_check_edges.py）、
#      testcases/clock_edges（check_clock_edges.py）、
#      testcases/nonunate_clock（check_nonunate_clock.py）。
# 缺哪个工具就跳过哪个；在的工具必须和 msta 对得上。
# 工具路径：OPENSTA_BIN、OPENTIMER_BIN；不设时在 vendor/ 下自动查找。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CASE="${ROOT}/testcases/sta_compare"
OUT="${COMPARE_OUT:-${ROOT}/build/compare_sta}"
MSTA_BIN="${MSTA_BIN:-${ROOT}/build/msta}"
OPENSTA_BIN="${OPENSTA_BIN:-${ROOT}/vendor/build/opensta/sta}"
TOLERANCE_NS="${TOLERANCE_NS:-0.05}"

mkdir -p "${OUT}"

if [[ ! -x "${MSTA_BIN}" ]]; then
    echo "错误：找不到 msta（${MSTA_BIN}），请先运行 make" >&2
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
    OPENTIMER_BIN="$(find "${ROOT}/vendor/opentimer-src" "${ROOT}/vendor/opentimer" \
                     -type f -path '*/bin/ot-shell' 2>/dev/null | head -n 1 || true)"
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
            printf "失败 %s：msta=%s 参考=%s 差值=%.6f ns（容差 %s ns）\n",
                   label, a, b, delta, tolerance;
            exit 1;
        }
        printf "通过 %s：msta=%s 参考=%s 差值=%.6f ns\n", label, a, b, delta;
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
    echo "跳过：没有找到 OpenSTA（设置 OPENSTA_BIN 后启用）"
fi

echo "==> OpenTimer"
if [[ -n "${OPENTIMER_BIN:-}" && -x "${OPENTIMER_BIN}" ]]; then
    (cd "${CASE}" && "${OPENTIMER_BIN}" -q -i opentimer.cmd) \
        > "${OUT}/opentimer.log" 2>&1
    read -r OPENTIMER_SETUP_WNS OPENTIMER_HOLD_WNS < <(parse_opentimer "${OUT}/opentimer.log")
else
    echo "跳过：没有找到 OpenTimer（设置 OPENTIMER_BIN 后启用）"
fi

# 一个汉字占 3 字节、显示 2 列，表头第一列宽度多给 2 才能和下面的行对齐。
printf "\n%-14s %14s %14s\n" "工具" "setup WNS" "hold WNS"
printf "%-12s %14s %14s\n" msta "${MSTA_SETUP_WNS:-n/a}" "${MSTA_HOLD_WNS:-n/a}"
if [[ -n "${OPENSTA_SETUP_WNS:-}" ]]; then
    printf "%-12s %14s %14s\n" OpenSTA "${OPENSTA_SETUP_WNS}" "${OPENSTA_HOLD_WNS}"
fi
if [[ -n "${OPENTIMER_SETUP_WNS:-}" ]]; then
    printf "%-12s %14s %14s\n" OpenTimer "${OPENTIMER_SETUP_WNS}" "${OPENTIMER_HOLD_WNS}"
fi
printf "\n日志：%s\n" "${OUT}"

failures=0
if [[ -n "${OPENSTA_SETUP_WNS:-}" ]]; then
    compare_value "setup WNS 与 OpenSTA 对比" "${MSTA_SETUP_WNS}" "${OPENSTA_SETUP_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
    compare_value "hold WNS 与 OpenSTA 对比" "${MSTA_HOLD_WNS}" "${OPENSTA_HOLD_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
fi
if [[ -n "${OPENTIMER_SETUP_WNS:-}" ]]; then
    compare_value "setup WNS 与 OpenTimer 对比" "${MSTA_SETUP_WNS}" "${OPENTIMER_SETUP_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
    compare_value "hold WNS 与 OpenTimer 对比" "${MSTA_HOLD_WNS}" "${OPENTIMER_HOLD_WNS}" "${TOLERANCE_NS}" || failures=$((failures + 1))
fi

if (( failures > 0 )); then
    echo "==> 对比失败：${failures} 项超出容差" >&2
    exit 1
fi
if [[ -x "${OPENSTA_BIN}" ]]; then
    python3 "${ROOT}/scripts/compare_check_edges.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
    python3 "${ROOT}/scripts/check_clock_edges.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
    python3 "${ROOT}/scripts/check_nonunate_clock.py" "${MSTA_BIN}" "${OPENSTA_BIN}" "${OUT}"
fi
echo "==> 对比通过"
