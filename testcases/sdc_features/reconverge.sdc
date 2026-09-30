create_clock -name core -period 50.0 [get_ports tau2015_clk]
create_clock -name blocked -period 50.0
set_input_delay -clock blocked -max 5.0 [get_ports inp1]
set_input_delay -clock blocked -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]
set_clock_groups -asynchronous -group [get_clocks blocked] -group [get_clocks core]
