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

INVX4 u_inv ( .A(clk), .Y(clk_n) );
CLKBUF1 u_buf ( .A(clk_n), .Y(clk_nb) );
DFFPOSX1 f1 ( .D(d), .CLK(clk_nb), .Q(q) );

endmodule
