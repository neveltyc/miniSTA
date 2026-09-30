# -fall_through：只切掉"在 u_mux/X 上走下降沿"的那条路径，端点继续找次优路径。
# 基准（不写例外）最差路径是在 X 上下降的 my_capture_en -> my_cfo；
# 换成上升沿走同一条网络链后 slack 变大，端点仍然有检查。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty 0.2 [get_clocks shift_clk]
set_false_path -fall_through [get_pins u_mux/X]
