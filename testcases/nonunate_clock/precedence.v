module precedence(input clk, input sel, input b, input d, output q);
wire cclk, x;
BOOLCLK u_clk(.A(clk),.B(b),.S(sel),.Y(cclk));
BUF u_data(.A(d),.Y(x)); DFF u_ff(.CLK(cclk),.D(x),.Q(q));
endmodule
