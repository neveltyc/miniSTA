# create_generated_clock -edges {1 3 5}：用主时钟的第 1、3、5 个边沿定义新时钟。
# 边号从 1 开始数（1=首个上升沿，2=首个下降沿，3=第二个上升沿……），主时钟的
# 波形是 1.0/4.0、周期 10，所以新时钟是 rise@1、fall@11、下一个 rise@21：
# 周期 20，和 generated.sdc 的 -divide_by 2 完全等价（两份的时序数字应当一致）。
# 这里只看寄存器路径（divider -> sink），两边工具的检查口径一致。
set p 10.0
create_clock -name master -period $p -waveform {1.0 4.0} [get_ports clk]
create_generated_clock -name slow -source [get_ports clk] -edges {1 3 5} [get_pins divider/Q]
set_input_transition 0.03 [get_ports clk]
set_input_transition 0.05 [get_ports d]
set_input_delay -clock master -max 1.0 [get_ports d]
set_input_delay -clock master -min 0.0 [get_ports d]
set_clock_latency -source -max 0.5 [get_clocks master]
set_clock_latency -source -min 0.2 [get_clocks master]
set_clock_uncertainty -setup -hold 0.1 [get_clocks slow]
