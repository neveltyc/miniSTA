# load_subtract.sdc 的对照版本：把总负载减去脚电容（0.05 - 0.002302 = 0.047698）
# 直接写成普通 set_load，两份的时序数字应当完全一样。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_load 0.047698 [get_nets n3]
