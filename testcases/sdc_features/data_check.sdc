# set_data_check：两条数据路径之间的检查。-from 的路径提供参照到达，
# -to 的路径要比它早 margin（setup）。参考工具同样只在 max 角算数据检查。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty -setup 0.2 [get_clocks shift_clk]
set_clock_uncertainty -hold 0.1 [get_clocks shift_clk]
set_data_check -from [get_ports my_cti] -to [get_pins u_sff/D] -setup 0.5
