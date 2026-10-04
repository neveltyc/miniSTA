create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -late -data -fall 2
set_timing_derate -late -data -rise 0.5
set_timing_derate -early -data -rise 2
set_timing_derate -early -data -fall 0.5
