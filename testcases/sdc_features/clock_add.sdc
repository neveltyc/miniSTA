# 同一根网络（同一个端口）上挂两个时钟：create_clock -add 之后两个时钟各自
# 传播、各自参与检查，端点 slack 取两者里最差的那个。两个时钟声明成异步，
# 所以跨这两个时钟的检查不成立（不加时钟组的写法见 clock_add_align.sdc）。
create_clock -name core -period 20.0 [get_ports tau2015_clk]
create_clock -add -name slow -period 50.0 [get_ports tau2015_clk]
set_clock_groups -asynchronous -group {core} -group {slow}
# 慢钟这一拍给得很紧（45/50），所以最差 slack 应该出自 slow 而不是 core。
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
