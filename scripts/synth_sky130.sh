#!/usr/bin/env bash
#
# 把 testcases/synth_sky130/rtl 下的 RTL 综合成 sky130 门级网表。
# 生成结果进仓库，这样跑 STA 不需要 Yosys。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="${ROOT}/testcases/lib/sky130.lib"
RTL="${ROOT}/testcases/synth_sky130/rtl/pipe_demo.v"
OUT_DIR="${ROOT}/testcases/synth_sky130/netlist"
LOG_DIR="${ROOT}/build/synth_sky130"

mkdir -p "${OUT_DIR}" "${LOG_DIR}"

yosys -q -l "${LOG_DIR}/synth.log" -p "
    read_verilog -sv ${RTL};
    hierarchy -check -top pipe_demo;
    synth -top pipe_demo -flatten -noabc;
    dfflibmap -liberty ${LIB};
    abc -liberty ${LIB};
    opt_clean;
    splitnets -ports;
    opt -fast -purge;
    opt_clean -purge;
    write_verilog -noattr ${OUT_DIR}/pipe_demo.v;
    write_json ${LOG_DIR}/pipe_demo.json;
    stat;
"

# OpenTimer 的 Verilog 前端把 "[" "]" 当分隔符，所以入库的网表里把总线位
# 写成 name_index 这样的普通标识符。
sed -E -e 's/\\([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]/\1_\2/g' \
       -e 's/([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]/\1_\2/g' \
       "${OUT_DIR}/pipe_demo.v" > "${OUT_DIR}/pipe_demo.flat.v"
mv "${OUT_DIR}/pipe_demo.flat.v" "${OUT_DIR}/pipe_demo.v"

echo "==> ${OUT_DIR}/pipe_demo.v"
