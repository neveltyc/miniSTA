module launch(input clk, input d, output q);
wire cclk, x;
BUF u_clk (.A(clk), .Y(cclk));
DFF u_ff (.CLK(cclk), .D(d), .Q(x));
BUF u_data (.A(x), .Y(q));
endmodule
