create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
create_clock -add -name alias -period 10 [get_ports clk]
set_propagated_clock [get_clocks alias]
set_cppr 1
