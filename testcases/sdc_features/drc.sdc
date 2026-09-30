# 设计规则与面积约束：[current_design] 是全局限制，分对象写法见 multi_vt/drc_objects.sdc。
create_clock -name core_clk -period 5.0 [get_ports clk]
set_input_delay -clock core_clk -max 1.0 [get_ports {a_* b_* sel rst_n}]
set_output_delay -clock core_clk -max 1.0 [get_ports {y_*}]
set_input_transition 0.05 [get_ports {a_* b_* sel rst_n clk}]
set_load 0.01 [get_ports {y_*}]
set_max_transition 0.05 [current_design]
set_max_fanout 4 [current_design]
set_max_capacitance 0.02 [current_design]
set_min_capacitance 0.002 [current_design]
set_max_area 1000
