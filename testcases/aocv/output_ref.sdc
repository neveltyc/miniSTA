create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_output_delay -clock core -reference_pin [get_pins u_common/Y] -max -10 [get_ports q]
