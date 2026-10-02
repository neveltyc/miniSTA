module generated_demo(input clk, input d, output q);
  wire divclk;
  sky130_fd_sc_hd__dfxtp_1 divider (.D(d), .CLK(clk), .Q(divclk));
  sky130_fd_sc_hd__dfxtp_1 sink (.D(d), .CLK(divclk), .Q(q));
endmodule
