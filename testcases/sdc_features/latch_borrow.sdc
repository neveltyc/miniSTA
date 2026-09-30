# set_max_time_borrow：限制锁存器能"借"的时间。
# 不写命令时要求 D 在关闭沿前 setup 时间到（借满整个开窗）；写了 v 之后按参考工具
# 的口径换成"数据必须在开沿后 v 之内到"，v 越小越紧。
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports d]
set_input_delay -clock clk -min 0.5 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]

set_max_time_borrow 0.5 [get_cells u_lat]
