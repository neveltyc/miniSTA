// 本文件改写自 OpenTimer（https://github.com/OpenTimer/OpenTimer）的 example/simple/simple.v，
// 改动：删去 NOR2X1 u4 与线网 n2，触发器 f1 由 DFFNEGX1 改为 DFFPOSX1、D 端改接 n1。
// OpenTimer 按 MIT 许可证分发，许可证全文见仓库 THIRD_PARTY_NOTICES.md。
// Copyright (c) 2018-2021 Tsung-Wei Huang and Martin D. F. Wong
module simple (
inp1,
inp2,
tau2015_clk,
out
);

// Start PIs
input inp1;
input inp2;
input tau2015_clk;

// Start POs
output out;

// Start wires
wire n1;
wire n3;
wire n4;
wire inp1;
wire inp2;
wire tau2015_clk;
wire out;

// Start cells
NAND2X1 u1 ( .A(inp1), .B(inp2), .Y(n1) );
DFFPOSX1 f1 ( .D(n1), .CLK(tau2015_clk), .Q(n3) );
INVX1 u2 ( .A(n3), .Y(n4) );
INVX2 u3 ( .A(n4), .Y(out) );

endmodule
