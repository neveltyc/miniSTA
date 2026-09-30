# 多阈值网表（picorv32 走 LVT、timer8 走 HVT、gpio8 走 RVT）的约束。
create_clock -name clk -period 20.0 [get_ports clk]

set_input_delay -clock clk -max 2.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
set_input_delay -clock clk -min 0.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
# 时钟脚用 input transition，数据输入用驱动单元（会按端口负载算延迟与摆率）。
set_input_transition 0.05 [get_ports clk]
set_driving_cell -lib_cell INVX0P5H7L -input_transition_rise 0.05 -input_transition_fall 0.05 \
    [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]

set_output_delay -clock clk -max 2.0 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]
set_output_delay -clock clk -min 0.0 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]
set_load 0.01 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]

