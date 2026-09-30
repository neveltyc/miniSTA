// 时钟门控最小用例：一个 ICG（dlclkp_1）+ 一个被它门控的 FF + 一个反相器。
module gating_demo (
    clk,
    en,
    d,
    q
);

  input  clk;
  input  en;
  input  d;
  output q;

  wire gclk;
  wire n1;

  sky130_fd_sc_hd__dlclkp_1 u_icg (
      .CLK(clk),
      .GATE(en),
      .GCLK(gclk)
  );
  sky130_fd_sc_hd__dfxtp_1 u_ff (
      .CLK(gclk),
      .D(d),
      .Q(n1)
  );
  sky130_fd_sc_hd__inv_1 u_inv (
      .A(n1),
      .Y(q)
  );

endmodule
