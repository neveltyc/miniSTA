# all_registers -fall_clock / -rise_clock：按时钟的"有效沿"过滤。
# c1 是 clkA 上的下降沿 FF，c2 挂在反相时钟上、有效沿是上升沿，
# 所以 -fall_clock clkA 只挑出 c1（切掉 c1 -> q3 一条路径）。
# 两个-*_clock 不能写在一条命令里，这里用两次 set_false_path 观察效果：
# 第一条只切 c1，第二条（_check.sdc 里）验证上升沿那一侧。
create_clock -name clkA -period 10.0 [get_ports clk]
set_input_delay -clock clkA -max 1.0 [get_ports da]
set_input_delay -clock clkA -min 0.0 [get_ports da]
set_output_delay -clock clkA -max 1.0 [get_ports {q1 q3 q4}]

set_false_path -from [all_registers -fall_clock clkA]
