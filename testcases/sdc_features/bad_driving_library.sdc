create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core 1.0 [get_ports inp1]
set_driving_cell -library no_such_lib -lib_cell sky130_fd_sc_hd__inv_1 [get_ports inp1]
