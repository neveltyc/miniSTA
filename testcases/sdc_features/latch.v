// 透明锁存器最小用例：一个高电平锁存的 D 锁存器 + 一个反相器。
module latch_demo (
    clk,
    d,
    q
);

  input  clk;
  input  d;
  output q;

  wire latq;

  sky130_fd_sc_hd__dlxtp_1 u_lat (
      .D(d),
      .GATE(clk),
      .Q(latq)
  );
  sky130_fd_sc_hd__inv_1 u_inv (
      .A(latq),
      .Y(q)
  );

endmodule
