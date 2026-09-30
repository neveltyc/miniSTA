# -no_propagate：只把 ct2 的输出网络标成理想网络，理想属性不再往 ct3/ct4 传。
# 于是 c2 不再累计 ct2 的弧延迟，c3/c4 照常算，插入延迟介于两种写法之间。
create_clock -name core -period 10.0 [get_ports clk]
set_propagated_clock [get_clocks core]
set_ideal_network -no_propagate [get_pins ct2/Y]
set_input_delay -clock core -max 1.0 [get_ports d_in]
set_input_delay -clock core -min 0.0 [get_ports d_in]
set_input_transition 0.05 [get_ports d_in]
set_output_delay -clock core -max 1.0 [get_ports q_out]
set_output_delay -clock core -min 0.0 [get_ports q_out]
set_load 0.01 [get_ports q_out]
