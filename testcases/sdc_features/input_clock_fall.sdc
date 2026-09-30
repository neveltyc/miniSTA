# set_input_delay -clock_fall：输入端口的到达时间参照时钟的下降沿（25 ns），
# 出发沿也是下降沿，hold 取不晚于它的那个捕捉沿。
create_clock -name core -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -clock_fall -max 5.0 [get_ports inp1]
set_input_delay -clock core -clock_fall -min 0.0 [get_ports inp1]
set_input_delay -clock core -max 1.0 [get_ports inp2]
set_input_delay -clock core -min 0.0 [get_ports inp2]
set_output_delay -clock core -max 30.0 [get_ports out]
set_output_delay -clock core -min -10.0 [get_ports out]
