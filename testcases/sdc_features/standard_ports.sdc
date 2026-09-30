# "除时钟外的输入"在 SDC 1.8 里的标准写法：SDC 没有集合减法，把对象显式列出来。
# 本项目所有用例都只写 SDC 1.8 语法。
create_clock -name shift_clk -period 20.0 [get_ports my_shift_clk]
set_input_delay -clock shift_clk 2.0 [get_ports {my_cti my_cfi my_capture_en my_shift_en}]
