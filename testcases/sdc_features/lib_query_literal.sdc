# lib_query.sdc 的对照版本：库对象直接写名字，结果应当与用查询的版本完全一致。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_transition 0.05 [get_ports {inp1 inp2 tau2015_clk}]
set_input_delay -clock core -max 5.0 [get_ports inp1]
set_input_delay -clock core -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_load 0.05 [get_ports out]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]

set_driving_cell -lib_cell NAND2X1 -pin Y -from_pin A [get_ports inp1]
