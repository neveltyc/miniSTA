# OpenE902 RISC-V 内核（网表在 netlist/opene902.v）。
# 内核时钟和 JTAG 调试时钟互为异步。

create_clock -name core_clk -period 5.0 [get_ports pll_core_cpuclk]
create_clock -name jtg_clk  -period 40.0 [get_ports pad_had_jtg_tclk]

set_propagated_clock [all_clocks]
set_clock_groups -asynchronous -group {core_clk} -group {jtg_clk}

set_input_transition 0.05 [get_ports {
    pll_core_cpuclk pad_had_jtg_tclk
    pad_biu_hrdata[*] pad_biu_hready pad_biu_hresp
    pad_bmu_iahbl_base[*] pad_bmu_iahbl_mask[*]
    pad_clic_int_vld[*] pad_cpu_dfs_req pad_cpu_ext_int_b pad_cpu_nmi
    pad_cpu_rst_addr[*] pad_cpu_sys_cnt[*] pad_cpu_wakeup_event
    pad_had_jtg_tms_i pad_had_rst_b
    pad_iahbl_hrdata[*] pad_iahbl_hready pad_iahbl_hresp
    pad_sysio_dbgrq_b pad_yy_gate_clk_en_b pad_yy_test_mode
}]

# 功能侧，参照内核时钟。
set_input_delay -clock core_clk -max 1.0 [get_ports {
    pad_biu_hrdata[*] pad_biu_hready pad_biu_hresp
    pad_bmu_iahbl_base[*] pad_bmu_iahbl_mask[*]
    pad_clic_int_vld[*] pad_cpu_dfs_req pad_cpu_ext_int_b pad_cpu_nmi
    pad_cpu_rst_addr[*] pad_cpu_sys_cnt[*] pad_cpu_wakeup_event
    pad_iahbl_hrdata[*] pad_iahbl_hready pad_iahbl_hresp
    pad_sysio_dbgrq_b pad_yy_gate_clk_en_b pad_yy_test_mode
}]
set_input_delay -clock core_clk -min 0.0 [get_ports {
    pad_biu_hrdata[*] pad_biu_hready pad_biu_hresp
    pad_bmu_iahbl_base[*] pad_bmu_iahbl_mask[*]
    pad_clic_int_vld[*] pad_cpu_dfs_req pad_cpu_ext_int_b pad_cpu_nmi
    pad_cpu_rst_addr[*] pad_cpu_sys_cnt[*] pad_cpu_wakeup_event
    pad_iahbl_hrdata[*] pad_iahbl_hready pad_iahbl_hresp
    pad_sysio_dbgrq_b pad_yy_gate_clk_en_b pad_yy_test_mode
}]

# 调试侧，参照 JTAG 时钟。
set_input_delay -clock jtg_clk -max 2.0 [get_ports {pad_had_jtg_tms_i pad_had_rst_b}]
set_input_delay -clock jtg_clk -min 0.0 [get_ports {pad_had_jtg_tms_i pad_had_rst_b}]

set_output_delay -clock core_clk -max 1.0 [get_ports {
    biu_pad_haddr[*] biu_pad_hburst[*] biu_pad_hprot[*] biu_pad_hsize[*]
    biu_pad_htrans[*] biu_pad_hwdata[*] biu_pad_hwrite
    iahbl_pad_haddr[*] iahbl_pad_hburst[*] iahbl_pad_hprot[*] iahbl_pad_hsize[*]
    iahbl_pad_htrans[*] iahbl_pad_hwdata[*] iahbl_pad_hwrite
    cp0_pad_mcause[*] cp0_pad_mintstatus[*] cp0_pad_mstatus[*]
    cpu_pad_dfs_ack cpu_pad_lockup cpu_pad_soft_rst
    iu_pad_gpr_data[*] iu_pad_gpr_index[*]
}]
set_output_delay -clock core_clk -min 0.0 [get_ports {
    biu_pad_haddr[*] biu_pad_hburst[*] biu_pad_hprot[*] biu_pad_hsize[*]
    biu_pad_htrans[*] biu_pad_hwdata[*] biu_pad_hwrite
    iahbl_pad_haddr[*] iahbl_pad_hburst[*] iahbl_pad_hprot[*] iahbl_pad_hsize[*]
    iahbl_pad_htrans[*] iahbl_pad_hwdata[*] iahbl_pad_hwrite
    cp0_pad_mcause[*] cp0_pad_mintstatus[*] cp0_pad_mstatus[*]
    cpu_pad_dfs_ack cpu_pad_lockup cpu_pad_soft_rst
    iu_pad_gpr_data[*] iu_pad_gpr_index[*]
}]

set_load 0.05 [get_ports {
    biu_pad_haddr[*] biu_pad_hwdata[*] iahbl_pad_haddr[*] iahbl_pad_hwdata[*]
    iu_pad_gpr_data[*]
}]
