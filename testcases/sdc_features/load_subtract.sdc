# set_load -subtract_pin_load：给的值就是"总负载"，网络上原有的脚电容不再另加。
# n3 上只有 INVX1 的 A 脚（0.00932456 pF），这里注解 0.05 pF 总负载，
# 等价于"set_load 0.040676 [get_nets n3]"（见 load_subtract_literal.sdc）。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_load 0.05 -subtract_pin_load [get_nets n3]
