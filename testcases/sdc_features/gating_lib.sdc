# 不写 set_clock_gating_check 时，检查值取库里使能脚上的 setup_rising/hold_rising 弧。
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports {en d}]
set_input_delay -clock clk -min 0.5 [get_ports {en d}]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
