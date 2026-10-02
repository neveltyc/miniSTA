# -rise/-fall 让分对象 derate 只管一个边沿：u2（反相器）上给 -rise 1.5，
# 只放大上升沿那一段延迟，下降沿仍然用全局的 1.1。
# out 的负载取 0.01：最差 setup 路径是 f1/Q 下降、u2/Y 上升的那条，报告里能看到
# u2 上升沿用的是 1.5。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_load 0.01 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_timing_derate -late 1.1
set_timing_derate -late 1.5 -rise [get_cells u2]
