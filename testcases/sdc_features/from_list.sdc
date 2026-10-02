# -from/-to/-through 各带一个 Tcl 参数：一个名字、一个列表或一个集合。
source generated.sdc
# 列表 {d sink/Q} 是两个起点：setup 检查的三个端点都被排除。
set_false_path -setup -from {d sink/Q}
# 集合展开出两个引脚：hold 检查只剩输出端口 q。
set_false_path -hold -to [get_pins {divider/D sink/D}]
