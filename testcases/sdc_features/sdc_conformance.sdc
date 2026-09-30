# 手册里有、但 msta 没建模的选项（这里是 DC 的 -expression）：报告后整条命令作废，
# 免得拿一个含义不同的集合去约束设计。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_nets -expression "fanout>1" my_cti]
set_multicycle_path 2 -start -from [get_ports my_cfi] -to [get_pins u_sff/D]
# "-max 0.5 -min 0.2" 这种连着给两个值的写法不是 SDC 1.8 语法（手册里 -min/-max
# 是开关，值只有一个），整条命令作废，不会按其中一个值去约束。
set_ideal_latency -max 0.5 -min 0.2 [get_ports tau2015_clk]
# 同一个源脚上的第二个时钟必须写 -add（不写是"替换"，msta 不做替换，整条拒绝）。
create_clock -name shift_clk_fast -period 10.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports my_cti]
