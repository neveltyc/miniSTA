// 时钟树 4 级缓冲：f1/f3 挂在第一级，f2 挂在最深一级，两级之间的偏差就是
// set_ideal_network 要去掉的那部分插入延迟。
module ideal_demo (
clk,
d_in,
q_out
);

input clk;
input d_in;

output q_out;

wire c1;
wire c2;
wire c3;
wire c4;
wire n1;
wire n2;

sky130_fd_sc_hd__clkbuf_1 ct1 ( .A(clk), .X(c1) );
sky130_fd_sc_hd__clkbuf_1 ct2 ( .A(c1),  .X(c2) );
sky130_fd_sc_hd__clkbuf_1 ct3 ( .A(c2),  .X(c3) );
sky130_fd_sc_hd__clkbuf_1 ct4 ( .A(c3),  .X(c4) );

sky130_fd_sc_hd__dfxtp_1 f1 ( .D(d_in), .CLK(c1), .Q(n1) );
sky130_fd_sc_hd__dfxtp_1 f2 ( .D(n1),   .CLK(c4), .Q(n2) );
sky130_fd_sc_hd__dfxtp_1 f3 ( .D(n2),   .CLK(c1), .Q(q_out) );

endmodule
