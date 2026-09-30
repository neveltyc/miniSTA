# Tcl 语法、变量、列表、标准选项顺序，以及时钟组。
set period 50.0
create_clock -name core -period $period [get_ports tau2015_clk]
create_clock -name external -period $period
create_clock -name spare -period $period
set_input_delay -max -clock external 5.0 [get_ports {inp1 inp2}]
set_input_delay -min -clock external 0.0 [get_ports {inp1 inp2}]
set_output_delay -max -clock core 30.0 [get_ports out]
set_output_delay -min -clock core -10.0 [get_ports out]
set_clock_groups -asynchronous -group {external spare} -group [get_clocks core]
