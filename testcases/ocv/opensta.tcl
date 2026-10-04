set here [file dirname [file normalize [info script]]]
set sta_crpr_enabled 0
if {[string match corners_* $::env(OCV_CASE)] || $::env(OCV_CASE) eq "implicit_bc_wc"} {
    read_liberty -max $here/ocv.lib
    read_liberty -min $here/fast.lib
} else {
    read_liberty $here/ocv.lib
}
set netlist ocv
if {[info exists ::env(OCV_NETLIST)]} { set netlist $::env(OCV_NETLIST) }
read_verilog $here/$netlist.v
link_design ocv
read_sdc $here/$::env(OCV_CASE).sdc
report_checks -path_delay max -format full_clock_expanded -digits 6
report_checks -path_delay min -format full_clock_expanded -digits 6
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
