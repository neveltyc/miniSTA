# 单元延迟的 -rise/-fall 选择弧的输入边沿（与 OpenSTA 一致）。
# u2 为反相器，输入上升沿对应输出下降沿；其余维度沿用全局 1.1。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.01 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_timing_derate -late 1.1
set_timing_derate -late 1.5 -rise [get_cells u2]
