# 命令解析规则：下面每一条写错的约束都整条作废（告警并计入忽略数），
# 其余约束照常生效，所以时序结果与只读 generated.sdc 完全相同。
source generated.sdc

# 不认识的选项
set_input_delay -clock master -max 3.0 -foo [get_ports d]
# 带值的选项缺值（-name 后面紧跟的是另一个选项）
create_clock -name -period 5.0 [get_ports d]
# 同一个选项写了两次
set_output_delay -clock master -max 5.0 -max [get_ports q]
# 负数是值，不是选项：-2 被当成周期数，而周期数必须是正整数
set_multicycle_path -2 -setup -to [get_ports q]
# 周期数不是整数
set_multicycle_path 2.5 -setup -to [get_ports q]
# 只有一个值的命令写了两个数（"-max 2 -min 1" 不是 SDC 1.8 语法）
set_input_delay -clock master -max 2.0 -min 1.0 [get_ports d]
# 对象列表里混进了数值
set_load 0.05 [get_ports q] 0.02
# set_case_analysis 只接受 0、1、zero、one；rising/falling 不建模
set_case_analysis 2 [get_ports d]
set_case_analysis rising [get_ports d]
# 不该为负的值
set_load -0.05 [get_ports q]
# 给了对象但集合为空：作废整条，而不是退化成"不写对象 = 全局"
set_timing_derate -late 3.0 [get_clocks nomatch*]
set_timing_derate -late 3.0 [get_cells -of_objects [get_nets divclk] nomatch*]
# 选项只带紧跟的一个参数。set_disable_timing 的 -from 只能是一个库引脚名，
# 写成列表 {CLK D} 作废（D 不会被当成实例）
set_disable_timing -from {CLK D} -to Q [get_cells sink]
# 多个起点要写成列表 {d clk} 或集合；裸写的 clk 是多出来的位置参数
set_false_path -from d clk
