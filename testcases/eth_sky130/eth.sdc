# OpenCores 以太网 MAC（网表在 netlist/ethernet_sky130.v）。
# 三个时钟域：Wishbone 主机接口和两路 MII 时钟，MII 的 TX/RX 时钟来自 PHY，
# 与 Wishbone 异步。

create_clock -name wb_clk  -period 10.0 [get_ports wb_clk_i]
create_clock -name mtx_clk -period 40.0 [get_ports mtx_clk_pad_i]
create_clock -name mrx_clk -period 40.0 [get_ports mrx_clk_pad_i]

# 打开时钟传播，让两个工具都算插入延迟。
set_propagated_clock [all_clocks]

# 给时钟输入一个确定的源摆率，时钟树第一级的查表点才一致。
set_input_transition 0.05 [get_ports {wb_clk_i mtx_clk_pad_i mrx_clk_pad_i}]

set_clock_groups -asynchronous \
    -group {wb_clk} -group {mtx_clk} -group {mrx_clk}

# Wishbone 主机侧。
set_input_delay -clock wb_clk -max 2.0 [get_ports {
    wb_dat_i[*] wb_adr_i[*] wb_sel_i[*] wb_we_i wb_cyc_i wb_stb_i wb_rst_i
}]
set_input_delay -clock wb_clk -min 0.0 [get_ports {
    wb_dat_i[*] wb_adr_i[*] wb_sel_i[*] wb_we_i wb_cyc_i wb_stb_i wb_rst_i
}]
set_input_delay -clock wb_clk -max 2.0 [get_ports {
    m_wb_dat_i[*] m_wb_ack_i m_wb_err_i
}]
set_input_delay -clock wb_clk -min 0.0 [get_ports {
    m_wb_dat_i[*] m_wb_ack_i m_wb_err_i
}]
set_input_transition 0.05 [get_ports {
    wb_dat_i[*] wb_adr_i[*] wb_sel_i[*] wb_we_i wb_cyc_i wb_stb_i wb_rst_i
    m_wb_dat_i[*] m_wb_ack_i m_wb_err_i
}]
set_output_delay -clock wb_clk -max 2.0 [get_ports {
    wb_dat_o[*] wb_ack_o wb_err_o int_o
}]
set_output_delay -clock wb_clk -min 0.0 [get_ports {
    wb_dat_o[*] wb_ack_o wb_err_o int_o
}]
set_output_delay -clock wb_clk -max 2.0 [get_ports {
    m_wb_adr_o[*] m_wb_sel_o[*] m_wb_we_o m_wb_dat_o[*] m_wb_cyc_o m_wb_stb_o
}]
set_output_delay -clock wb_clk -min 0.0 [get_ports {
    m_wb_adr_o[*] m_wb_sel_o[*] m_wb_we_o m_wb_dat_o[*] m_wb_cyc_o m_wb_stb_o
}]

# MII 接收侧，由 PHY 接收时钟驱动。
set_input_delay -clock mrx_clk -max 3.0 [get_ports {
    mrxd_pad_i[*] mrxdv_pad_i mrxerr_pad_i mcoll_pad_i mcrs_pad_i
}]
set_input_delay -clock mrx_clk -min 0.0 [get_ports {
    mrxd_pad_i[*] mrxdv_pad_i mrxerr_pad_i mcoll_pad_i mcrs_pad_i
}]
set_input_transition 0.05 [get_ports {
    mrxd_pad_i[*] mrxdv_pad_i mrxerr_pad_i mcoll_pad_i mcrs_pad_i
    mtx_clk_pad_i mrx_clk_pad_i md_pad_i
}]

# MII 发送侧，由 PHY 发送时钟驱动。
set_output_delay -clock mtx_clk -max 3.0 [get_ports {
    mtxd_pad_o[*] mtxen_pad_o mtxerr_pad_o
}]
set_output_delay -clock mtx_clk -min 0.0 [get_ports {
    mtxd_pad_o[*] mtxen_pad_o mtxerr_pad_o
}]
set_output_delay -clock mtx_clk -max 3.0 [get_ports {mdc_pad_o}]
set_output_delay -clock mtx_clk -min 0.0 [get_ports {mdc_pad_o}]
set_input_delay -clock mtx_clk -max 3.0 [get_ports {md_pad_i}]
set_input_delay -clock mtx_clk -min 0.0 [get_ports {md_pad_i}]

set_load 0.05 [get_ports {
    wb_dat_o[*] m_wb_adr_o[*] m_wb_dat_o[*]
    mtxd_pad_o[*] mdc_pad_o md_pad_o
}]
