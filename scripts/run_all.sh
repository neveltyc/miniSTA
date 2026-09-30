#!/usr/bin/env bash
#
# 冒烟回归：把 testcases/ 下的每个 dofile 各起一个进程跑一遍；
# 任何命令报错、解析失败或非零退出都让脚本失败。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSTA="${ROOT}/build/msta"

if [[ ! -x "${MSTA}" ]]; then
    echo "error: ${MSTA} is missing; run make first" >&2
    exit 1
fi

if ! find "${ROOT}/testcases" -name '*.dofile' -type f | grep -q .; then
    echo "error: no testcases/*.dofile found" >&2
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

echo "==> ${count} testcases passed"
bash "${ROOT}/scripts/check_sdc.sh"
