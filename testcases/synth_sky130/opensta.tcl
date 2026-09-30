read_liberty ../lib/sky130.lib
read_verilog netlist/pipe_demo.v
link_design pipe_demo
read_sdc pipe_demo.sdc

puts "=== OPENSTA SETUP ==="
report_checks -path_delay max -group_path_count 2 -digits 6
puts "=== OPENSTA HOLD ==="
report_checks -path_delay min -group_path_count 2 -digits 6
puts "=== OPENSTA SUMMARY ==="
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
report_tns -max -digits 6
report_tns -min -digits 6
exit
