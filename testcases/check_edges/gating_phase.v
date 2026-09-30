module gating_phase(input clk, input a, input b, output gclk); wire en; OR2 u_or(.A(a),.B(b),.Y(en)); ICG u_gate(.CLK(clk),.GATE(en),.GCLK(gclk)); endmodule
