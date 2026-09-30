# group_path 与路径例外一起用：端点被 false path 切掉后换到次优路径，
# 分组要跟着"最终获胜的那条路径"走，而不是停在被切掉的那条上。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
set_output_delay -clock shift_clk 2.0 [get_ports {my_cfo my_cto}]
set_clock_uncertainty 0.2 [get_clocks shift_clk]

set_false_path -fall_through [get_pins u_mux/X]
group_path -name mux_grp -through [get_cells u_mux]
group_path -default
