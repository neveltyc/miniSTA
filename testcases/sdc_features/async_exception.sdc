# recovery/removal 端点同样接受路径例外：异步复位脚被排除后不再报它的检查。
create_clock -name core_clk -period 5.0 [get_ports clk]
set_input_delay -clock core_clk -max 1.0 [get_ports {a_* b_* sel rst_n}]
set_input_delay -clock core_clk -min 0.0 [get_ports {a_* b_* sel rst_n}]
set_output_delay -clock core_clk -max 1.0 [get_ports {y_*}]
set_output_delay -clock core_clk -min 0.0 [get_ports {y_*}]
set_false_path -to [get_pins {_150_/RESET_B _151_/RESET_B}]
