create_clock -period 50.0 -name tau2015_clk [get_ports tau2015_clk]
set_input_delay 0.0 -min -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 25.0 -min -fall [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 0.0 -max -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 25.0 -max -fall [get_ports tau2015_clk] -clock tau2015_clk

set_input_delay 0.0 -min -rise [get_ports inp1] -clock tau2015_clk
set_input_delay 0.0 -min -fall [get_ports inp1] -clock tau2015_clk
set_input_delay 5.0 -max -rise [get_ports inp1] -clock tau2015_clk
set_input_delay 5.0 -max -fall [get_ports inp1] -clock tau2015_clk
set_input_delay 0.0 -min -rise [get_ports inp2] -clock tau2015_clk
set_input_delay 0.0 -min -fall [get_ports inp2] -clock tau2015_clk
set_input_delay 1.0 -max -rise [get_ports inp2] -clock tau2015_clk
set_input_delay 1.0 -max -fall [get_ports inp2] -clock tau2015_clk

set_input_transition 0.05 -min -rise [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -min -fall [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -max -rise [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -max -fall [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -min -rise [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -min -fall [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -max -rise [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -max -fall [get_ports inp2] -clock tau2015_clk
set_input_transition 0.03 -min -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -min -fall [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -max -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -max -fall [get_ports tau2015_clk] -clock tau2015_clk

set_load -pin_load 0.05 [get_ports out]
set_output_delay -10.0 -min -rise [get_ports out] -clock tau2015_clk
set_output_delay -10.0 -min -fall [get_ports out] -clock tau2015_clk
set_output_delay 30.0 -max -rise [get_ports out] -clock tau2015_clk
set_output_delay 30.0 -max -fall [get_ports out] -clock tau2015_clk
