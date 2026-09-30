# msta 和 OpenSTA 共用：总线端口两边都用 * 通配。
create_clock -name core_clk -period 5.0 [get_ports clk]

set_input_delay -clock core_clk -max 1.0 [get_ports {a_* b_* sel rst_n}]
set_input_delay -clock core_clk -min 0.0 [get_ports {a_* b_* sel rst_n}]

set_output_delay -clock core_clk -max 1.0 [get_ports {y_*}]
set_output_delay -clock core_clk -min 0.0 [get_ports {y_*}]

# 所有主输入用同一个摆率，两个工具的查表点才一致。
set_input_transition 0.05 [get_ports {a_* b_* sel rst_n clk}]

# 输出显式给负载，免得 msta 对没有扇出的网络套用兜底负载，两边查表点不同。
set_load 0.01 [get_ports {y_*}]
