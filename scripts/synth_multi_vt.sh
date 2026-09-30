#!/usr/bin/env bash
#
# 用 icsprout55 PDK 的三个阈值库综合 testcases/multi_vt：
#   picorv32 核 -> LVT(ics55_LLSC_H7CL)，timer8 -> HVT(H7CH)，gpio8 -> RVT(H7CR)。
# 三个块分别做工艺映射，最后和顶层互连组装成一份网表，所以网表里同时有三种阈值的单元。
#
# 完整库有 3x78MB，不进仓库；先放到 vendor/ics55/（在 .gitignore 里）：
#   bash scripts/fetch_ics55_liberty.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="${ICS55_LIB_DIR:-${ROOT}/vendor/ics55}"
LVT="${LIB_DIR}/ics55_LLSC_H7CL_typ_tt_1p2_25_nldm.lib"
RVT="${LIB_DIR}/ics55_LLSC_H7CR_typ_tt_1p2_25_nldm.lib"
HVT="${LIB_DIR}/ics55_LLSC_H7CH_typ_tt_1p2_25_nldm.lib"

for f in "${LVT}" "${RVT}" "${HVT}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: missing ${f}" >&2
        echo "       run scripts/fetch_ics55_liberty.sh first" >&2
        exit 1
    fi
done

RTL="${ROOT}/testcases/multi_vt/rtl"
OUT="${ROOT}/testcases/multi_vt/netlist"
LOG="${ROOT}/build/synth_multi_vt"
mkdir -p "${OUT}" "${LOG}"

# 每个块单独映射到自己的阈值库。
map_block () {   # <模块名> <lib>
    yosys -q -l "${LOG}/$1.log" -p "
        read_verilog ${RTL}/$1.v;
        hierarchy -check -top $1;
        synth -top $1 -flatten -noabc;
        dfflibmap -liberty $2;
        abc -liberty $2;
        opt_clean;
        write_verilog -noattr ${LOG}/blk_$1.v;
    "
}

map_block picorv32 "${LVT}"
map_block timer8   "${HVT}"
map_block gpio8    "${RVT}"

# 组装：三个块已经是门级黑盒，只有顶层那点胶合逻辑需要再过一次映射（用 RVT 库），
# 这样不会把已经映射好的块重新映射掉。
yosys -q -l "${LOG}/assemble.log" -p "
    read_verilog ${LOG}/blk_picorv32.v ${LOG}/blk_timer8.v ${LOG}/blk_gpio8.v ${RTL}/soc_top.v;
    hierarchy -top soc_top;
    flatten;
    techmap;
    opt_clean;
    abc -liberty ${RVT};
    opt_clean -purge;
    splitnets -ports;
    opt -fast -purge; opt_clean -purge;
    write_verilog -noattr ${OUT}/multi_vt_soc.v;
"

# 总线位名转成纯标识符（和 sky130 用例保持一致，别的工具前端更好读）。
sed -E -e 's/\\([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]/\1_\2/g' \
       -e 's/([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]/\1_\2/g' \
       "${OUT}/multi_vt_soc.v" > "${OUT}/multi_vt_soc.flat.v"
mv "${OUT}/multi_vt_soc.flat.v" "${OUT}/multi_vt_soc.v"

echo "==> ${OUT}/multi_vt_soc.v"
for flavor in H7L H7R H7H; do
    printf '    %s cells: %s instances\n' "${flavor}" \
        "$(grep -oE "[A-Za-z0-9_]+${flavor}" "${OUT}/multi_vt_soc.v" | wc -l | tr -d ' ')"
done
