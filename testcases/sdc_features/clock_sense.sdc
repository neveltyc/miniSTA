# 基准：时钟树里有反相器，FF 的有效沿按 timing_sense 自动反相
# （rise 沿 0.0、fall 沿 10.0，所以捕捉沿落在 10.0）。
create_clock -name clk -period 20.0 -waveform {0.0 10.0} [get_ports clk]
set_propagated_clock [get_clocks clk]
set_input_delay -clock clk -max 1.0 [get_ports d]
set_input_delay -clock clk -min 0.0 [get_ports d]
set_input_transition 0.05 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
set_load 0.01 [get_ports q]
