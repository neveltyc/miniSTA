# get_cells -filter：属性来自设计索引，表达式交给 Tcl 的 expr 求值
# （属性名会补成 $name，裸词加引号，=~ / !~ 翻成通配匹配）。
# f1 是 sky130_fd_sc_hd__dfxtp_1（=~ 是通配匹配）：切掉它出发的路径 → 输出端口那条被排除，
# 而终点是寄存器 f1 的那条（起点是输入端口）照常报。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_false_path -from [get_cells -filter {ref_name =~ *dfxtp*}]
