create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
create_generated_clock -name derived -source [get_ports clk] -divide_by 1 [get_pins u_common/Y]
set_propagated_clock [get_clocks derived]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
