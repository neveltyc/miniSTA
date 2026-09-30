set here [file dirname [file normalize [info script]]]
read_liberty $here/$::env(CLOCK_EDGE_LIB).lib
set netlist $::env(CLOCK_EDGE_DESIGN)
if {[info exists ::env(CLOCK_EDGE_NETLIST)]} { set netlist $::env(CLOCK_EDGE_NETLIST) }
read_verilog $here/$netlist.v
link_design $::env(CLOCK_EDGE_DESIGN)
read_sdc $here/$::env(CLOCK_EDGE_CASE).sdc
report_checks -path_delay max -format full_clock_expanded
report_checks -path_delay min -format full_clock_expanded
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
