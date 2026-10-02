// all_registers -clock 用例：两个时钟域，另有反相时钟路径上的一只下降沿 FF，
// 用来区分 -rise_clock / -fall_clock 的"有效沿"过滤。
// 下降沿 FF 用 sky130_fd_sc_hd__dfrtn_1（库里最简单的下降沿 D 触发器，只多一个异步复位脚），
// 它的异步复位 RESET_B 接常量 1，功能上就是普通的下降沿 D 触发器。
module allregs_demo (
    clk,
    clk2,
    da,
    db,
    q1,
    q2,
    q3,
    q4
);

  input  clk;
  input  clk2;
  input  da;
  input  db;
  output q1;
  output q2;
  output q3;
  output q4;

  wire n1, n2, clk_i;

  sky130_fd_sc_hd__dfxtp_1 a1 (.D(da), .CLK(clk), .Q(n1));
  sky130_fd_sc_hd__dfxtp_1 a2 (.D(n1), .CLK(clk), .Q(q1));
  sky130_fd_sc_hd__dfrtn_1 c1 (.D(da), .CLK_N(clk), .RESET_B(1'b1), .Q(q3));   // 同一个钟的下降沿
  sky130_fd_sc_hd__dfxtp_1 b1 (.D(db), .CLK(clk2), .Q(n2));
  sky130_fd_sc_hd__dfxtp_1 b2 (.D(n2), .CLK(clk2), .Q(q2));
  sky130_fd_sc_hd__inv_1 u_inv (.A(clk), .Y(clk_i));
  sky130_fd_sc_hd__dfrtn_1 c2 (.D(da), .CLK_N(clk_i), .RESET_B(1'b1), .Q(q4)); // 反相钟上的下降沿 = 有效上升沿

endmodule
