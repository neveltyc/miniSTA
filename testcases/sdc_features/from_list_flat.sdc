# from_list.sdc 的平铺写法：同样的约束，列表里只写名字。
source generated.sdc
set_false_path -setup -from {d sink/Q}
set_false_path -hold -to [get_pins {divider/D sink/D}]
set_multicycle_path -hold 1 -from {sink} -to {q}
group_path -name out_grp -from {d sink} -through {sink/Q q} -to {q divider/D}
set_input_transition 0.2 d clk
