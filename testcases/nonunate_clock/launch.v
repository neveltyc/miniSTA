module launch(input clk, input sel, input d, output q);
wire cclk, x;
XOR2 u_clk(.A(clk),.B(sel),.Y(cclk)); DFF u_ff(.CLK(cclk),.D(d),.Q(x)); BUF u_data(.A(x),.Y(q));
endmodule
