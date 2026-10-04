create_clock -name core -period 10 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_transition 0.1 [get_ports clk]
set_operating_conditions -analysis_type bc_wc -min_library ocv_fast -max_library ocv_test
