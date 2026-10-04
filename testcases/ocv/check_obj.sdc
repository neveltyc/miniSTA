create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
set_timing_derate -late -cell_check -rise 1.5 [get_cells u_capture]
set_timing_derate -early -cell_check -fall 0.7 [get_cells u_capture]
