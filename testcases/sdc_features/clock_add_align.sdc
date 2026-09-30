# 同端口两个周期不同的时钟、不声明时钟组：跨时钟检查要在"公共周期"里找最紧的
# 边沿对。core 20 ns、slow 50 ns 时是 slow 第 1 拍（50）-> core 第 3 拍（60），差 10 ns。
create_clock -name core -period 20.0 [get_ports tau2015_clk]
create_clock -add -name slow -period 50.0 [get_ports tau2015_clk]
set_input_delay -clock core -max 5.0 [get_ports {inp1 inp2}]
set_input_delay -clock core -min 0.0 [get_ports {inp1 inp2}]
set_input_delay -clock slow -max 45.0 [get_ports {inp1 inp2}]
set_input_delay -clock slow -min 0.0 [get_ports {inp1 inp2}]
set_input_transition 0.05 [get_ports {inp1 inp2}]
set_output_delay -clock core -max 5.0 [get_ports out]
set_output_delay -clock core -min 0.0 [get_ports out]
set_output_delay -clock slow -max 5.0 [get_ports out]
set_output_delay -clock slow -min 0.0 [get_ports out]
set_load 0.05 [get_ports out]
