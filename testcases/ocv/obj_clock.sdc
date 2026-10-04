create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -early -clock 0.8
set_timing_derate -late -clock 1.2
set_timing_derate -late -clock 1.5 [get_cells u_launch_clk]
set_timing_derate -early -clock 0.7 [get_cells u_capture_extra]
