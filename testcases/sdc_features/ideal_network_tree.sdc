# set_ideal_network 把时钟端口标成理想网络：插入延迟不再累计，整棵树都当 0
# （基准用例里是 0.315 ns，且 slack 随时钟偏差变化）。
create_clock -name core -period 10.0 [get_ports clk]
set_propagated_clock [get_clocks core]
set_ideal_network [get_ports clk]
set_input_delay -clock core -max 1.0 [get_ports d_in]
set_input_delay -clock core -min 0.3 [get_ports d_in]
set_input_transition 0.05 [get_ports d_in]
set_output_delay -clock core -max 1.0 [get_ports q_out]
set_output_delay -clock core -min 0.0 [get_ports q_out]
set_load 0.01 [get_ports q_out]
