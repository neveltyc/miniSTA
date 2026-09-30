# -edge_shift 给 -edges 的每个边沿再加一个偏移（可以是负数）。
# 这里 -edges {1 2 3} -edge_shift {0.5 0.2 0.3}：
#   rise = 1.0+0.5 = 1.5，fall = 4.0+0.2 = 4.2，下个 rise = 11.0+0.3 → 周期 9.8。
create_clock -name master -period 10.0 -waveform {1.0 4.0} [get_ports clk]
create_generated_clock -name shifted -source [get_ports clk] \
    -edges {1 2 3} -edge_shift {0.5 0.2 0.3} [get_pins divider/Q]
set_input_transition 0.03 [get_ports clk]
set_input_transition 0.05 [get_ports d]
set_input_delay -clock master -max 1.0 [get_ports d]
set_input_delay -clock master -min 0.0 [get_ports d]
