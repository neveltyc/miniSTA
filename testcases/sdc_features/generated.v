module generated_demo(input clk, input d, output q);
  wire divclk;
  DFFPOSX1 divider (.D(d), .CLK(clk), .Q(divclk));
  DFFPOSX1 sink (.D(d), .CLK(divclk), .Q(q));
endmodule
