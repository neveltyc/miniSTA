# -from/-to 直接用 get_pins：寄存器 Q 脚到 D 脚的反馈路径被排除。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports my_cti]
set_false_path -from [get_pins u_sff/Q] -to [get_pins u_sff/D]
