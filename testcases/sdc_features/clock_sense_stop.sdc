# set_clock_sense -stop_propagation：时钟到反相器输出就不再往下传，
# 后面那级缓冲和 FF 的时钟脚都不再属于这棵树（FF 的检查算未约束）。
create_clock -name clk -period 20.0 -waveform {0.0 10.0} [get_ports clk]
set_propagated_clock [get_clocks clk]
set_clock_sense -stop_propagation [get_pins u_inv/Y]
set_input_delay -clock clk -max 1.0 [get_ports d]
set_input_delay -clock clk -min 0.0 [get_ports d]
set_input_transition 0.05 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
set_load 0.01 [get_ports q]
