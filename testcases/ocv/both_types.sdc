create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -late -cell_delay -cell_check 1.2
set_timing_derate -early -cell_delay -cell_check 0.8
