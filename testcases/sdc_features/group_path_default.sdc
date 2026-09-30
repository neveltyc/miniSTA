# group_path -default：命名组都没命中时落到 **default** 组。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

group_path -name reg_grp -to [get_pins f1/D]
group_path -default -to [get_ports out]
