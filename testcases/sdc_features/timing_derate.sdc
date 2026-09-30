# 全局 OCV：early 作用在 min（hold）角，late 作用在 max（setup）角。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_timing_derate -early 0.9
set_timing_derate -late 1.1
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_input_transition 0.05 [get_ports {inp1 inp2}]
set_input_transition 0.03 [get_ports tau2015_clk]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]
