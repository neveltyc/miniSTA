# set_clock_sense -positive：强制反相器输出上的时钟"不倒相"，
# 捕捉沿回到 rise 沿（下一拍的 20.0），slack 相差一个半周期。
create_clock -name clk -period 20.0 -waveform {0.0 10.0} [get_ports clk]
set_propagated_clock [get_clocks clk]
set_clock_sense -positive [get_pins u_inv/Y]
set_input_delay -clock clk -max 1.0 [get_ports d]
set_input_delay -clock clk -min 0.0 [get_ports d]
set_input_transition 0.05 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
set_load 0.01 [get_ports q]
