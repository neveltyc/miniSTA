read_liberty ../lib/sky130.lib
read_verilog netlist/opene902.v
link_design openE902
read_sdc e902.sdc

puts "=== OPENSTA SETUP ==="
report_checks -path_delay max -group_path_count 1 -digits 6
puts "=== OPENSTA HOLD ==="
report_checks -path_delay min -group_path_count 1 -digits 6
puts "=== OPENSTA SUMMARY ==="
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
