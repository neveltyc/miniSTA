# Tcl 层的集合运算：约束本身还是标准写法，对象集合用 Tcl 拼出来，
# 这样不必写死端口名，也不用 all_inputs -no_clocks 这种方言选项。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set non_clock_inputs [remove_from_collection [all_inputs] [get_ports my_shift_clk]]
set_input_delay -clock shift_clk 2.0 $non_clock_inputs
