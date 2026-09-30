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

CLKBUF1 ct1 ( .A(clk), .Y(c1) );
CLKBUF1 ct2 ( .A(c1),  .Y(c2) );
CLKBUF1 ct3 ( .A(c2),  .Y(c3) );
CLKBUF1 ct4 ( .A(c3),  .Y(c4) );

DFFPOSX1 f1 ( .D(d_in), .CLK(c1), .Q(n1) );
DFFPOSX1 f2 ( .D(n1),   .CLK(c4), .Q(n2) );
DFFPOSX1 f3 ( .D(n2),   .CLK(c1), .Q(q_out) );

endmodule
