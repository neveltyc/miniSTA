# -rise_through：最差路径在 u_mux/X 上本来就是下降沿，所以这条例外不切任何路径
# （through_edge.sdc 把它换成 -fall_through 才会换路径）。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty 0.2 [get_clocks shift_clk]
set_false_path -rise_through [get_pins u_mux/X]
