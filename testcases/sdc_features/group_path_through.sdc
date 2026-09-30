# group_path 也接受 -from/-through/-rise_from 这类集合选项：
# 从 inp1 出发、经过 u1（NAND2X1）到寄存器的路径归 in_grp，其余落 **default** 组。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

group_path -name in_grp -from [get_ports inp1] -through [get_cells u1]
group_path -default
