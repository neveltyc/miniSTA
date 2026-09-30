# get_* -of_objects 的另一半：从引脚/网络反推实例。
# u2/A 所在网络的驱动实例是 f1 → 切掉 f1 出发的路径（输出端口 out 那条），
# f1 自己的端点（inp1 -> f1）照常报。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

# get_nets -of_objects [get_pins ...]：u2/A 接的那根网络 n3
# get_cells -of_objects <net>：网络上的驱动实例 -> f1
set_false_path -from [get_cells -of_objects [get_nets -of_objects [get_pins u2/A]]]
