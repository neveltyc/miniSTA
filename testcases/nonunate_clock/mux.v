module mux(input clk, input sel, input d, output q);
wire inv, mid, cclk, x;
INV u_inv(.A(clk),.Y(inv)); MUX2 u_mux(.A(clk),.B(inv),.S(sel),.Y(mid));
BUF u_clk(.A(mid),.Y(cclk)); BUF u_data(.A(d),.Y(x)); DFF u_ff(.CLK(cclk),.D(x),.Q(q));
endmodule
