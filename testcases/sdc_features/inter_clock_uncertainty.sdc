# 时钟间不确定度：只有跨这一对时钟的检查才用得到。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
create_clock -name vclk -period 50.0
set_input_delay -clock core -max 5.0 [get_ports {inp1 inp2}]
set_input_delay -clock core -min 0.0 [get_ports {inp1 inp2}]
set_input_transition 0.05 [get_ports {inp1 inp2}]
set_load 0.05 [get_ports out]
set_output_delay -clock vclk -max 30.0 [get_ports out]
set_output_delay -clock vclk -min -10.0 [get_ports out]
set_clock_uncertainty -from core -to core -setup 0.5
set_clock_uncertainty -from core -to vclk -hold 0.2
