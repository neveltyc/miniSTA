# 锁存器的 D 脚相对使能脚（这里是时钟）关闭沿的检查。
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports d]
set_input_delay -clock clk -min 0.5 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
