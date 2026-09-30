module capture(input clk, input d, output q);
wire cclk, x;
CLKBUF u_clk (.A(clk), .Y(cclk));
BUF u_data (.A(d), .Y(x));
DFF u_ff (.CLK(cclk), .D(x), .Q(q));
endmodule
