# 顺序反了就不该命中：路径先经过 u_mux，再到 u_sff/D。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports my_cti]
set_input_delay -clock shift_clk 2.0 [get_ports my_cfi]
set_false_path -through [get_pins u_sff/D] -through [get_cells u_mux]
