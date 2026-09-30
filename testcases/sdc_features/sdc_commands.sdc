# SDC 1.8 手册 Appendix A 的全部命令各来一条：建模的照常生效，没建模的告警后
# 忽略，只丢自己那一条。这条用例保证"别家工具导出的约束文件能整份读完"，
# 日志里不应出现 unknown sdc command。
current_instance
expr 1 + 1
list a b
set sweep_var 1
set_hierarchy_separator /
set_units -time ns -capacitance pf

all_inputs
all_outputs
all_clocks
all_registers
current_design
get_cells u_mux
get_clocks shift_clk
get_libs xx
get_lib_cells xx
get_lib_pins xx
get_nets mux_out
get_pins u_mux/X
get_ports my_shift_clk

create_clock -name sweep_clk -period 10.0 [get_ports my_shift_clk]
create_generated_clock -name sweep_gen -source [get_ports my_shift_clk] \
    -divide_by 2 [get_pins u_sff/Q]
group_path -name g1 -from [get_ports my_cti] -to [get_pins u_sff/D]
set_clock_gating_check -setup 0.5 [get_cells u_mux]
set_clock_groups -asynchronous -group {sweep_clk} -group {sweep_gen}
set_clock_latency -max 0.2 [get_clocks sweep_clk]
set_clock_sense -positive [get_pins u_sff/CLK]
set_clock_transition 0.1 [get_clocks sweep_clk]
set_clock_uncertainty -setup 0.1 [get_clocks sweep_clk]
set_data_check -from [get_ports my_cti] -to [get_pins u_sff/D]
set_disable_timing -from A0 -to X [get_cells u_mux]
set_false_path -from [get_ports my_cfi] -to [get_pins u_sff/D]
set_ideal_latency 0.1 [get_ports my_shift_clk]
set_ideal_network [get_ports my_shift_clk]
set_ideal_transition 0.05 [get_ports my_shift_clk]
set_input_delay -clock sweep_clk -max 1.0 [get_ports my_cti]
set_max_delay 5.0 -from [get_ports my_cti] -to [get_pins u_sff/D]
set_max_time_borrow 1.0 [get_cells u_mux]
set_min_delay 0.2 -from [get_ports my_cti] -to [get_pins u_sff/D]
set_multicycle_path 2 -setup -from [get_ports my_cti] -to [get_pins u_sff/D]
set_output_delay -clock sweep_clk -max 1.0 [get_ports my_cfo]
set_propagated_clock [get_clocks sweep_clk]

set_case_analysis 0 [get_ports my_shift_en]
set_drive 1.0 [get_ports my_cti]
set_driving_cell -lib_cell INVX1 [get_ports my_cfi]
set_fanout_load 3 [get_ports my_cfi]
set_input_transition 0.05 [get_ports my_cti]
set_load 0.01 [get_ports my_cfo]
set_logic_dc [get_nets my_cto]
set_logic_one [get_nets my_cto]
set_logic_zero [get_nets my_cto]
set_max_area 100000
set_max_capacitance 0.05 [get_ports my_cti]
set_max_fanout 8 [get_ports my_cti]
set_max_transition 0.2 [get_ports my_cti]
set_min_capacitance 0.001 [get_ports my_cti]
set_operating_conditions typical
set_port_fanout_number 4 [get_ports my_cfi]
set_resistance 1.0 [get_ports my_cti]
set_timing_derate -early 0.9 -late 1.1
set_voltage 1.2 -object_list [get_ports my_cti]
set_wire_load_min_block_size 1000
set_wire_load_mode top
set_wire_load_model -name small
set_wire_load_selection_group -name g
create_voltage_area -name va1 -coordinate {0 0 10 10}
set_level_shifter_strategy -rule low_to_high
set_level_shifter_threshold -voltage 1.0
set_max_dynamic_power 10.0
set_max_leakage_power 1.0
