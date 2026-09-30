# get_* -of_objects：按对象关系推集合。
# f1 的数据脚 D 是端点，切掉它 → f1 的路径被排除，输出端口 out 的路径照常报。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

# get_cells → 它的引脚；模式 D 只挑数据脚
set_false_path -to [get_pins -of_objects [get_cells f1] D]
