module async(input clk, input sel, input d, output q);
wire cclk, x;
XOR2 u_clk(.A(clk),.B(sel),.Y(cclk));
BUF u_data(.A(d),.Y(x)); DFF u_ff(.CLK(cclk),.D(1'b0),.RN(x),.Q(q));
endmodule
