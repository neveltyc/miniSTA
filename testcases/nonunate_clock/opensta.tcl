set here [file dirname [file normalize [info script]]]
set lib clock
if {[info exists ::env(NONUNATE_LIB)]} { set lib $::env(NONUNATE_LIB) }
read_liberty $here/$lib.lib
read_verilog $here/$::env(NONUNATE_DESIGN).v
link_design $::env(NONUNATE_DESIGN)
read_sdc $here/$::env(NONUNATE_CASE).sdc
report_checks -path_delay max -format full_clock_expanded
report_checks -path_delay min -format full_clock_expanded
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
