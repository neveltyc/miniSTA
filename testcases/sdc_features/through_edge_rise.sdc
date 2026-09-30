# -rise_through 只切 X 上的上升数据路径；下降路径仍然参与检查。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty 0.2 [get_clocks shift_clk]
set_false_path -rise_through [get_pins u_mux/X]
