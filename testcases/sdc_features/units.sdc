set_units -time ps -capacitance pf
create_clock -name tau2015_clk -period 50000 [get_ports tau2015_clk]
source units_io.sdc
