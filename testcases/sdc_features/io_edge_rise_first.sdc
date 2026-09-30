create_clock -name core -period 50.0 -waveform {5.0 20.0} [get_ports tau2015_clk]
set_input_delay -clock core -max -rise 1.0 [get_ports inp1]
set_input_delay -clock core -max -fall 5.0 -add_delay [get_ports inp1]
set_output_delay -clock core 2.0 [get_ports out]
