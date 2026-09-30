# 前端容错：拼错的命令名、未建模的集合选项都只丢自己那条，其余约束照常生效。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_max_transtion 0.1 [get_ports out]
set_input_delay -clock core -max 5.0 [get_ports {inp1 inp2}]
set_input_delay -clock core -min 0.0 [get_nets -expression "direction==in" {inp1 inp2}]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]
set_false_path -setup -from [get_ports inp1] -to [get_cells f1]
set_voltage 1.2 [get_ports inp1]
