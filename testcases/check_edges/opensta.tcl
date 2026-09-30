set here [file dirname [file normalize [info script]]]
set lib normal
if {[info exists ::env(CHECK_EDGE_LIB)]} { set lib $::env(CHECK_EDGE_LIB) }
read_liberty $here/$lib.lib
read_verilog $here/netlist.v
link_design check_edges
set sdc common
if {[info exists ::env(CHECK_EDGE_SDC)]} { set sdc $::env(CHECK_EDGE_SDC) }
read_sdc $here/$sdc.sdc
report_checks -path_delay max -format full_clock_expanded
report_checks -path_delay min -format full_clock_expanded
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
