# dedicated_wrp_cell 的约束
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]

# 不给 -max/-min：一个值同时约束 setup 和 hold，便于最小用例展示两种检查。
set_input_delay  -clock shift_clk 2.0 [get_ports my_cti]
set_input_delay  -clock shift_clk 2.0 [get_ports my_cfi]
set_input_delay  -clock shift_clk 2.0 [get_ports my_capture_en]
set_input_delay  -clock shift_clk 2.0 [get_ports my_shift_en]
set_output_delay -clock shift_clk 2.0 [get_ports my_cfo]
set_output_delay -clock shift_clk 2.0 [get_ports my_cto]

# 手册里 -setup/-hold 是开关，值只有一个：两个角分开写。
set_clock_uncertainty -setup 0.2 [get_clocks shift_clk]
set_clock_uncertainty -hold 0.1 [get_clocks shift_clk]
