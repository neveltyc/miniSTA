read_liberty lib/ics55_LLSC_H7CL_typ_tt.lib
read_liberty lib/ics55_LLSC_H7CR_typ_tt.lib
read_liberty lib/ics55_LLSC_H7CH_typ_tt.lib
read_verilog netlist/multi_vt_soc.v
link_design soc_top
read_sdc multi_vt_soc.sdc
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
report_tns -max -digits 6
report_tns -min -digits 6
exit
