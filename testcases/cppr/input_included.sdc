create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_timing_derate -early 0.8
set_timing_derate -late 1.2
set_cppr 1
set_clock_latency -source -early 0.2 [get_clocks core]
set_clock_latency -source -late 0.6 [get_clocks core]
set_input_delay -clock core -max 1 [get_ports a]
set_input_delay -clock core -min 1 [get_ports a]
set_input_delay -clock core -source_latency_included -max 1.8 [get_ports b]
set_input_delay -clock core -source_latency_included -min 1 [get_ports b]
set_input_transition 0.1 [get_ports {a b}]
