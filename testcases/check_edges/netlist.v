module check_edges(input clk, input d, output q);
wire x;
BUF u_buf (.A(d), .Y(x));
DFF u_ff (.CLK(clk), .D(x), .Q(q));
endmodule
