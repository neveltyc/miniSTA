// 用 sky130 库单元搭的专用 wrapper 单元（一位测试包装寄存器）。
// 信号含义：shift_clk 移位时钟，capture_en 捕获使能，shift_en 移位使能，
// cfi/cfo 功能输入/输出，cti/cto 测试（扫描链）输入/输出。
//
// 数据通路：
//   - 移位模式（capture_en=0）：mux 选 cto（触发器自身的反馈），触发器经 SCD 移入扫描数据
//   - 捕获模式（capture_en=1）：mux 选 cfi（功能输入），触发器从 D 端取数

module dedicated_wrp_cell (
    my_shift_clk,
    my_capture_en,
    my_shift_en,
    my_cfi,
    my_cfo,
    my_cti,
    my_cto
);

  input  my_shift_clk;
  input  my_capture_en;
  input  my_shift_en;
  input  my_cfi;
  input  my_cti;
  output my_cfo;
  output my_cto;

  wire mux_out;

  // 二选一 mux：在功能输入（cfi）和扫描反馈（cto）之间选择
  //   S=0（移位）：A0 -> cto（反馈）
  //   S=1（捕获）：A1 -> cfi（功能输入）
  sky130_fd_sc_hd__mux2_1 u_mux (
      .A0(my_cto),
      .A1(my_cfi),
      .S(my_capture_en),
      .X(mux_out)
  );

  // 扫描 D 触发器：SCE 有效时移入扫描数据（SCD），否则从 D 端取数
  sky130_fd_sc_hd__sdfxtp_1 u_sff (
      .CLK(my_shift_clk),
      .D(mux_out),
      .SCD(my_cti),
      .SCE(my_shift_en),
      .Q(my_cto)
  );

  assign my_cfo = mux_out;

endmodule