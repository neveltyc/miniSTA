create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
set_max_delay 8 -from [get_cells u_launch] -to [get_cells u_capture]
set_min_delay 1 -from [get_cells u_launch] -to [get_cells u_capture]
set_cppr 1
