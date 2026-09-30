# all_registers -clock：只挑由该时钟树驱动的寄存器。
# 这里把 clkB 域寄存器出发的路径全部切掉，clkA 域的路径不受影响。
create_clock -name clkA -period 10.0 [get_ports clk]
create_clock -name clkB -period 20.0 [get_ports clk2]
set_input_delay -clock clkA -max 1.0 [get_ports da]
set_input_delay -clock clkA -min 0.0 [get_ports da]
set_input_delay -clock clkB -max 2.0 [get_ports db]
set_input_delay -clock clkB -min 0.0 [get_ports db]
set_output_delay -clock clkA -max 1.0 [get_ports {q1 q3 q4}]
set_output_delay -clock clkB -max 1.0 [get_ports q2]

set_false_path -from [all_registers -clock clkB]
