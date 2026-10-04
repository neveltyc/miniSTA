set here [file dirname [file normalize [info script]]]
proc set_cppr {args} {
    if {[llength $args] == 3} {set ::sta_crpr_mode [lindex $args 1]}
    set ::sta_crpr_enabled [lindex $args end]
}
set sta_crpr_mode same_transition
set lib $here/../aocv/merge.lib
if {[info exists ::env(CPPR_LIB)]} {set lib $here/$::env(CPPR_LIB)}
read_liberty $lib
read_verilog $here/$::env(CPPR_NETLIST).v
link_design ocv
read_sdc $here/$::env(CPPR_CASE).sdc
report_checks -path_delay max -format full_clock_expanded -digits 6
report_checks -path_delay min -format full_clock_expanded -digits 6
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
