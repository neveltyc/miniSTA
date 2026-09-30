# 分对象的设计规则约束：限制只作用在指定对象上；
# 库写在驱动脚上的 max_transition/max_capacitance 同时参与，取最紧的那个。
create_clock -name clk -period 20.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
set_input_delay -clock clk -min 0.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
set_output_delay -clock clk -max 2.0 [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_addr_*}]
set_input_transition 0.05 [get_ports {resetn clk gpio_in_* mem_ready mem_rdata_*}]
set_load 0.01 [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_addr_*}]

set_max_fanout 8 [get_ports resetn]
set_max_transition 0.15 [get_ports {timer_count_*}]
set_max_capacitance 0.02 [get_ports resetn]
