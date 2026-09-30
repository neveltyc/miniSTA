# 多个 -through：组内取"或"（u_no_such_cell 不会命中），组与组按路径顺序匹配。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports my_cti]
set_input_delay -clock shift_clk 2.0 [get_ports my_cfi]
set_false_path -through {u_mux u_no_such_cell} -through [get_pins u_sff/D]
