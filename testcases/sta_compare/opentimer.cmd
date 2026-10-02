read_celllib ../lib/sky130.lib
read_verilog netlist.v
read_sdc opentimer.sdc
update_timing
report_timing -max -rise -num_paths 1
report_timing -max -fall -num_paths 1
report_timing -min -rise -num_paths 1
report_timing -min -fall -num_paths 1
report_wns
report_tns
exit
