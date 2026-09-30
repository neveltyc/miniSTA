read_liberty ../lib/sky130.lib
read_verilog netlist/ethernet_sky130.v
link_design eth_top
read_sdc eth.sdc

puts "=== OPENSTA SETUP ==="
report_checks -path_delay max -group_path_count 1 -digits 6
puts "=== OPENSTA HOLD ==="
report_checks -path_delay min -group_path_count 1 -digits 6
puts "=== OPENSTA SUMMARY ==="
report_worst_slack -max -digits 6
report_worst_slack -min -digits 6
exit
