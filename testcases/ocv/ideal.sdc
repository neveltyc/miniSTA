create_clock -name core -period 10 [get_ports clk]
set_input_transition 0.1 [get_ports clk]
set_clock_latency -min 1 [get_clocks core]
set_clock_latency -max 3 [get_clocks core]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
