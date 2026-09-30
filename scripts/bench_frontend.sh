#!/usr/bin/env bash
#
# 把前端耗时拆成 Yosys 那一步和 msta 自己那一步，分别计时。
#
#   yosys              read_verilog + write_json
#   msta read_json     只读 JSON 并展平（不经过 Yosys）
#   msta read_verilog  用户平时走的端到端路径
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="${ROOT}/testcases/lib/sky130.lib"
NETLIST="${1:-${ROOT}/testcases/eth_sky130/netlist/ethernet_sky130.v}"
TOP="${2:-eth_top}"
WORK="${ROOT}/build/bench_frontend"
JSON="${WORK}/netlist.json"

if [[ ! -f "${NETLIST}" ]]; then
    echo "error: no such netlist: ${NETLIST}" >&2
    exit 1
fi
mkdir -p "${WORK}"

echo "netlist: ${NETLIST}"
echo "top    : ${TOP}"

printf '%-22s' "yosys read+write_json"
/usr/bin/time -p yosys -q -p "read_verilog ${NETLIST}; write_json ${JSON}" 2>&1 \
    | awk '/^real/{printf "%6.2f s\n", $2}'
printf '%-22s' "json size"
ls -la "${JSON}" | awk '{printf "%6.2f MB\n", $5/1048576}'

cat > "${WORK}/from_json.dofile" <<EOF
read_liberty ${LIB}
read_json ${JSON}
current_design ${TOP}
EOF
printf '%-22s' "msta read_json"
/usr/bin/time -p "${ROOT}/build/msta" -q "${WORK}/from_json.dofile" 2>&1 \
    | awk '/^real/{printf "%6.2f s\n", $2}'

cat > "${WORK}/from_verilog.dofile" <<EOF
read_liberty ${LIB}
read_verilog ${NETLIST}
current_design ${TOP}
EOF
printf '%-22s' "msta read_verilog"
/usr/bin/time -p "${ROOT}/build/msta" -q "${WORK}/from_verilog.dofile" 2>&1 \
    | awk '/^real/{printf "%6.2f s\n", $2}'

echo
echo "files: ${WORK}"
