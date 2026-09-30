set here [file dirname [file normalize [info script]]]
read_liberty $here/gating_phase.lib
read_verilog $here/gating_phase.v
link_design gating_phase
read_sdc $here/gating_phase.sdc
report_checks -path_delay max -group_path_count 5 -format full_clock_expanded
report_checks -path_delay min -group_path_count 5 -format full_clock_expanded
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
