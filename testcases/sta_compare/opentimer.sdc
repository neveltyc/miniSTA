# 本文件改写自 OpenTimer（https://github.com/OpenTimer/OpenTimer）的 example/simple/simple.sdc，
# 改动：输入转换时间改为数据端口 0.05、时钟端口 0.03，输出负载改为 0.05，数值写成小数形式。
# OpenTimer 按 MIT 许可证分发，许可证全文见仓库 THIRD_PARTY_NOTICES.md。
# Copyright (c) 2018-2021 Tsung-Wei Huang and Martin D. F. Wong
create_clock -period 50.0 -name tau2015_clk [get_ports tau2015_clk]
set_input_delay 0.0 -min -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 25.0 -min -fall [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 0.0 -max -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_delay 25.0 -max -fall [get_ports tau2015_clk] -clock tau2015_clk

set_input_delay 0.0 -min -rise [get_ports inp1] -clock tau2015_clk
set_input_delay 0.0 -min -fall [get_ports inp1] -clock tau2015_clk
set_input_delay 5.0 -max -rise [get_ports inp1] -clock tau2015_clk
set_input_delay 5.0 -max -fall [get_ports inp1] -clock tau2015_clk
set_input_delay 0.0 -min -rise [get_ports inp2] -clock tau2015_clk
set_input_delay 0.0 -min -fall [get_ports inp2] -clock tau2015_clk
set_input_delay 1.0 -max -rise [get_ports inp2] -clock tau2015_clk
set_input_delay 1.0 -max -fall [get_ports inp2] -clock tau2015_clk

set_input_transition 0.05 -min -rise [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -min -fall [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -max -rise [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -max -fall [get_ports inp1] -clock tau2015_clk
set_input_transition 0.05 -min -rise [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -min -fall [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -max -rise [get_ports inp2] -clock tau2015_clk
set_input_transition 0.05 -max -fall [get_ports inp2] -clock tau2015_clk
set_input_transition 0.03 -min -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -min -fall [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -max -rise [get_ports tau2015_clk] -clock tau2015_clk
set_input_transition 0.03 -max -fall [get_ports tau2015_clk] -clock tau2015_clk

set_load -pin_load 0.05 [get_ports out]
set_output_delay -10.0 -min -rise [get_ports out] -clock tau2015_clk
set_output_delay -10.0 -min -fall [get_ports out] -clock tau2015_clk
set_output_delay 30.0 -max -rise [get_ports out] -clock tau2015_clk
set_output_delay 30.0 -max -fall [get_ports out] -clock tau2015_clk
