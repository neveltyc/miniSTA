# msta / OpenSTA / OpenTimer 共用的约束。
# 时间单位跟随 Liberty 库（本用例是 ns）。
create_clock -name tau2015_clk -period 50.0 [get_ports tau2015_clk]

set_input_delay -clock tau2015_clk -max 5.0 [get_ports inp1]
set_input_delay -clock tau2015_clk -min 0.0 [get_ports inp1]
set_input_delay -clock tau2015_clk -max 1.0 [get_ports inp2]
set_input_delay -clock tau2015_clk -min 0.0 [get_ports inp2]

set_input_transition 0.05 [get_ports inp1]
set_input_transition 0.05 [get_ports inp2]
set_input_transition 0.03 [get_ports tau2015_clk]

set_load 0.05 [get_ports out]
set_output_delay -clock tau2015_clk -max 30.0 [get_ports out]
set_output_delay -clock tau2015_clk -min -10.0 [get_ports out]
