# set_timing_derate 分对象：分对象的值覆盖全局值（不是相乘）。
#   全局 -late 1.1            → 路径上所有单元延迟 ×1.1（含 FF 的 clk-to-Q）
#   u2（INVX1）另给 -late 1.2 → 只 u2 用 1.2，其它仍是 1.1
#   -cell_check 分对象        → f1 的 setup 检查值 ×1.2
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_timing_derate -late 1.1
set_timing_derate -late 1.2 [get_cells u2]
set_timing_derate -late 1.2 -cell_check [get_cells f1]
