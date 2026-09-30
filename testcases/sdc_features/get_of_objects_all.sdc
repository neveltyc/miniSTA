# -of_objects 的父对象也可以是 all_* 这类整体集合（由 C 侧展开）：
# all_registers 里所有寄存器的 D 脚就是 f1/D。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]

set_false_path -to [get_pins -of_objects [all_registers] D]
