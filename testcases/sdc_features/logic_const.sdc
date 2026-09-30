# set_logic_zero / set_logic_one / set_logic_dc：把端口钉成常量，路径不再穿过它。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports my_cti]
set_input_delay -clock shift_clk 2.0 [get_ports my_cfi]
set_logic_zero [get_ports my_cfi]
set_logic_one [get_ports my_capture_en]
set_logic_dc [get_ports my_cti]
