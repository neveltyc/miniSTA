// 时钟先过一级反相器、再过一级缓冲到 FF：FF 的有效触发沿由时钟极性决定
// （反相器自带 negative_unate，set_clock_sense 可以覆盖它或让传播停下）。
module clock_sense_demo (
clk,
d,
q
);

input clk;
input d;

output q;

wire clk_n;
wire clk_nb;

sky130_fd_sc_hd__inv_4 u_inv ( .A(clk), .Y(clk_n) );
sky130_fd_sc_hd__clkbuf_1 u_buf ( .A(clk_n), .X(clk_nb) );
sky130_fd_sc_hd__dfxtp_1 f1 ( .D(d), .CLK(clk_nb), .Q(q) );

endmodule
