module gate(input clk, input sel, input d, output q);
wire cclk, x; XOR2 u_clk(.A(clk),.B(sel),.Y(cclk));
BUF u_data(.A(d),.Y(x)); ICG u_gate(.CLK(cclk),.GATE(x),.GCLK(q));
endmodule
