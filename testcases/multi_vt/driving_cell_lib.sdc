# 多库限定：用"库名/cell 名"指定输入端驱动单元（-library + -lib_cell）。
# msta 取驱动单元的延迟与摆率，不额外建它的输入端；这条只做自身数值检查。
create_clock -name clk -period 20.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
set_input_delay -clock clk -min 0.0 [get_ports {resetn gpio_in_* mem_ready mem_rdata_*}]
set_output_delay -clock clk -max 2.0 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]
set_output_delay -clock clk -min 0.0 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]
set_load 0.01 \
    [get_ports {trap irq_out gpio_out_* timer_count_* mem_valid mem_instr mem_addr_* mem_wdata_* mem_wstrb_*}]

set_driving_cell -library ics55_LLSC_H7CL_typ_tt_1p2_25 -lib_cell INVX0P5H7L [get_ports resetn]
set_driving_cell -library ics55_LLSC_H7CH_typ_tt_1p2_25 -lib_cell BUFX0P5H7H [get_ports mem_ready]
