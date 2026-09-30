# group_path：命中的路径归到命名组，报告里按组出 WNS/TNS。分组不改变 slack。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

# 到寄存器的路径归 reg_grp（权重 2.0，只记录），到输出端口的路径归 out_grp。
group_path -name reg_grp -weight 2.0 -to [get_pins f1/D]
group_path -name out_grp -to [get_ports out]
