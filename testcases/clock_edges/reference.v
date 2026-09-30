module reference(input clk, input d, output q);
wire cclk, x;
BUF u_clk (.A(clk), .Y(cclk));
BUF u_data (.A(d), .Y(x));
DFF u_ff (.CLK(clk), .D(x), .Q(q));
endmodule
