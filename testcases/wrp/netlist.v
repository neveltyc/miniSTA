// Dedicated wrapper cell using sky130 library cells
// Semantic signals: shift_clk, capture_en, shift_en, cfi, cti, cfo, cto
//
// Data path:
//   - shift mode  (capture_en=0): mux selects cto (FF feedback), FF shifts via SCD
//   - capture mode (capture_en=1): mux selects cfi (functional input), FF takes from D

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

  // 2-input mux: selects between functional input (cfi) and scan feedback (cto)
  //   S=0 (shift):     A0 -> cto (feedback)
  //   S=1 (capture):   A1 -> cfi (functional input)
  sky130_fd_sc_hd__mux2_1 u_mux (
      .A0(my_cto),
      .A1(my_cfi),
      .S(my_capture_en),
      .X(mux_out)
  );

  // Scan D-FF: shifts scan data when SCE active, otherwise takes from D
  sky130_fd_sc_hd__sdfxtp_1 u_sff (
      .CLK(my_shift_clk),
      .D(mux_out),
      .SCD(my_cti),
      .SCE(my_shift_en),
      .Q(my_cto)
  );

  assign my_cfo = mux_out;

endmodule