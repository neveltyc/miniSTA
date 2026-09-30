module seq_qn(input clk, input d, output q);
wire cclk, gen, x;
BUF u_clk(.A(clk),.Y(cclk));
DFF divider(.CLK(cclk),.D(d),.QN(gen));
BUF u_data(.A(d),.Y(x));
DFF sink(.CLK(gen),.D(x),.Q(q));
endmodule
