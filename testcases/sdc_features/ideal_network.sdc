# 基准：时钟树按 Liberty 弧传播，4 级 CLKBUF1 的插入延迟会算进来。
create_clock -name core -period 10.0 [get_ports clk]
set_propagated_clock [get_clocks core]
set_input_delay -clock core -max 1.0 [get_ports d_in]
set_input_delay -clock core -min 0.0 [get_ports d_in]
set_input_transition 0.05 [get_ports d_in]
set_output_delay -clock core -max 1.0 [get_ports q_out]
set_output_delay -clock core -min 0.0 [get_ports q_out]
set_load 0.01 [get_ports q_out]
