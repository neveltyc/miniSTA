# -edges 也能做出非 50% 占空比的生成时钟：-edges {1 2 3} 取主时钟的第 1、2、3
# 个边沿 → rise@1、fall@4、下个 rise@11：周期 10、占空比 30%。
create_clock -name master -period 10.0 -waveform {1.0 4.0} [get_ports clk]
create_generated_clock -name duty30 -source [get_ports clk] \
    -edges {1 2 3} [get_pins divider/Q]
set_input_transition 0.03 [get_ports clk]
set_input_transition 0.05 [get_ports d]
set_input_delay -clock master -max 1.0 [get_ports d]
set_input_delay -clock master -min 0.0 [get_ports d]
