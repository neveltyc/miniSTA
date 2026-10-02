# clock_groups.sdc 的嵌套写法：-group 的列表里套集合（名字在前、集合在后，以及两层
# 嵌套），效果与 {external spare}、[get_clocks core] 相同。
set period 50.0
create_clock -name core -period $period [get_ports tau2015_clk]
create_clock -name external -period $period
create_clock -name spare -period $period
set_input_delay -max -clock external 5.0 [get_ports {inp1 inp2}]
set_input_delay -min -clock external 0.0 [get_ports {inp1 inp2}]
set_output_delay -max -clock core 30.0 [get_ports out]
set_output_delay -min -clock core -10.0 [get_ports out]
set_clock_groups -asynchronous -group [list spare [get_clocks external]] \
    -group [list [list [get_clocks core]]]
