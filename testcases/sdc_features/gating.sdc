# 时钟门控检查：门控单元 u_icg 的使能脚 GATE 相对时钟脚 CLK 的 setup/hold。
# 写了 set_clock_gating_check 时用它给的值，覆盖库里使能脚约束弧的值。
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports {en d}]
set_input_delay -clock clk -min 0.5 [get_ports {en d}]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]

set_clock_gating_check -setup 0.400 -hold 0.100 [get_cells u_icg]
