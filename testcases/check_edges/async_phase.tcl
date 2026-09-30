set here [file dirname [file normalize [info script]]]
read_liberty $here/async_phase.lib
read_verilog $here/async_phase.v
link_design repro
read_sdc $here/async_phase.sdc
report_checks -path_delay max -group_path_count 5 -format full_clock_expanded
report_checks -path_delay min -group_path_count 5 -format full_clock_expanded
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
