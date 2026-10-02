# -from/-to/-through 各带一个 Tcl 参数：一个名字、一个列表或一个集合。
source generated.sdc
# 列表 {d sink/Q} 是两个起点：setup 检查的三个端点都被排除。
set_false_path -setup -from {d sink/Q}
# 集合展开出两个引脚：hold 检查只剩输出端口 q。
set_false_path -hold -to [get_pins {divider/D sink/D}]

# 列表里可以再套集合（可以多层，也可以用 concat 拼）：每个元素各自展开，合起来是
# 这个选项的对象，空元素不贡献对象。from_list_flat.sdc 用平铺写法写同样的约束，
# 两份报告逐行相同。
# hold 到 q 推后一个 slow 周期。-from：集合、空集合、空元素；-to：两层嵌套的集合。
set_multicycle_path -hold 1 \
    -from [list [get_cells sink] [remove_from_collection [get_pins sink/D] [get_pins sink/D]] {}] \
    -to [list [list [get_ports q]]]
# 端口与实例、端口与引脚混在一个列表里；-through 是名字和集合用 concat 拼成的列表。
group_path -name out_grp -from [list [get_ports d] [get_cells sink]] \
    -through [concat [get_pins sink/Q] q] -to [list [get_ports q] [get_pins divider/D]]
# 位置参数的对象也可以写成列表：{d clk} 与 from_list_flat.sdc 里分开写的 d clk 相同。
set_input_transition 0.2 {d clk}
