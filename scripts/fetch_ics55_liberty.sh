#!/usr/bin/env bash
#
# 下载 icsprout55 PDK 的三个阈值库（Apache-2.0）到 vendor/ics55/，只取 typical 角，
# 每个库约 78MB。完整库不进仓库，综合脚本 scripts/synth_multi_vt.sh 用它们。
#
#   用法: bash scripts/fetch_ics55_liberty.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ICS55_LIB_DIR:-${ROOT}/vendor/ics55}"
TAG="${ICS55_RELEASE:-v1.10.102}"
BASE="https://github.com/openecos-projects/icsprout55-pdk/releases/download/${TAG}"
mkdir -p "${OUT}"

for flavor in H7CH H7CL H7CR; do   # H=高阈值, L=低阈值, R=标准阈值
    lib="ics55_LLSC_${flavor}_typ_tt_1p2_25_nldm.lib"
    if [[ -f "${OUT}/${lib}" ]]; then
        echo "==> ${lib} 已存在，跳过"
        continue
    fi
    echo "==> 下载 ${flavor} liberty（约 78MB）"
    curl -sSL --max-time 900 -o "${OUT}/tmp.tar.bz2" \
        "${BASE}/ics55_LLSC_${flavor}_liberty.tar.bz2"
    tar -xjf "${OUT}/tmp.tar.bz2" -C "${OUT}" "liberty/${lib}"
    mv "${OUT}/liberty/${lib}" "${OUT}/${lib}"
    rmdir "${OUT}/liberty"
    rm -f "${OUT}/tmp.tar.bz2"        # 解压完就删掉压缩包，省空间
done

ls -la "${OUT}"
