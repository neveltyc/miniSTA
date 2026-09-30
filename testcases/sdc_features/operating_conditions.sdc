# set_operating_conditions：选分析用的工艺角。bc_wc 时 min/max 各选一个角；
# 库里只声明一个标称角时，也可以直接给 -voltage/-temperature，
# 有 K 因子的库会据此缩放延时表（本用例用的 sky130 库没有 K 因子，会告警
# "tables are used as read"）。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty -setup 0.2 [get_clocks shift_clk]
set_operating_conditions -analysis_type bc_wc \
    -max tt_025C_1v80 -min tt_025C_1v80
set_operating_conditions -voltage 0.95 -temperature 105
set_voltage 0.95
# on_chip_variation is rejected instead of silently running the bc_wc model.
set_operating_conditions -analysis_type on_chip_variation \
    -max tt_025C_1v80 -min tt_025C_1v80
