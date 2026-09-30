# ac97_ctrl 的时序约束（msta 的 SDC 子集）
# 单位：时间一律按 ns 写，与业界 SDC 一致；msta 内部换算成 ps。

create_clock -name core_clk -period 10.0 [get_ports clk_i]

# 输入/输出相对时钟的延迟：外部逻辑送数据进来要花 3ns，出去还有 2ns 预算
set_input_delay  -clock core_clk -max 3.0 [get_ports rst_i]
set_output_delay -clock core_clk -max 2.0 [get_ports wb_ack_o]

# 其它顶层输入统一给 2ns 的外部延迟（没约束的输入端点会被标成 unconstrained）
set_input_delay  -clock core_clk -max 2.0 [get_ports bit_clk_pad_i]
set_input_delay  -clock core_clk -max 2.0 [get_ports sdata_pad_i]
set_input_delay  -clock core_clk -max 2.0 [get_ports dma_ack_i]

# 输出负载：0.005（单位跟随库的 capacitive_load_unit，sky130 是 pF）
set_load 0.005 [get_ports wb_data_o]

# 时钟不确定性（抖动 + 边缘率），setup/hold 各一档（-setup/-hold 是开关，值分开写）
set_clock_uncertainty -setup 0.3 [get_clocks core_clk]
set_clock_uncertainty -hold 0.1 [get_clocks core_clk]
